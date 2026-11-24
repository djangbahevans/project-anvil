#include "anvil/core/smart_select.hpp"

#include "anvil/core/brackets.hpp"
#include "anvil/core/editing.hpp"

#include <algorithm>
#include <optional>
#include <string>

namespace anvil::core {
namespace {

bool contains(const Selection& outer, const Selection& inner) {
    return outer.start() <= inner.start() && inner.end() <= outer.end();
}

bool is_lower(unsigned char c) {
    return c >= 'a' && c <= 'z';
}

bool is_upper(unsigned char c) {
    return c >= 'A' && c <= 'Z';
}

bool is_separator(unsigned char c) {
    return c == '_' || c == '-';
}

struct Blanks {
    // The offsets of the first character that is not a space or tab and of the end of the last
    // one; both are absent for a line with nothing else.
    std::optional<std::size_t> first;
    std::size_t last_end = 0;
};

Blanks blanks_of(const Text& text, std::size_t line) {
    const std::size_t begin = text.line_start(line);
    const std::string content = text.slice(begin, text.line_end(line) - begin);
    const std::size_t first = content.find_first_not_of(" \t");
    if (first == std::string::npos) {
        return {};
    }
    return {begin + first, begin + content.find_last_not_of(" \t") + 1};
}

std::size_t line_of(const Text& text, std::size_t offset) {
    return text.position_at(offset).line;
}

// The words of identifiers: `fooBar` is `foo` and `Bar`, `foo_bar` is `foo` and `bar`. The scan of
// the right half starts with the last character that the left half looked at, as VS Code's does.
std::optional<Selection> subword(const Text& text, std::size_t offset, const Selection& word) {
    const std::string chars = text.slice(word.start(), word.end() - word.start());
    const auto at = [&chars](std::ptrdiff_t index) -> unsigned char {
        return static_cast<std::size_t>(index) < chars.size()
                   ? static_cast<unsigned char>(chars[static_cast<std::size_t>(index)])
                   : 0;
    };
    const auto caret = static_cast<std::ptrdiff_t>(offset - word.start());
    std::ptrdiff_t start = caret;
    std::ptrdiff_t end = caret;
    unsigned char last = 0;
    for (; start >= 0; --start) {
        const unsigned char c = at(start);
        if ((start != caret && is_separator(c)) || (is_lower(c) && is_upper(last))) {
            break;
        }
        last = c;
    }
    ++start;
    for (; end < static_cast<std::ptrdiff_t>(chars.size()); ++end) {
        const unsigned char c = at(end);
        if ((is_upper(c) && is_lower(last)) || is_separator(c)) {
            break;
        }
        last = c;
    }
    if (start >= end) {
        return std::nullopt;
    }
    return Selection{word.start() + static_cast<std::size_t>(start),
                     word.start() + static_cast<std::size_t>(end)};
}

void add_bracket_ranges(const Text& text, const BracketPair& pair, std::vector<Selection>& out) {
    out.push_back({pair.open + 1, pair.close});
    const Selection outer{pair.open, pair.close + 1};
    out.push_back(outer);

    const std::size_t line = line_of(text, outer.start());
    if (line == line_of(text, outer.end())) {
        return;
    }
    const Blanks own = blanks_of(text, line);
    if (own.first && *own.first != outer.start()) {
        out.push_back({*own.first, outer.end()});
        out.push_back({text.line_start(line), outer.end()});
    }
    if (line > 0) {
        const Blanks above = blanks_of(text, line - 1);
        if (above.first &&
            text.position_at(*above.first).column == text.position_at(outer.start()).column) {
            out.push_back({*above.first, outer.end()});
            out.push_back({text.line_start(line - 1), outer.end()});
        }
    }
}

// Adds, between two ranges that differ in the lines they cover, the lines of the smaller one:
// trimmed, then whole.
std::vector<Selection> with_lines(const Text& text, const std::vector<Selection>& ranges) {
    std::vector<Selection> result{ranges.front()};
    for (std::size_t i = 1; i < ranges.size(); ++i) {
        const Selection& previous = ranges[i - 1];
        const Selection& current = ranges[i];
        const std::size_t first_line = line_of(text, previous.start());
        const std::size_t last_line = line_of(text, previous.end());
        if (first_line != line_of(text, current.start()) ||
            last_line != line_of(text, current.end())) {
            const Blanks first = blanks_of(text, first_line);
            const Blanks last = blanks_of(text, last_line);
            std::optional<Selection> trimmed;
            if (first.first && last.first) {
                trimmed = Selection{*first.first, last.last_end};
                if (contains(*trimmed, previous) && *trimmed != previous &&
                    contains(current, *trimmed) && *trimmed != current) {
                    result.push_back(*trimmed);
                }
            }
            const Selection whole{text.line_start(first_line), text.line_end(last_line)};
            if (contains(whole, previous) && whole != previous && whole != trimmed &&
                contains(current, whole) && whole != current) {
                result.push_back(whole);
            }
        }
        result.push_back(current);
    }
    return result;
}

} // namespace

std::vector<Selection> selection_ranges(const Text& text, std::size_t offset) {
    offset = std::min(offset, text.size());
    while (!text.is_char_boundary(offset)) {
        --offset;
    }

    std::vector<Selection> candidates;
    for (const BracketPair& pair : bracket_pairs(text.to_string())) {
        if (pair.open < offset && offset <= pair.close) {
            add_bracket_ranges(text, pair, candidates);
        }
    }
    if (const std::optional<Selection> word = word_at(text, offset)) {
        if (const std::optional<Selection> part = subword(text, offset, *word)) {
            candidates.push_back(*part);
        }
        candidates.push_back(*word);
    }
    const std::size_t line = line_of(text, offset);
    if (text.line_end(line) > text.line_start(line) && !blanks_of(text, line).first) {
        candidates.push_back({text.line_start(line), text.line_end(line)});
    }
    candidates.push_back({0, text.size()});

    std::erase_if(candidates, [offset](const Selection& range) {
        return offset < range.start() || range.end() < offset;
    });
    std::ranges::sort(candidates, [](const Selection& a, const Selection& b) {
        return a.start() != b.start() ? a.start() > b.start() : a.end() < b.end();
    });
    std::vector<Selection> chain;
    for (const Selection& range : candidates) {
        if (chain.empty() || (contains(range, chain.back()) && range != chain.back())) {
            chain.push_back(range);
        }
    }
    return with_lines(text, chain);
}

} // namespace anvil::core
