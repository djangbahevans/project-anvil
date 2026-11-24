#pragma once

#include "anvil/core/selection.hpp"
#include "anvil/core/text.hpp"

#include <cstddef>
#include <vector>

namespace anvil::core {

// The ranges that Expand Selection steps through from a caret at `offset`, smallest first, as VS
// Code's smart select builds them when no language provides ranges: each contains the one before
// it strictly, and all contain `offset`. They come from three sources.
//
//  - Words: the part of the word that a camelCase, `_` or `-` boundary delimits around the caret,
//    then the whole word, then the whole line when it holds only spaces and tabs.
//  - Brackets: for each pair of `()`, `[]` and `{}` around the caret, from the innermost, the
//    inside of the pair and then the pair with its brackets. A pair that spans several lines also
//    gives the text from the first non-blank character of its first line, and the whole of that
//    line, up to the closing bracket; and when the opening bracket is the first non-blank character
//    of its line, the same ranges that start at the line above if that line holds text.
//  - The whole text.
//
// Where a range wraps onto other lines than the one before it, the lines of the one before are
// added between them, first without their leading and trailing spaces and tabs, then whole.
//
// Every range is a selection whose anchor is its start. An offset inside a character moves back to
// its start.
std::vector<Selection> selection_ranges(const Text& text, std::size_t offset);

} // namespace anvil::core
