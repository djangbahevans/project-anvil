#pragma once

#include <QFont>
#include <QList>
#include <QString>
#include <QTextLayout>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

class QPainter;
class QPointF;

namespace anvil::ui {

// One line of text, laid out as consecutive chunks of about `chunk_length` UTF-16 code units that
// read as a single line, so that the cost of showing a part of a long line does not grow with its
// length. A chunk is laid out when something asks about it. Until then its width is an estimate, an
// `advance` for each code unit and tabs to the next stop, which is exact for a monospace font
// without wide characters; the measured width replaces it. Text that reorders, such as
// right-to-left runs, reorders within a chunk only. Columns are UTF-16 code units and x coordinates
// are relative to the start of the line.
class ChunkedLine {
public:
    struct Style {
        QFont font;
        qreal tab_stop = 0; // the distance between tab stops, from the start of the line
        qreal advance = 0;  // the expected width of a code unit
    };

    static constexpr std::size_t chunk_length = 2048;
    // The most chunks of one line whose layouts are kept; the widths of the others stay.
    static constexpr std::size_t kept_layouts = 16;

    // `formats` are sorted and do not overlap. `built`, when given, counts the QTextLayouts made.
    ChunkedLine(QString text, QList<QTextLayout::FormatRange> formats, Style style,
                std::size_t* built = nullptr, std::size_t length = chunk_length);

    const QString& text() const {
        return text_;
    }
    std::size_t size() const {
        return static_cast<std::size_t>(text_.size());
    }
    std::size_t chunk_count() const {
        return chunks_.size();
    }
    // The column where chunk `index` begins.
    std::size_t chunk_begin(std::size_t index) const {
        return chunks_[index].begin;
    }

    // The width of the line, from the estimates of the chunks that are not laid out. A line of one
    // chunk is measured.
    qreal width();
    qreal x_of(std::size_t column);
    // The column nearest to `x`.
    std::size_t column_at(qreal x);
    // The columns [first, second) of the chunks that show a part of [`from`, `to`), which are laid
    // out. Both are `size()` when there is none.
    std::pair<std::size_t, std::size_t> columns_between(qreal from, qreal to);
    // Draws the chunks that show a part of [`from`, `to`), with the start of the line at `origin`.
    // The `selections` are in columns of the line.
    void draw(QPainter& painter, const QPointF& origin,
              const QList<QTextLayout::FormatRange>& selections, qreal from, qreal to);
    // Takes the widths of the chunks that end at or before `column` from `earlier`, a layout of a
    // version of the line with the same text before it, so that the text does not move.
    void keep_widths(const ChunkedLine& earlier, std::size_t column);

private:
    struct Chunk {
        std::size_t begin = 0;
        std::size_t end = 0;
        qreal width = 0;
        // The offset from a tab stop that the layout was made for.
        qreal phase = 0;
        bool has_tab = false;
        std::unique_ptr<QTextLayout> layout;
    };

    void split(std::size_t length);
    qreal start(std::size_t index);
    std::size_t chunk_of(std::size_t column) const;
    std::size_t chunk_at(qreal x);
    // The layout of chunk `index`, made if needed, which also measures its width.
    QTextLayout& layout(std::size_t index);
    qreal phase_of(std::size_t index);
    // Calls `visit` with the index of each chunk that shows a part of [`from`, `to`), laid out.
    template <class Visit> void for_each_between(qreal from, qreal to, const Visit& visit);

    QString text_;
    QList<QTextLayout::FormatRange> formats_;
    Style style_;
    std::size_t* built_ = nullptr;
    std::vector<Chunk> chunks_;
    // starts_[i] is the x of chunk i, valid for i < valid_starts_.
    std::vector<qreal> starts_;
    std::size_t valid_starts_ = 1;
    // The indexes of the chunks that have a layout.
    std::vector<std::size_t> laid_out_;
};

} // namespace anvil::ui
