#pragma once

#include "anvil/ui/chunked_line.hpp"

#include <cstddef>
#include <map>
#include <memory>
#include <vector>

namespace anvil::ui {

// A line of the editor as it is shown: one row, or with word wrap several. Each row is laid out on
// its own, as a ChunkedLine, when something asks about it. Columns are UTF-16 code units of the
// line, and x coordinates are relative to the left edge of its rows.
class LineLayout {
public:
    // Where a row after the first begins.
    struct Break {
        std::size_t column = 0;
        // The visible column of the line there, which the tab stops of the row are counted from.
        double visible = 0;
    };
    struct Wrap {
        std::vector<Break> breaks; // by column, without repeats
        // The end of the leading spaces and tabs that the rows after the first are indented like,
        // or 0 when they are not indented.
        std::size_t indent_end = 0;
        std::size_t tab_size = 4;
    };
    // The most rows of one line whose layouts are kept.
    static constexpr std::size_t kept_rows = 256;

    // `formats` are sorted and do not overlap. `built`, when given, counts the QTextLayouts made.
    LineLayout(QString text, QList<QTextLayout::FormatRange> formats, ChunkedLine::Style style,
               Wrap wrap, std::size_t* built = nullptr);

    const QString& text() const {
        return text_;
    }
    std::size_t row_count() const {
        return wrap_.breaks.size() + 1;
    }
    // The columns [begin(row), end(row)) that the row shows.
    std::size_t begin(std::size_t row) const {
        return row == 0 ? 0 : wrap_.breaks[row - 1].column;
    }
    std::size_t end(std::size_t row) const {
        return row < wrap_.breaks.size() ? wrap_.breaks[row].column
                                         : static_cast<std::size_t>(text_.size());
    }
    // The row that shows `column`; a column where the line breaks is on the later row.
    std::size_t row_of(std::size_t column) const;
    // Where the text of the row begins: 0, or past the indent.
    qreal left(std::size_t row);
    // The text of the row, whose column 0 is begin(row) and whose x 0 is left(row).
    ChunkedLine& row(std::size_t row);
    // Where `column` is, on row row_of(column).
    qreal x_of(std::size_t column);
    // The column of row `row` nearest to `x`, which is end(row) past the end of the row.
    std::size_t column_at(std::size_t row, qreal x);

private:
    QString text_;
    QList<QTextLayout::FormatRange> formats_;
    ChunkedLine::Style style_;
    Wrap wrap_;
    std::size_t* built_ = nullptr;
    std::map<std::size_t, std::unique_ptr<ChunkedLine>> rows_;
};

} // namespace anvil::ui
