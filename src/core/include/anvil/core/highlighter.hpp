#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace anvil::core {

// A document larger than this many bytes is not highlighted, and neither is a line of more than
// this many UTF-16 units: parsing and styling them would cost more than the colors are worth.
inline constexpr std::size_t max_highlighted_bytes = 5 * 1024 * 1024;
inline constexpr std::size_t max_highlighted_line_units = 10000;

// A stretch of one line with a scope.
struct HighlightSpan {
    // UTF-16 columns of the line, `begin` included and `end` not.
    std::size_t begin = 0;
    std::size_t end = 0;
    // A TextMate-style dotted scope name, such as `keyword.control.cpp`. It stays valid as long as
    // the highlighter does, so that a span costs no allocation.
    std::string_view scope;
};

// What a syntax highlighter tells the editor: the spans of a line, with the scopes that a color
// theme's `tokenColors` style (`core::TokenTheme`). The interface does not say how the spans are
// found: tree-sitter ([ADR 0010](../../../../../docs/adr/0010-syntax-highlighting.md)) is the first
// implementation and a TextMate tokenizer can be a second.
class Highlighter {
public:
    virtual ~Highlighter() = default;

    // The spans of a line, sorted by `begin` and not overlapping; where scopes nest, the innermost
    // one is the span. Text that is not in a span has no syntax style. A line that is not there, or
    // that has not been parsed yet, has none.
    virtual std::vector<HighlightSpan> spans(std::size_t line) const = 0;

    // The scope of the language as a whole, such as `source.cpp`: the outermost entry of the scope
    // stack of every span.
    virtual std::string_view root_scope() const = 0;

    // Changes whenever `spans` can answer differently than it did, so that a view knows to paint
    // again.
    virtual std::uint64_t version() const = 0;
};

} // namespace anvil::core
