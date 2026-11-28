#pragma once

#include "anvil/core/text.hpp"

#include <cstddef>

namespace anvil::core {

// How wide a level of indentation is and what it is written with.
struct Indentation {
    std::size_t tab_size = 4;
    bool insert_spaces = true;

    friend bool operator==(const Indentation&, const Indentation&) = default;
};

// VS Code's guess from the first 10000 lines of a text, ported from its `guessIndentation`: spaces
// or tabs by what more lines are indented with, and with spaces the width from 2 to 8 that most
// steps between lines have. What the text does not decide is taken from `defaults`.
Indentation guess_indentation(const Text& text, Indentation defaults);

} // namespace anvil::core
