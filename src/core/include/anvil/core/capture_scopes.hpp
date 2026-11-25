#pragma once

#include <span>
#include <string_view>

namespace anvil::core {

// One row of the table that gives a grammar's highlight query captures the TextMate-style scope
// names that color themes style.
struct CaptureScope {
    std::string_view capture;
    // The scope without the suffix of the language, which the grammar adds (`string.quoted.double`
    // for a scope of `string.quoted.double.json`).
    std::string_view scope;
    // A capture that every node of a kind has, and that gives way to any other capture of the same
    // node whatever the order of their patterns in the query.
    bool weak = false;
};

// The row for a capture name such as `string.special.key`: the one whose `capture` is the longest
// that equals the name or is a prefix of it that ends at a `.`. Null when no row matches, which
// gives the captured text no scope.
const CaptureScope* find_capture_scope(std::span<const CaptureScope> table,
                                       std::string_view capture);

// The scope of a capture name such as `string.special.key`: that of the row whose `capture` is the
// longest one that equals the name or is a prefix of it that ends at a `.`. Empty when no row
// matches, which gives the captured text no scope.
std::string_view scope_for_capture(std::span<const CaptureScope> table, std::string_view capture);

// The rows of each grammar, with the scopes of VS Code's TextMate grammar of the language.
std::span<const CaptureScope> json_capture_scopes();
std::span<const CaptureScope> c_capture_scopes();
std::span<const CaptureScope> cpp_capture_scopes();
std::span<const CaptureScope> javascript_capture_scopes();
std::span<const CaptureScope> typescript_capture_scopes();
std::span<const CaptureScope> markdown_capture_scopes();
std::span<const CaptureScope> markdown_inline_capture_scopes();

} // namespace anvil::core
