#pragma once

#include "anvil/core/document.hpp"
#include "anvil/core/line_breaks.hpp"
#include "anvil/core/text.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace anvil::core {

// The rows of a wrapped text: how many each line takes, and which row begins each line. The counts
// of all lines are computed at once, so the number of rows and the position of every line are
// exact; where the rows of a line begin is computed when asked for.
class RowMap {
public:
    RowMap(const Text& text, const WrapOptions& options);

    const WrapOptions& options() const {
        return options_;
    }
    // Computes the rows of the lines that `change` touched; `text` is the text after it. The map
    // must have been made for, or brought to, the text before the change.
    void apply(const DocumentChange& change, const Text& text);

    std::size_t line_count() const {
        return counts_.size();
    }
    std::size_t row_count() const {
        return rows_;
    }
    // The number of rows of `line`, which is less than line_count().
    std::size_t rows_of(std::size_t line) const {
        return counts_[line];
    }
    // The row that begins `line`; row_count() for a line past the end.
    std::size_t first_row(std::size_t line) const;
    // The line that row `row` is a part of; the last line for a row past the end.
    std::size_t line_of(std::size_t row) const;

    struct Row {
        std::size_t line = 0;
        // The bytes [begin, end) of the line.
        std::size_t begin = 0;
        std::size_t end = 0;
        // The columns by which the row is indented.
        std::size_t indent = 0;

        bool operator==(const Row&) const = default;
    };
    // Row `row`, which is less than row_count(), of `text`, the text the map is for. The cost grows
    // with the length of the line.
    Row row(const Text& text, std::size_t row) const;

    // The lines that rows have been computed for since the map was made, for tests.
    std::size_t lines_computed() const {
        return lines_computed_;
    }

private:
    // The lines of a block, whose first rows are kept so that a lookup adds few counts.
    static constexpr std::size_t block_lines = 256;

    // Makes block_starts_ valid up to `block`.
    void sum_to(std::size_t block) const;

    WrapOptions options_;
    std::vector<std::uint32_t> counts_;
    std::size_t rows_ = 0;
    // block_starts_[b] is the first row of line b * block_lines, for b < summed_blocks_.
    mutable std::vector<std::size_t> block_starts_;
    mutable std::size_t summed_blocks_ = 1;
    std::size_t lines_computed_ = 0;
};

} // namespace anvil::core
