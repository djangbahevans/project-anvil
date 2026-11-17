#pragma once

#include "anvil/core/error.hpp"
#include "anvil/core/text.hpp"

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

namespace anvil::core {

// The line ending style of a file. In memory text always uses "\n"; the style is applied when
// the text is written to a file.
enum class LineEnding {
    lf,
    crlf,
    cr,
};

// The characters of one line ending, and their number of UTF-16 code units.
std::string_view line_ending_text(LineEnding ending);
std::size_t line_ending_width(LineEnding ending);

// The most frequent line ending in `raw`; "\n" when there are none or the counts tie.
LineEnding detect_line_ending(std::string_view raw);

// Replaces each "\n" of normalized text with the line ending.
std::string apply_line_ending(std::string_view normalized, LineEnding ending);

struct DecodedFile {
    Text text;
    LineEnding line_ending = LineEnding::lf;
    bool has_bom = false;
};

// Reads the bytes of a UTF-8 file, with or without a byte order mark. Fails with
// ErrorCode::invalid_encoding when the bytes are not valid UTF-8, which is the cue to try
// another encoding.
std::expected<DecodedFile, Error> decode_file(std::string_view bytes);

// The bytes to write for `text`: its line breaks converted to `ending`, behind a byte order mark
// when `bom` is set.
std::string encode_file(const Text& text, LineEnding ending, bool bom);

} // namespace anvil::core
