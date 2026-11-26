#include "anvil/ui/line_layout.hpp"

#include <algorithm>
#include <cmath>

namespace anvil::ui {

LineLayout::LineLayout(QString text, QList<QTextLayout::FormatRange> formats,
                       ChunkedLine::Style style, Wrap wrap, std::size_t* built)
    : text_(std::move(text)), formats_(std::move(formats)), style_(std::move(style)),
      wrap_(std::move(wrap)), built_(built) {}

std::size_t LineLayout::row_of(std::size_t column) const {
    const auto after = std::ranges::upper_bound(wrap_.breaks, column, {}, &Break::column);
    return static_cast<std::size_t>(after - wrap_.breaks.begin());
}

qreal LineLayout::left(std::size_t row) {
    // The indent is as wide as the leading blanks are on the first row, so the rows line up.
    return row == 0 || wrap_.indent_end == 0 ? 0 : this->row(0).x_of(wrap_.indent_end);
}

ChunkedLine& LineLayout::row(std::size_t row) {
    if (const auto found = rows_.find(row); found != rows_.end()) {
        return *found->second;
    }
    const auto from = static_cast<qsizetype>(begin(row));
    const auto length = static_cast<qsizetype>(end(row)) - from;
    QList<QTextLayout::FormatRange> formats;
    auto format = std::ranges::partition_point(
        formats_, [&](const auto& range) { return range.start + range.length <= from; });
    for (; format != formats_.end() && format->start < from + length; ++format) {
        const qsizetype first = std::max<qsizetype>(format->start - from, 0);
        const qsizetype last = std::min<qsizetype>(format->start + format->length - from, length);
        formats.push_back(
            {static_cast<int>(first), static_cast<int>(last - first), format->format});
    }
    ChunkedLine::Style style = style_;
    if (row > 0) {
        const auto tab = static_cast<double>(wrap_.tab_size);
        style.origin = std::fmod(wrap_.breaks[row - 1].visible, tab) * style.tab_stop / tab;
    }
    auto made = std::make_unique<ChunkedLine>(row_count() == 1 ? text_ : text_.mid(from, length),
                                              std::move(formats), style, built_);
    if (rows_.size() >= kept_rows) {
        // The first row stays, which the indent is measured on.
        const auto lowest = std::next(rows_.begin(), rows_.begin()->first == 0 ? 1 : 0);
        const auto highest = std::prev(rows_.end());
        const auto distance = [row](std::size_t other) {
            return other > row ? other - row : row - other;
        };
        rows_.erase(distance(lowest->first) > distance(highest->first) ? lowest : highest);
    }
    return *rows_.emplace(row, std::move(made)).first->second;
}

qreal LineLayout::x_of(std::size_t column) {
    const std::size_t index = row_of(column);
    const qreal indent = left(index);
    return indent + row(index).x_of(column - begin(index));
}

std::size_t LineLayout::column_at(std::size_t row, qreal x) {
    const qreal indent = left(row);
    std::size_t column = begin(row) + this->row(row).column_at(x - indent);
    if (row + 1 < row_count() && column >= end(row) && column > begin(row)) {
        column = end(row) - 1;
        if (column > begin(row) && text_[static_cast<qsizetype>(column)].isLowSurrogate()) {
            --column;
        }
    }
    return column;
}

} // namespace anvil::ui
