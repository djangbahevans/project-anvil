#include "anvil/ui/chunked_line.hpp"

#include <QPainter>
#include <QTextBoundaryFinder>
#include <QTextOption>
#include <algorithm>
#include <cmath>

namespace anvil::ui {

namespace {

// Where a chunk that would end at `wanted` ends: at the last boundary between grapheme clusters
// near it, and at least between code points.
std::size_t cut_near(const QString& text, std::size_t begin, std::size_t wanted) {
    constexpr std::size_t reach = 64;
    const std::size_t size = static_cast<std::size_t>(text.size());
    const std::size_t from = std::max(begin, wanted - std::min(wanted, reach));
    const std::size_t to = std::min(size, wanted + reach);
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme,
                               text.constData() + static_cast<qsizetype>(from),
                               static_cast<qsizetype>(to - from));
    finder.setPosition(static_cast<qsizetype>(wanted - from));
    std::size_t cut = wanted;
    if (!finder.isAtBoundary()) {
        const qsizetype previous = finder.toPreviousBoundary();
        // The start of the window is a boundary to the finder, though not always to the text.
        if (previous > 0) {
            cut = from + static_cast<std::size_t>(previous);
        }
    }
    if (cut > begin && text[static_cast<qsizetype>(cut)].isLowSurrogate()) {
        --cut;
    }
    return cut > begin ? cut : wanted + 1;
}

} // namespace

ChunkedLine::ChunkedLine(QString text, QList<QTextLayout::FormatRange> formats, Style style,
                         std::size_t* built, std::size_t length)
    : text_(std::move(text)), formats_(std::move(formats)), style_(std::move(style)),
      built_(built) {
    split(std::max<std::size_t>(length, 2));
}

void ChunkedLine::split(std::size_t length) {
    const std::size_t total = size();
    std::size_t begin = 0;
    qreal x = 0;
    do {
        Chunk chunk;
        chunk.begin = begin;
        chunk.end = total - begin <= length ? total : cut_near(text_, begin, begin + length);
        const qreal start = x;
        const QChar* const units = text_.constData();
        for (std::size_t column = chunk.begin; column < chunk.end; ++column) {
            if (units[column] == u'\t' && style_.tab_stop > 0) {
                chunk.has_tab = true;
                x = (std::floor((style_.origin + x) / style_.tab_stop + 1e-6) + 1) *
                        style_.tab_stop -
                    style_.origin;
            } else {
                x += style_.advance;
            }
        }
        chunk.width = x - start;
        begin = chunk.end;
        chunks_.push_back(std::move(chunk));
    } while (begin < total);
    starts_.assign(chunks_.size(), 0);
}

qreal ChunkedLine::start(std::size_t index) {
    for (; valid_starts_ <= index; ++valid_starts_) {
        starts_[valid_starts_] = starts_[valid_starts_ - 1] + chunks_[valid_starts_ - 1].width;
    }
    return starts_[index];
}

std::size_t ChunkedLine::chunk_of(std::size_t column) const {
    const auto after = std::ranges::upper_bound(chunks_, column, {},
                                                [](const Chunk& chunk) { return chunk.begin; });
    return static_cast<std::size_t>(after - chunks_.begin()) - 1;
}

std::size_t ChunkedLine::chunk_at(qreal x) {
    start(chunks_.size() - 1);
    const auto after = std::ranges::upper_bound(starts_, x);
    return after == starts_.begin() ? 0 : static_cast<std::size_t>(after - starts_.begin()) - 1;
}

qreal ChunkedLine::phase_of(std::size_t index) {
    if (!chunks_[index].has_tab) {
        return 0;
    }
    const qreal phase = std::fmod(style_.origin + start(index), style_.tab_stop);
    // A start on a stop, give or take rounding, needs no stops of its own.
    return phase < 0.01 || style_.tab_stop - phase < 0.01 ? 0 : phase;
}

QTextLayout& ChunkedLine::layout(std::size_t index) {
    const qreal phase = phase_of(index);
    Chunk& chunk = chunks_[index];
    if (chunk.layout && std::abs(chunk.phase - phase) < 0.01) {
        return *chunk.layout;
    }
    const auto begin = static_cast<qsizetype>(chunk.begin);
    const auto length = static_cast<qsizetype>(chunk.end - chunk.begin);
    auto made = std::make_unique<QTextLayout>(text_.mid(begin, length), style_.font);
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    option.setTabStopDistance(style_.tab_stop);
    if (phase > 0) {
        // The stops of the line, seen from the start of the chunk. Two advances a code unit bounds
        // how far wide characters reach.
        const auto count =
            static_cast<int>(2 * static_cast<qreal>(length) * style_.advance / style_.tab_stop) +
            static_cast<int>(text_.mid(begin, length).count(u'\t')) + 2;
        QList<qreal> stops;
        stops.reserve(count);
        for (int stop = 1; stop <= count; ++stop) {
            stops.push_back(stop * style_.tab_stop - phase);
        }
        option.setTabArray(stops);
    }
    made->setTextOption(option);
    QList<QTextLayout::FormatRange> formats;
    auto format = std::ranges::partition_point(
        formats_, [&](const auto& range) { return range.start + range.length <= begin; });
    for (; format != formats_.end() && format->start < begin + length; ++format) {
        const qsizetype from = std::max<qsizetype>(format->start - begin, 0);
        const qsizetype to = std::min<qsizetype>(format->start + format->length - begin, length);
        formats.push_back({static_cast<int>(from), static_cast<int>(to - from), format->format});
    }
    // Set before the layout, since only then do font styles such as bold and italic apply.
    made->setFormats(formats);
    made->beginLayout();
    QTextLine line = made->createLine();
    if (line.isValid()) {
        line.setLineWidth(1 << 24);
        line.setPosition(QPointF(0, 0));
    }
    made->endLayout();
    if (built_) {
        ++*built_;
    }

    chunk.width = line.isValid() ? line.horizontalAdvance() : 0;
    chunk.phase = phase;
    if (!chunk.layout) {
        laid_out_.push_back(index);
    }
    chunk.layout = std::move(made);
    valid_starts_ = std::min(valid_starts_, index + 1);
    if (laid_out_.size() > kept_layouts) {
        const auto distance = [index](std::size_t other) {
            return other > index ? other - index : index - other;
        };
        const auto farthest = std::ranges::max_element(laid_out_, {}, distance);
        chunks_[*farthest].layout.reset();
        laid_out_.erase(farthest);
    }
    return *chunk.layout;
}

qreal ChunkedLine::width() {
    if (chunks_.size() == 1) {
        layout(0);
    }
    const std::size_t last = chunks_.size() - 1;
    return start(last) + chunks_[last].width;
}

qreal ChunkedLine::x_of(std::size_t column) {
    column = std::min(column, size());
    const std::size_t index = chunk_of(column);
    const QTextLine line = layout(index).lineAt(0);
    return start(index) +
           (line.isValid() ? line.cursorToX(static_cast<int>(column - chunks_[index].begin)) : 0);
}

std::size_t ChunkedLine::column_at(qreal x) {
    std::size_t index = chunk_at(x);
    QTextLine line = layout(index).lineAt(0);
    // A chunk that turns out to be narrower than its estimate leaves `x` to a later one.
    while (index + 1 < chunks_.size() && x >= start(index) + chunks_[index].width) {
        line = layout(++index).lineAt(0);
    }
    if (!line.isValid()) {
        return chunks_[index].begin;
    }
    return chunks_[index].begin +
           static_cast<std::size_t>(std::max(0, line.xToCursor(x - start(index))));
}

template <class Visit>
void ChunkedLine::for_each_between(qreal from, qreal to, const Visit& visit) {
    for (std::size_t index = chunk_at(std::max<qreal>(from, 0));
         index < chunks_.size() && start(index) < to; ++index) {
        // A chunk whose estimated end is before `from` is not laid out for nothing.
        if (start(index) + chunks_[index].width < from) {
            continue;
        }
        layout(index);
        if (start(index) + chunks_[index].width >= from) {
            visit(index);
        }
    }
}

std::pair<std::size_t, std::size_t> ChunkedLine::columns_between(qreal from, qreal to) {
    std::size_t first = size();
    std::size_t last = size();
    bool any = false;
    for_each_between(from, to, [&](std::size_t index) {
        first = any ? first : chunks_[index].begin;
        last = chunks_[index].end;
        any = true;
    });
    return {first, last};
}

void ChunkedLine::draw(QPainter& painter, const QPointF& origin,
                       const QList<QTextLayout::FormatRange>& selections, qreal from, qreal to) {
    for_each_between(from, to, [&](std::size_t index) {
        const auto begin = static_cast<qsizetype>(chunks_[index].begin);
        const auto end = static_cast<qsizetype>(chunks_[index].end);
        QList<QTextLayout::FormatRange> own;
        for (const QTextLayout::FormatRange& range : selections) {
            const qsizetype first = std::max<qsizetype>(range.start, begin);
            const qsizetype last = std::min<qsizetype>(range.start + range.length, end);
            if (first < last) {
                own.push_back({static_cast<int>(first - begin), static_cast<int>(last - first),
                               range.format});
            }
        }
        layout(index).draw(&painter, origin + QPointF(start(index), 0), own);
    });
}

void ChunkedLine::draw_in(QPainter& painter, const QPointF& origin, const QColor& color, qreal from,
                          qreal to) {
    for_each_between(from, to, [&](std::size_t index) {
        const QTextLayout& shown = layout(index);
        QTextLayout copy(shown.text(), shown.font());
        copy.setTextOption(shown.textOption());
        QList<QTextLayout::FormatRange> styles = shown.formats();
        for (QTextLayout::FormatRange& range : styles) {
            range.format.clearForeground();
            range.format.clearBackground();
        }
        copy.setFormats(styles);
        copy.beginLayout();
        if (QTextLine line = copy.createLine(); line.isValid()) {
            line.setLineWidth(1 << 24);
            line.setPosition(QPointF(0, 0));
        }
        copy.endLayout();
        painter.save();
        painter.setPen(color);
        copy.draw(&painter, origin + QPointF(start(index), 0));
        painter.restore();
    });
}

void ChunkedLine::keep_widths(const ChunkedLine& earlier, std::size_t column) {
    for (std::size_t index = 0; index < chunks_.size() && index < earlier.chunks_.size(); ++index) {
        const Chunk& before = earlier.chunks_[index];
        Chunk& chunk = chunks_[index];
        if (chunk.end > column || chunk.begin != before.begin || chunk.end != before.end) {
            break;
        }
        chunk.width = before.width;
    }
    valid_starts_ = 1;
}

} // namespace anvil::ui
