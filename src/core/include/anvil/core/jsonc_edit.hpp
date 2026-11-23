#pragma once

#include "anvil/core/error.hpp"

#include <expected>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace anvil::core {

// Changes one setting in the text of a settings file, a JSON-with-comments object, and leaves the
// rest of the text as it is: comments, order, spacing, line endings and trailing commas. `key` is
// a dotted key such as "editor.fontSize" that `Settings::find` would find, and `value` is the new
// value, or nothing to remove the key.
//
// - The property that holds the key is changed where it is, whether it is spelled flat
//   (`"editor.fontSize": 14`) or nested (`"editor": {"fontSize": 14}`). Where both exist it is the
//   one that `Settings::find` reads. Removing takes out the property, the comma that belongs to it,
//   and its line when it is alone on it, and does the same for a second spelling of the key, so
//   that the key is gone afterwards.
// - A key that is not there is added as a flat property at the end of the top-level object,
//   indented like the property before it, with a comma after that property when it has none; a
//   trailing comma the file uses is kept. Removing a key that is not there changes nothing.
// - A text with nothing in it but whitespace and comments gets an object after the comments.
//
// Fails with ErrorCode::invalid_argument when the text does not parse as JSON with comments, with
// the position of the error as `parse_jsonc` gives it, or when its top level is not an object.
std::expected<std::string, Error> edit_jsonc_property(std::string_view text, std::string_view key,
                                                      const std::optional<nlohmann::json>& value);

} // namespace anvil::core
