#include "anvil/core/indentation.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <string>

namespace anvil::core {
namespace {

constexpr std::size_t lines_looked_at = 10000;
constexpr std::size_t widest_guess = 8;

// A line and the number of tabs and spaces before its first other character.
struct Line {
    std::string text;
    std::size_t indentation = 0;
};

// Whether the UTF-16 unit `index` of the line is a space. VS Code counts a place in the line above
// by units, and that line can hold any characters before it.
bool is_space_at_unit(const std::string& line, std::size_t index) {
    std::size_t unit = 0;
    for (const char character : line) {
        const auto byte = static_cast<unsigned char>(character);
        if ((byte & 0xC0) == 0x80) {
            continue;
        }
        if (unit >= index) {
            return unit == index && byte == ' ';
        }
        unit += byte >= 0xF0 ? 2 : 1;
    }
    return false;
}

struct Difference {
    std::size_t spaces = 0;
    // The step lines `b` up under a word of the line above, which says nothing about indentation.
    bool looks_like_alignment = false;
};

// How many spaces one level takes, as the step from the indentation of `a` to that of `b` shows it.
Difference difference(const Line& a, const Line& b) {
    std::size_t common = 0;
    while (common < a.indentation && common < b.indentation && a.text[common] == b.text[common]) {
        ++common;
    }
    const auto count = [common](const Line& line, char wanted) {
        return static_cast<std::size_t>(std::count(
            line.text.begin() + static_cast<std::ptrdiff_t>(common),
            line.text.begin() + static_cast<std::ptrdiff_t>(std::max(common, line.indentation)),
            wanted));
    };
    const std::size_t a_spaces = count(a, ' ');
    const std::size_t a_tabs = count(a, '\t');
    const std::size_t b_spaces = count(b, ' ');
    const std::size_t b_tabs = count(b, '\t');
    if ((a_spaces > 0 && a_tabs > 0) || (b_spaces > 0 && b_tabs > 0)) {
        return {};
    }
    const auto distance = [](std::size_t x, std::size_t y) { return x > y ? x - y : y - x; };
    const std::size_t tabs = distance(a_tabs, b_tabs);
    const std::size_t spaces = distance(a_spaces, b_spaces);
    if (tabs == 0) {
        const bool aligned = spaces > 0 && b_spaces > 0 && b_spaces < b.text.size() &&
                             b.text[b_spaces] != ' ' && is_space_at_unit(a.text, b_spaces - 1) &&
                             a.text.ends_with(',');
        return {spaces, aligned};
    }
    return {spaces % tabs == 0 ? spaces / tabs : 0, false};
}

} // namespace

Indentation guess_indentation(const Text& text, Indentation defaults) {
    std::size_t with_tabs = 0;
    std::size_t with_spaces = 0;
    std::array<std::size_t, widest_guess + 1> steps{};
    // The latest line that held something besides blanks.
    Line previous;
    const std::size_t lines = std::min(text.line_count(), lines_looked_at);
    for (std::size_t number = 0; number < lines; ++number) {
        const std::size_t start = text.line_start(number);
        Line line{text.slice(start, text.line_end(number) - start)};
        const std::size_t content = line.text.find_first_not_of(" \t");
        if (content == std::string::npos) {
            continue;
        }
        line.indentation = content;
        const auto end = line.text.begin() + static_cast<std::ptrdiff_t>(content);
        if (std::find(line.text.begin(), end, '\t') != end) {
            ++with_tabs;
        } else if (content > 1) {
            ++with_spaces;
        }

        const Difference step = difference(previous, line);
        // An aligned line is skipped, unless it is also a level of the indentation that is
        // expected.
        if (step.looks_like_alignment &&
            !(defaults.insert_spaces && defaults.tab_size == step.spaces)) {
            continue;
        }
        if (step.spaces <= widest_guess) {
            ++steps[step.spaces];
        }
        previous = std::move(line);
    }

    Indentation guess = defaults;
    if (with_tabs != with_spaces) {
        guess.insert_spaces = with_tabs < with_spaces;
    }
    // With tabs the width on screen is the user's own choice.
    if (!guess.insert_spaces) {
        return guess;
    }
    std::size_t score = 0;
    // The even widths first, so that they win a tie.
    for (const std::size_t width : {2u, 4u, 6u, 8u, 3u, 5u, 7u}) {
        if (steps[width] > score) {
            score = steps[width];
            guess.tab_size = width;
        }
    }
    // Two wins over four when it is nearly as common, as deeper nesting in a file indented by two
    // makes steps of four.
    if (guess.tab_size == 4 && steps[4] > 0 && steps[2] > 0 && 3 * steps[2] >= 2 * steps[4]) {
        guess.tab_size = 2;
    }
    return guess;
}

} // namespace anvil::core
