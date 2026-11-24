#pragma once

#ifdef ANVIL_TREE_SITTER

#include "anvil/core/error.hpp"
#include "anvil/core/highlighter.hpp"
#include "anvil/core/text.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <string_view>
#include <vector>

namespace anvil::core {

// Highlights a snapshot of a text with a tree-sitter grammar
// ([ADR 0010](../../../../../docs/adr/0010-syntax-highlighting.md)). The whole text is parsed when
// the highlighter is made, and the grammar's `highlights.scm` runs on the lines that `spans` is
// asked for. Only available when the build found the sources that
// `scripts/fetch-tree-sitter.sh` fetches, which is when `ANVIL_TREE_SITTER` is defined.
//
// A highlighter is for one snapshot and one thread at a time.
class TreeSitterHighlighter final : public Highlighter {
public:
    // Whether a grammar for `language_id` is built in.
    static bool supports(std::string_view language_id);

    // Fails with ErrorCode::unsupported for a language without a grammar, and with
    // ErrorCode::environment_unavailable when the grammar and the runtime do not fit each other
    // or the query does not compile.
    static std::expected<std::unique_ptr<TreeSitterHighlighter>, Error>
    create(std::string_view language_id, Text text);

    ~TreeSitterHighlighter() override;
    TreeSitterHighlighter(const TreeSitterHighlighter&) = delete;
    TreeSitterHighlighter& operator=(const TreeSitterHighlighter&) = delete;

    std::vector<HighlightSpan> spans(std::size_t line) const override;
    std::string_view root_scope() const override;
    std::uint64_t version() const override;

private:
    struct Impl;
    explicit TreeSitterHighlighter(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

} // namespace anvil::core

#endif
