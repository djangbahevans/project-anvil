#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

namespace anvil::core {

// A matched pair of `()`, `[]` or `{}`, as the offsets of the two bracket characters.
struct BracketPair {
    std::size_t open = 0;
    std::size_t close = 0;

    bool operator==(const BracketPair&) const = default;
};

// The matched pairs of the text, ordered by their opening bracket (so that an enclosing pair comes
// before the pairs it contains). A closing bracket closes the nearest open bracket of its kind;
// the open brackets inside that one stay unmatched, and a closing bracket with no open bracket of
// its kind is ignored, as in VS Code's bracket pair parser. Every bracket counts: the text is not
// tokenized, so a bracket in a string or a comment is a bracket.
std::vector<BracketPair> bracket_pairs(std::string_view text);

// Where Go to Bracket moves a caret at `offset`, as in VS Code: to the other bracket of the pair
// that the caret touches (before or after a bracket), else to the closing bracket of the innermost
// pair around it, else to the next bracket of any kind, else nowhere (`offset` itself).
std::size_t jump_to_bracket(std::string_view text, std::size_t offset);

} // namespace anvil::core
