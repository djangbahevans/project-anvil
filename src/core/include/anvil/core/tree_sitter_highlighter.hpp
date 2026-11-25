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
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
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
    //
    // A stop requested through `stop` while the text is parsed ends the parse, which then fails.
    static std::expected<std::unique_ptr<TreeSitterHighlighter>, Error>
    create(std::string_view language_id, Text text, std::stop_token stop = {});

    // Like `create`, with `query` as the highlights query instead of the embedded one, for tests
    // and for trying a query out. Fails with ErrorCode::environment_unavailable when the query
    // does not compile for the grammar or has a predicate that is not understood.
    static std::expected<std::unique_ptr<TreeSitterHighlighter>, Error>
    create_with_query(std::string_view language_id, std::string_view query, Text text,
                      std::stop_token stop = {});

    // The names of the captures of the grammar's highlights query that have no scope, which leave
    // their text with the style of what encloses it. Empty for a language without a grammar.
    static std::vector<std::string> unmapped_captures(std::string_view language_id);

    // The root scope of the grammar of `language_id`, empty when there is none.
    static std::string_view root_scope_of(std::string_view language_id);

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
    // A stop requested through `stop` while the text is parsed ends the parse, which then fails and
    // leaves the highlighter without a tree: it has no spans, and every later `apply` fails.
    std::expected<void, Error> apply(std::span<const TextChange> changes, const Text& new_text,
                                     std::stop_token stop = {});

    // The highlighter's text and tree as they are now, as a highlighter of their own that later
    // edits do not change. It can be read on another thread than the one that goes on editing this
    // one, which is how the editor shows what a worker parsed.
    std::shared_ptr<const Highlighter> snapshot() const;

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

    // Parses the inline nodes of the tree again where their text is new, and returns the ranges
    // of the document of those. Nothing for a language without inline content.
    std::expected<std::vector<std::pair<std::size_t, std::size_t>>, Error> rebuild_inlines();

    std::unique_ptr<Impl> impl_;
};

} // namespace anvil::core

#endif
