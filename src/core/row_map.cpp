#include "anvil/core/row_map.hpp"

#include <algorithm>
#include <numeric>
#include <string>
#include <string_view>

namespace anvil::core {
namespace {

// Calls `visit` with each line of `text` in order, copying only the lines that cross a boundary of
// the text's storage.
template <class Visit> void for_each_line(const Text& text, const Visit& visit) {
    std::string carried;
    for (std::size_t offset = 0; offset < text.size();) {
        std::string_view chunk = text.chunk_at(offset);
        offset += chunk.size();
        for (std::size_t end = chunk.find('\n'); end != std::string_view::npos;
             end = chunk.find('\n')) {
            if (carried.empty()) {
                visit(chunk.substr(0, end));
            } else {
                carried += chunk.substr(0, end);
                visit(std::string_view(carried));
                carried.clear();
            }
            chunk.remove_prefix(end + 1);
        }
        carried += chunk;
    }
    visit(std::string_view(carried));
}

} // namespace

RowMap::RowMap(const Text& text, const WrapOptions& options) : options_(options) {
    counts_.reserve(text.line_count());
    for_each_line(text, [this](std::string_view line) {
        const std::size_t rows = count_rows(line, options_);
        counts_.push_back(static_cast<std::uint32_t>(rows));
        rows_ += rows;
    });
    lines_computed_ = counts_.size();
    block_starts_.assign(counts_.size() / block_lines + 1, 0);
}

void RowMap::apply(const DocumentChange& change, const Text& text) {
    // The changes are described one after another, but only the final text is known, so the lines
    // each one replaces are first marked with no rows and computed at the end.
    std::size_t dirty_begin = counts_.size();
    std::size_t dirty_end = 0;
    for (const TextChange& edit : change.changes) {
        const std::size_t first = edit.range.start.line;
        const std::size_t last = edit.range.end.line;
        const std::size_t removed = last - first + 1;
        const std::size_t inserted =
            static_cast<std::size_t>(std::ranges::count(edit.text, '\n')) + 1;
        const auto at = counts_.begin() + static_cast<std::ptrdiff_t>(first);
        rows_ -= std::accumulate(at, at + static_cast<std::ptrdiff_t>(removed), std::size_t{0});
        if (inserted > removed) {
            counts_.insert(at, inserted - removed, 0);
        } else {
            counts_.erase(at, at + static_cast<std::ptrdiff_t>(removed - inserted));
        }
        std::fill_n(counts_.begin() + static_cast<std::ptrdiff_t>(first), inserted, 0);
        dirty_end = dirty_end > last + 1 ? dirty_end - removed + inserted : first + inserted;
        dirty_begin = std::min(dirty_begin, first);
        summed_blocks_ = std::min(summed_blocks_, first / block_lines + 1);
    }
    for (std::size_t line = dirty_begin; line < dirty_end; ++line) {
        if (counts_[line] == 0) {
            counts_[line] = static_cast<std::uint32_t>(count_rows(text.line(line), options_));
            rows_ += counts_[line];
            ++lines_computed_;
        }
    }
    block_starts_.resize(counts_.size() / block_lines + 1);
    summed_blocks_ = std::min(summed_blocks_, block_starts_.size());
}

void RowMap::sum_to(std::size_t block) const {
    for (; summed_blocks_ <= block; ++summed_blocks_) {
        const auto begin =
            counts_.begin() + static_cast<std::ptrdiff_t>((summed_blocks_ - 1) * block_lines);
        block_starts_[summed_blocks_] =
            std::accumulate(begin, begin + static_cast<std::ptrdiff_t>(block_lines),
                            block_starts_[summed_blocks_ - 1]);
    }
}

std::size_t RowMap::first_row(std::size_t line) const {
    if (line >= counts_.size()) {
        return rows_;
    }
    const std::size_t block = line / block_lines;
    sum_to(block);
    return std::accumulate(counts_.begin() + static_cast<std::ptrdiff_t>(block * block_lines),
                           counts_.begin() + static_cast<std::ptrdiff_t>(line),
                           block_starts_[block]);
}

std::size_t RowMap::line_of(std::size_t row) const {
    if (row >= rows_) {
        return counts_.size() - 1;
    }
    while (summed_blocks_ < block_starts_.size() && block_starts_[summed_blocks_ - 1] <= row) {
        sum_to(summed_blocks_);
    }
    const auto summed = block_starts_.begin() + static_cast<std::ptrdiff_t>(summed_blocks_);
    const auto after = std::upper_bound(block_starts_.begin(), summed, row);
    const auto block = static_cast<std::size_t>(after - block_starts_.begin()) - 1;
    std::size_t line = block * block_lines;
    for (std::size_t next = block_starts_[block] + counts_[line]; next <= row;
         next += counts_[line]) {
        ++line;
    }
    return line;
}

RowMap::Row RowMap::row(const Text& text, std::size_t row) const {
    const std::size_t line = line_of(row);
    const std::string content = text.line(line);
    const LineBreaks breaks = break_line(content, options_);
    const std::size_t part = row - first_row(line);
    return {line, part == 0 ? 0 : breaks.starts[part - 1],
            part < breaks.starts.size() ? breaks.starts[part] : content.size(),
            part == 0 ? 0 : breaks.indent};
}

} // namespace anvil::core
