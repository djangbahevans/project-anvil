#pragma once

#include "anvil/core/error.hpp"

#include <expected>
#include <nlohmann/json.hpp>
#include <string_view>

namespace anvil::core {

// Parses JSON with comments (`// line` and `/* block */`) and trailing commas in objects and
// arrays: the format of VS Code's settings, keybindings and theme files. A leading byte order mark
// is skipped, and a repeated object key keeps its last value.
//
// A syntax error fails with ErrorCode::invalid_argument and a message that begins with the position
// of the error, "line L, column C: ", both counted from 1, the column in characters. Malformed
// UTF-8 is a syntax error too.
std::expected<nlohmann::json, Error> parse_jsonc(std::string_view text);

// True when `text` holds nothing but whitespace and comments, which several VS Code files accept
// as "no content".
bool is_blank_jsonc(std::string_view text);

} // namespace anvil::core
