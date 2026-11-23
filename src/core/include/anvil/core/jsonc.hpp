#pragma once

#include "anvil/core/error.hpp"

#include <expected>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace anvil::core {

// Parses JSON with comments (`// line` and `/* block */`) and trailing commas in objects and
// arrays: the format of VS Code's settings, keybindings and theme files. A leading byte order mark
// is skipped, and a repeated object key keeps its last value.
//
// A syntax error fails with ErrorCode::invalid_argument and a message that begins with the position
// of the error, "line L, column C: ", both counted from 1, the column in characters. Malformed
// UTF-8 is a syntax error too.
std::expected<nlohmann::json, Error> parse_jsonc(std::string_view text);

struct JsoncMember;

// A value of a JSON-with-comments text and where it is in it: the byte range `[begin, end)` of
// the value, counted from the start of the text (a byte order mark included).
struct JsoncNode {
    enum class Kind { object, array, string, number, boolean, null };

    Kind kind = Kind::null;
    std::size_t begin = 0;
    std::size_t end = 0;
    // The members of an object, in the order of the text, and a repeated name appears each time.
    std::vector<JsoncMember> members;
    // The elements of an array.
    std::vector<JsoncNode> elements;
    // For an object or an array, the position of its closing bracket.
    std::size_t close = 0;
};

// A property of an object: the range of its name, quotes included, its value, and the position of
// the comma that follows it, or `npos` when none does.
struct JsoncMember {
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    std::string name{};
    std::size_t name_begin = 0;
    std::size_t name_end = 0;
    JsoncNode value{};
    std::size_t comma = npos;
};

// Parses like `parse_jsonc` and gives where every value and every property name is. The same
// text fails in the same way. The text must hold a value.
std::expected<JsoncNode, Error> parse_jsonc_tree(std::string_view text);

// True when `text` holds nothing but whitespace and comments, which several VS Code files accept
// as "no content".
bool is_blank_jsonc(std::string_view text);

} // namespace anvil::core
