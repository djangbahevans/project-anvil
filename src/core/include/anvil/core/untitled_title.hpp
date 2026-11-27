#pragma once

#include "anvil/core/text.hpp"

#include <optional>
#include <string>

namespace anvil::core {

// The title that VS Code gives an untitled document after its first line
// (`updateNameFromFirstLine`): the first 400 UTF-16 units of the line, without white space at its
// ends, a run of it as one space and without U+202E, cut at 40 units between grapheme clusters.
// None for a line with no word, only white space and the separators of words.
std::optional<std::string> untitled_title(const Text& text);

} // namespace anvil::core
