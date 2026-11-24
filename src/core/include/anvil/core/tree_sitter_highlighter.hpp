#pragma once

#ifdef ANVIL_TREE_SITTER

#include "anvil/core/document.hpp"
#include "anvil/core/error.hpp"
#include "anvil/core/highlighter.hpp"
#include "anvil/core/text.hpp"
#include "anvil/core/text_buffer.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
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

    // Follows an edit of the text this highlighter describes: `new_text` is the text after the
    // change. The tree is edited and parsed again with the old tree, which reads only what the edit
    // reached, and the spans kept for a line stay unless the line was edited or lies in a range
    // whose syntax changed. The result of `spans` is that of a highlighter made from `new_text`.
    //
    // A list of changes is in the order they were applied, each described against the text the
    // ones before left (a `DocumentChange` is that). Fails with ErrorCode::invalid_argument when
    // the changes do not lead from the highlighter's text to one the size of `new_text`, and then
    // leaves the highlighter as it was.
    std::expected<void, Error> apply(const TextChange& change, const Text& new_text);
    std::expected<void, Error> apply(const DocumentChange& change, const Text& new_text);
    std::expected<void, Error> apply(std::span<const TextChange> changes, const Text& new_text);

    // Diagnostics for tests and benchmarks, counted since the highlighter was made: the bytes the
    // parser read from the text, and the times the query ran for a line.
    std::size_t bytes_read() const;
    // The tree as nested node types with their start and end rows and byte columns, which an
    // edited tree and a fresh one must agree on.
    std::string describe_tree() const;
    std::size_t line_queries() const;

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
