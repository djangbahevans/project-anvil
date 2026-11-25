#include "anvil/core/capture_scopes.hpp"

#include <array>
#include <vector>

namespace anvil::core {

namespace {

constexpr std::array json_rows = {
    CaptureScope{"string.special.key", "support.type.property-name"},
    CaptureScope{"string", "string.quoted.double"},
    CaptureScope{"escape", "constant.character.escape"},
    CaptureScope{"number", "constant.numeric"},
    CaptureScope{"constant.builtin", "constant.language"},
    CaptureScope{"comment", "comment"},
    CaptureScope{"punctuation.bracket", "punctuation.bracket"},
    CaptureScope{"punctuation.delimiter", "punctuation.separator"},
};

// The captures of the C grammar's `highlights.scm` and of Anvil's own patterns for it
// (`queries/c.scm`), with the scopes of VS Code's C grammar. Left out: `variable`, which every
// identifier has and which the theme would color as a variable wherever VS Code's grammar gives
// no scope; and `delimiter`, which themes do not style.
constexpr std::array c_rows = {
    CaptureScope{"keyword.control.directive", "keyword.control.directive"},
    CaptureScope{"keyword.control", "keyword.control"},
    CaptureScope{"keyword.operator.sizeof", "keyword.operator.sizeof"},
    CaptureScope{"keyword.other.typedef", "keyword.other.typedef"},
    CaptureScope{"keyword", "keyword.other"},
    CaptureScope{"operator", "keyword.operator"},
    CaptureScope{"storage.modifier", "storage.modifier"},
    CaptureScope{"storage.type", "storage.type"},
    CaptureScope{"entity.name.function.preprocessor", "entity.name.function.preprocessor"},
    CaptureScope{"function.special", "entity.name.function.preprocessor"},
    CaptureScope{"function", "entity.name.function"},
    CaptureScope{"type", "entity.name.type"},
    CaptureScope{"label", "entity.name.label"},
    CaptureScope{"property", "variable.other.member", true},
    CaptureScope{"constant.language", "constant.language"},
    CaptureScope{"constant.character.escape", "constant.character.escape"},
    CaptureScope{"constant", "variable.other.constant"},
    CaptureScope{"number", "constant.numeric"},
    CaptureScope{"string.quoted.other.lt-gt.include", "string.quoted.other.lt-gt.include"},
    CaptureScope{"string.quoted.single", "string.quoted.single"},
    CaptureScope{"string.quoted.double", "string.quoted.double"},
    CaptureScope{"string", "string.quoted.double"},
    CaptureScope{"comment", "comment"},
};

// The same for C++ (`queries/cpp.scm`), plus what only C++ has.
constexpr std::array cpp_extra_rows = {
    CaptureScope{"keyword.operator.new", "keyword.operator.new"},
    CaptureScope{"keyword.operator.delete", "keyword.operator.delete"},
    CaptureScope{"keyword.operator.noexcept", "keyword.operator.noexcept"},
    CaptureScope{"keyword.other.using", "keyword.other.using"},
    CaptureScope{"keyword.other.namespace", "keyword.other.namespace"},
    CaptureScope{"storage.type.modifier.access", "storage.type.modifier.access"},
    CaptureScope{"variable.language.this", "variable.language.this"},
    CaptureScope{"variable.builtin", "variable.language.this"},
    CaptureScope{"entity.name.namespace", "entity.name.namespace"},
    CaptureScope{"string.quoted.double.raw", "string.quoted.double.raw"},
};

// The captures of the JavaScript grammar's `highlights.scm` (JSX included) and of Anvil's own
// patterns for it (`queries/javascript.scm`, `javascript-jsx.scm`), with the scopes of VS Code's
// JavaScript grammar. `variable` and `property` are weak: they are on every identifier and property
// name, and give way to any other capture of the node. Left out: `punctuation.delimiter` and
// `punctuation.bracket`, which themes do not style.
constexpr std::array js_rows = {
    CaptureScope{"keyword.control", "keyword.control"},
    CaptureScope{"keyword.operator.expression", "keyword.operator.expression"},
    CaptureScope{"keyword.operator.new", "keyword.operator.new"},
    CaptureScope{"keyword", "keyword.other"},
    CaptureScope{"operator", "keyword.operator"},
    CaptureScope{"storage.modifier", "storage.modifier"},
    CaptureScope{"storage.type", "storage.type"},
    CaptureScope{"variable.language", "variable.language"},
    CaptureScope{"variable.builtin", "support.variable"},
    CaptureScope{"variable.parameter", "variable.parameter"},
    CaptureScope{"variable.other.constant", "variable.other.constant"},
    CaptureScope{"variable", "variable.other.readwrite", true},
    CaptureScope{"property", "variable.other.property", true},
    CaptureScope{"function.builtin", "support.function"},
    CaptureScope{"function", "entity.name.function"},
    CaptureScope{"entity.name.type.class", "entity.name.type.class"},
    CaptureScope{"constructor", "entity.name.type.class"},
    CaptureScope{"constant.language", "constant.language"},
    CaptureScope{"constant.character.escape", "constant.character.escape"},
    CaptureScope{"constant.builtin", "constant.language"},
    CaptureScope{"constant", "variable.other.constant"},
    CaptureScope{"number", "constant.numeric"},
    CaptureScope{"string.template", "string.template"},
    CaptureScope{"string.regexp", "string.regexp"},
    CaptureScope{"string.special", "string.regexp"},
    CaptureScope{"string", "string.quoted.double"},
    CaptureScope{"comment", "comment"},
    CaptureScope{"embedded", "meta.template.expression"},
    CaptureScope{"punctuation.definition.template-expression",
                 "punctuation.definition.template-expression"},
    CaptureScope{"punctuation.special", "punctuation.definition.template-expression"},
    CaptureScope{"punctuation.definition.tag", "punctuation.definition.tag"},
    CaptureScope{"tag", "entity.name.tag"},
    CaptureScope{"attribute", "entity.other.attribute-name"},
};

// What TypeScript adds (`queries/typescript.scm`), on top of the JavaScript rows.
constexpr std::array ts_extra_rows = {
    CaptureScope{"support.type.primitive", "support.type.primitive"},
    CaptureScope{"type.builtin", "support.type.primitive"},
    CaptureScope{"variable.other.enummember", "variable.other.enummember"},
    CaptureScope{"entity.name.type", "entity.name.type"},
    CaptureScope{"type", "entity.name.type"},
};

std::vector<CaptureScope> joined(std::span<const CaptureScope> first,
                                 std::span<const CaptureScope> second) {
    std::vector<CaptureScope> all(first.begin(), first.end());
    all.insert(all.end(), second.begin(), second.end());
    return all;
}

// The captures of the Markdown block grammar's `highlights.scm` and of Anvil's own patterns for it
// (`queries/markdown.scm`), with the scopes of VS Code's Markdown grammar. A span has one scope
// and not the stack of scopes that a TextMate grammar gives a token, so a mark that VS Code's
// grammar scopes inside a heading or a quote (`#`, `>`) has the scope of the heading or the
// quote's marker that themes style. Left out: `punctuation.special`, `punctuation.delimiter` and
// `none`, which themes do not style.
constexpr std::array markdown_rows = {
    CaptureScope{"markup.heading", "markup.heading"},
    CaptureScope{"text.title", "markup.heading"},
    CaptureScope{"punctuation.definition.list.begin", "punctuation.definition.list.begin"},
    CaptureScope{"punctuation.definition.quote.begin", "punctuation.definition.quote.begin"},
    CaptureScope{"meta.separator", "meta.separator"},
    CaptureScope{"markup.fenced_code.block", "markup.fenced_code.block"},
    CaptureScope{"markup.raw.block", "markup.raw.block"},
    CaptureScope{"text.literal", "markup.raw.block"},
    CaptureScope{"fenced_code.block.language", "fenced_code.block.language"},
    CaptureScope{"string.other.link.title", "string.other.link.title"},
    CaptureScope{"markup.underline.link", "markup.underline.link"},
    CaptureScope{"text.uri", "markup.underline.link"},
    CaptureScope{"constant.other.reference.link", "constant.other.reference.link"},
    CaptureScope{"text.reference", "constant.other.reference.link"},
    CaptureScope{"constant.character.escape", "constant.character.escape"},
    CaptureScope{"string.escape", "constant.character.escape"},
};

// The same for the inline grammar (`queries/markdown-inline.scm`): what is inside a paragraph, a
// heading or a table cell.
constexpr std::array markdown_inline_rows = {
    CaptureScope{"markup.inline.raw.string", "markup.inline.raw.string"},
    CaptureScope{"text.literal", "markup.inline.raw.string"},
    CaptureScope{"markup.italic", "markup.italic"},
    CaptureScope{"text.emphasis", "markup.italic"},
    CaptureScope{"markup.bold", "markup.bold"},
    CaptureScope{"text.strong", "markup.bold"},
    CaptureScope{"markup.strikethrough", "markup.strikethrough"},
    CaptureScope{"string.other.link.description", "string.other.link.description"},
    CaptureScope{"text.reference", "string.other.link.description"},
    CaptureScope{"markup.underline.link", "markup.underline.link"},
    CaptureScope{"text.uri", "markup.underline.link"},
    CaptureScope{"string.other.link.title", "string.other.link.title"},
    CaptureScope{"constant.other.reference.link", "constant.other.reference.link"},
    CaptureScope{"constant.character.escape", "constant.character.escape"},
    CaptureScope{"string.escape", "constant.character.escape"},
};

bool names(std::string_view row, std::string_view capture) {
    return capture.starts_with(row) && (capture.size() == row.size() || capture[row.size()] == '.');
}

} // namespace

const CaptureScope* find_capture_scope(std::span<const CaptureScope> table,
                                       std::string_view capture) {
    const CaptureScope* best = nullptr;
    for (const CaptureScope& row : table) {
        if (names(row.capture, capture) && (!best || row.capture.size() > best->capture.size())) {
            best = &row;
        }
    }
    return best;
}

std::string_view scope_for_capture(std::span<const CaptureScope> table, std::string_view capture) {
    const CaptureScope* row = find_capture_scope(table, capture);
    return row ? row->scope : std::string_view{};
}

std::span<const CaptureScope> json_capture_scopes() {
    return json_rows;
}

std::span<const CaptureScope> c_capture_scopes() {
    return c_rows;
}

std::span<const CaptureScope> cpp_capture_scopes() {
    static const std::vector<CaptureScope> rows = joined(cpp_extra_rows, c_rows);
    return rows;
}

std::span<const CaptureScope> javascript_capture_scopes() {
    return js_rows;
}

std::span<const CaptureScope> markdown_capture_scopes() {
    return markdown_rows;
}

std::span<const CaptureScope> markdown_inline_capture_scopes() {
    return markdown_inline_rows;
}

std::span<const CaptureScope> typescript_capture_scopes() {
    static const std::vector<CaptureScope> rows = joined(ts_extra_rows, js_rows);
    return rows;
}

} // namespace anvil::core
