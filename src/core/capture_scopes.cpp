#include "anvil/core/capture_scopes.hpp"

#include <array>

namespace anvil::core {

namespace {

constexpr std::array json_rows = {
    CaptureScope{"string.special.key", "support.type.property-name.json"},
    CaptureScope{"string", "string.quoted.double.json"},
    CaptureScope{"escape", "constant.character.escape.json"},
    CaptureScope{"number", "constant.numeric.json"},
    CaptureScope{"constant.builtin", "constant.language.json"},
    CaptureScope{"comment", "comment.json"},
    CaptureScope{"punctuation.bracket", "punctuation.bracket.json"},
    CaptureScope{"punctuation.delimiter", "punctuation.separator.json"},
};

bool names(std::string_view row, std::string_view capture) {
    return capture.starts_with(row) && (capture.size() == row.size() || capture[row.size()] == '.');
}

} // namespace

std::string_view scope_for_capture(std::span<const CaptureScope> table, std::string_view capture) {
    const CaptureScope* best = nullptr;
    for (const CaptureScope& row : table) {
        if (names(row.capture, capture) && (!best || row.capture.size() > best->capture.size())) {
            best = &row;
        }
    }
    return best ? best->scope : std::string_view{};
}

std::span<const CaptureScope> json_capture_scopes() {
    return json_rows;
}

} // namespace anvil::core
