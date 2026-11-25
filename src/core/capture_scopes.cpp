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

// The captures of the C grammar's `highlights.scm` and of Anvil's own patterns for it
// (`queries/c.scm`), with the scopes of VS Code's C grammar. Left out: `variable`, which every
// identifier has and which the theme would color as a variable wherever VS Code's grammar gives
// no scope; and `delimiter`, which themes do not style.
constexpr std::array c_rows = {
    CaptureScope{"keyword.control.directive", "keyword.control.directive.c"},
    CaptureScope{"keyword.control", "keyword.control.c"},
    CaptureScope{"keyword.operator.sizeof", "keyword.operator.sizeof.c"},
    CaptureScope{"keyword.other.typedef", "keyword.other.typedef.c"},
    CaptureScope{"keyword", "keyword.other.c"},
    CaptureScope{"operator", "keyword.operator.c"},
    CaptureScope{"storage.modifier", "storage.modifier.c"},
    CaptureScope{"storage.type", "storage.type.c"},
    CaptureScope{"entity.name.function.preprocessor", "entity.name.function.preprocessor.c"},
    CaptureScope{"function.special", "entity.name.function.preprocessor.c"},
    CaptureScope{"function", "entity.name.function.c"},
    CaptureScope{"type", "entity.name.type.c"},
    CaptureScope{"label", "entity.name.label.c"},
    CaptureScope{"property", "variable.other.member.c"},
    CaptureScope{"constant.language", "constant.language.c"},
    CaptureScope{"constant.character.escape", "constant.character.escape.c"},
    CaptureScope{"constant", "variable.other.constant.c"},
    CaptureScope{"number", "constant.numeric.c"},
    CaptureScope{"string.quoted.other.lt-gt.include", "string.quoted.other.lt-gt.include.c"},
    CaptureScope{"string.quoted.single", "string.quoted.single.c"},
    CaptureScope{"string.quoted.double", "string.quoted.double.c"},
    CaptureScope{"string", "string.quoted.double.c"},
    CaptureScope{"comment", "comment.c"},
};

// The same for C++ (`queries/cpp.scm`), plus what only C++ has.
constexpr std::array cpp_rows = {
    CaptureScope{"keyword.operator.new", "keyword.operator.new.cpp"},
    CaptureScope{"keyword.operator.delete", "keyword.operator.delete.cpp"},
    CaptureScope{"keyword.operator.noexcept", "keyword.operator.noexcept.cpp"},
    CaptureScope{"keyword.other.using", "keyword.other.using.cpp"},
    CaptureScope{"keyword.other.namespace", "keyword.other.namespace.cpp"},
    CaptureScope{"storage.type.modifier.access", "storage.type.modifier.access.cpp"},
    CaptureScope{"variable.language.this", "variable.language.this.cpp"},
    CaptureScope{"variable.builtin", "variable.language.this.cpp"},
    CaptureScope{"entity.name.namespace", "entity.name.namespace.cpp"},
    CaptureScope{"string.quoted.double.raw", "string.quoted.double.raw.cpp"},
    CaptureScope{"keyword.control.directive", "keyword.control.directive.cpp"},
    CaptureScope{"keyword.control", "keyword.control.cpp"},
    CaptureScope{"keyword.operator.sizeof", "keyword.operator.sizeof.cpp"},
    CaptureScope{"keyword.other.typedef", "keyword.other.typedef.cpp"},
    CaptureScope{"keyword", "keyword.other.cpp"},
    CaptureScope{"operator", "keyword.operator.cpp"},
    CaptureScope{"storage.modifier", "storage.modifier.cpp"},
    CaptureScope{"storage.type", "storage.type.cpp"},
    CaptureScope{"entity.name.function.preprocessor", "entity.name.function.preprocessor.cpp"},
    CaptureScope{"function.special", "entity.name.function.preprocessor.cpp"},
    CaptureScope{"function", "entity.name.function.cpp"},
    CaptureScope{"type", "entity.name.type.cpp"},
    CaptureScope{"label", "entity.name.label.cpp"},
    CaptureScope{"property", "variable.other.member.cpp"},
    CaptureScope{"constant.language", "constant.language.cpp"},
    CaptureScope{"constant.character.escape", "constant.character.escape.cpp"},
    CaptureScope{"constant", "variable.other.constant.cpp"},
    CaptureScope{"number", "constant.numeric.cpp"},
    CaptureScope{"string.quoted.other.lt-gt.include", "string.quoted.other.lt-gt.include.cpp"},
    CaptureScope{"string.quoted.single", "string.quoted.single.cpp"},
    CaptureScope{"string.quoted.double", "string.quoted.double.cpp"},
    CaptureScope{"string", "string.quoted.double.cpp"},
    CaptureScope{"comment", "comment.cpp"},
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

std::span<const CaptureScope> c_capture_scopes() {
    return c_rows;
}

std::span<const CaptureScope> cpp_capture_scopes() {
    return cpp_rows;
}

} // namespace anvil::core
