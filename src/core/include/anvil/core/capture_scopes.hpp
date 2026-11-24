#pragma once

#include <span>
#include <string_view>

namespace anvil::core {

// One row of the table that gives a grammar's highlight query captures the TextMate-style scope
// names that color themes style.
struct CaptureScope {
    std::string_view capture;
    std::string_view scope;
};

// The scope of a capture name such as `string.special.key`: that of the row whose `capture` is the
// longest one that equals the name or is a prefix of it that ends at a `.`. Empty when no row
// matches, which gives the captured text no scope.
std::string_view scope_for_capture(std::span<const CaptureScope> table, std::string_view capture);

// The rows of each grammar. The scope names carry the language's suffix, as the scopes of its
// TextMate grammar do.
std::span<const CaptureScope> json_capture_scopes();

} // namespace anvil::core
