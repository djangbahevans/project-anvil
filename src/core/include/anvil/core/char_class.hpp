#pragma once

#include <cstddef>
#include <string_view>

namespace anvil::core {

enum class CharClass {
    space,
    word,
    punctuation,
};

// The classes that word motion, word selection and whole-word search agree on. Letters, digits,
// the underscore and every non-ASCII character are word characters; tab, space and line break are
// space; the remaining ASCII characters are punctuation.
CharClass classify(char32_t c);

inline bool is_word_character(char32_t c) {
    return classify(c) == CharClass::word;
}

// The code point whose encoding starts at byte `offset` of `utf8`, which must be valid UTF-8 and
// start on a character boundary before the end.
char32_t decode_code_point(std::string_view utf8, std::size_t offset);

} // namespace anvil::core
