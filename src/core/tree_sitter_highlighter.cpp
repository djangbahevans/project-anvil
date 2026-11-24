#include "anvil/core/tree_sitter_highlighter.hpp"

#include "anvil/core/capture_scopes.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <tree_sitter/api.h>

extern "C" const TSLanguage* tree_sitter_json(void);

namespace anvil::core::grammar_queries {
extern const std::string_view json;
}

namespace anvil::core {

namespace {

struct Grammar {
    std::string_view language_id;
    std::string_view root_scope;
    const TSLanguage* (*language)();
    const std::string_view& highlights;
    std::span<const CaptureScope> (*scopes)();
};

const std::array<Grammar, 1>& grammars() {
    static const std::array<Grammar, 1> table = {
        Grammar{"json", "source.json", tree_sitter_json, grammar_queries::json,
                json_capture_scopes},
    };
    return table;
}

const Grammar* find_grammar(std::string_view language_id) {
    for (const Grammar& grammar : grammars()) {
        if (grammar.language_id == language_id) {
            return &grammar;
        }
    }
    return nullptr;
}

struct ParserDeleter {
    void operator()(TSParser* parser) const {
        ts_parser_delete(parser);
    }
};
struct TreeDeleter {
    void operator()(TSTree* tree) const {
        ts_tree_delete(tree);
    }
};
struct QueryDeleter {
    void operator()(TSQuery* query) const {
        ts_query_delete(query);
    }
};
struct CursorDeleter {
    void operator()(TSQueryCursor* cursor) const {
        ts_query_cursor_delete(cursor);
    }
};

const char* read_chunk(void* payload, std::uint32_t byte, TSPoint, std::uint32_t* bytes_read) {
    const std::string_view chunk = static_cast<const Text*>(payload)->chunk_at(byte);
    *bytes_read = static_cast<std::uint32_t>(chunk.size());
    return chunk.data();
}

struct Capture {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::uint32_t pattern = 0;
    std::string_view scope;
};

// Turns captures, which may nest and share ranges, into byte spans that do not overlap, where the
// innermost capture of a stretch gives it its scope and, for the same range, the pattern that
// comes first in the query. Nodes of a tree nest properly, which lets a stack do it in one pass.
std::vector<Capture> flatten(std::vector<Capture> captures) {
    std::ranges::stable_sort(captures, [](const Capture& a, const Capture& b) {
        if (a.begin != b.begin) {
            return a.begin < b.begin;
        }
        if (a.end != b.end) {
            return a.end > b.end;
        }
        return a.pattern < b.pattern;
    });

    std::vector<Capture> out;
    std::vector<Capture> open;
    std::size_t position = 0;
    const auto close_until = [&](std::size_t end, const Capture& top) {
        if (position < end) {
            out.push_back({position, end, top.pattern, top.scope});
            position = end;
        }
    };

    for (const Capture& capture : captures) {
        while (!open.empty() && open.back().end <= capture.begin) {
            close_until(open.back().end, open.back());
            open.pop_back();
        }
        if (open.empty()) {
            position = capture.begin;
        } else {
            if (open.back().begin == capture.begin && open.back().end == capture.end) {
                continue;
            }
            close_until(capture.begin, open.back());
        }
        Capture inner = capture;
        if (!open.empty()) {
            inner.end = std::min(inner.end, open.back().end);
        }
        open.push_back(inner);
    }
    while (!open.empty()) {
        close_until(open.back().end, open.back());
        open.pop_back();
    }
    return out;
}

} // namespace

struct TreeSitterHighlighter::Impl {
    Text text;
    const Grammar* grammar = nullptr;
    std::unique_ptr<TSTree, TreeDeleter> tree;
    std::unique_ptr<TSQuery, QueryDeleter> query;
    // The scope of each capture of the query, by capture index; empty for a capture the table does
    // not map.
    std::vector<std::string_view> capture_scopes;
};

bool TreeSitterHighlighter::supports(std::string_view language_id) {
    return find_grammar(language_id) != nullptr;
}

std::expected<std::unique_ptr<TreeSitterHighlighter>, Error>
TreeSitterHighlighter::create(std::string_view language_id, Text text) {
    const Grammar* grammar = find_grammar(language_id);
    if (!grammar) {
        return std::unexpected(Error{ErrorCode::unsupported,
                                     "no tree-sitter grammar for " + std::string(language_id)});
    }
    if (text.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(
            Error{ErrorCode::limit_exceeded, "text is too large for tree-sitter"});
    }

    const TSLanguage* language = grammar->language();
    std::unique_ptr<TSParser, ParserDeleter> parser(ts_parser_new());
    if (!ts_parser_set_language(parser.get(), language)) {
        return std::unexpected(Error{ErrorCode::environment_unavailable,
                                     "the " + std::string(language_id) +
                                         " grammar does not fit the tree-sitter runtime"});
    }

    std::uint32_t error_offset = 0;
    TSQueryError error_type = TSQueryErrorNone;
    std::unique_ptr<TSQuery, QueryDeleter> query(ts_query_new(
        language, grammar->highlights.data(),
        static_cast<std::uint32_t>(grammar->highlights.size()), &error_offset, &error_type));
    if (!query) {
        return std::unexpected(Error{ErrorCode::environment_unavailable,
                                     "the highlights query of " + std::string(language_id) +
                                         " is invalid at byte " + std::to_string(error_offset)});
    }

    auto impl = std::make_unique<Impl>();
    impl->text = std::move(text);
    impl->grammar = grammar;
    const TSInput input{&impl->text, read_chunk, TSInputEncodingUTF8, nullptr};
    impl->tree.reset(ts_parser_parse(parser.get(), nullptr, input));
    if (!impl->tree) {
        return std::unexpected(Error{ErrorCode::environment_unavailable, "parsing failed"});
    }
    impl->query = std::move(query);

    const std::span<const CaptureScope> table = grammar->scopes();
    const std::uint32_t captures = ts_query_capture_count(impl->query.get());
    impl->capture_scopes.reserve(captures);
    for (std::uint32_t i = 0; i < captures; ++i) {
        std::uint32_t length = 0;
        const char* name = ts_query_capture_name_for_id(impl->query.get(), i, &length);
        impl->capture_scopes.push_back(scope_for_capture(table, std::string_view(name, length)));
    }
    return std::unique_ptr<TreeSitterHighlighter>(new TreeSitterHighlighter(std::move(impl)));
}

TreeSitterHighlighter::TreeSitterHighlighter(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

TreeSitterHighlighter::~TreeSitterHighlighter() = default;

std::vector<HighlightSpan> TreeSitterHighlighter::spans(std::size_t line) const {
    const Text& text = impl_->text;
    if (line >= text.line_count()) {
        return {};
    }
    const std::size_t line_begin = text.line_start(line);
    const std::size_t line_end = text.line_end(line);
    if (line_begin == line_end) {
        return {};
    }

    std::unique_ptr<TSQueryCursor, CursorDeleter> cursor(ts_query_cursor_new());
    ts_query_cursor_set_byte_range(cursor.get(), static_cast<std::uint32_t>(line_begin),
                                   static_cast<std::uint32_t>(line_end));
    ts_query_cursor_exec(cursor.get(), impl_->query.get(), ts_tree_root_node(impl_->tree.get()));

    std::vector<Capture> captures;
    TSQueryMatch match;
    std::uint32_t index = 0;
    while (ts_query_cursor_next_capture(cursor.get(), &match, &index)) {
        const TSQueryCapture& capture = match.captures[index];
        const std::string_view scope = impl_->capture_scopes[capture.index];
        if (scope.empty()) {
            continue;
        }
        const std::size_t begin =
            std::max<std::size_t>(ts_node_start_byte(capture.node), line_begin);
        const std::size_t end = std::min<std::size_t>(ts_node_end_byte(capture.node), line_end);
        if (begin < end) {
            captures.push_back({begin - line_begin, end - line_begin, match.pattern_index, scope});
        }
    }

    const std::string text_of_line = text.line(line);
    std::size_t byte = 0;
    std::size_t column = 0;
    const auto column_of = [&](std::size_t target) {
        for (; byte < target; ++byte) {
            const auto c = static_cast<unsigned char>(text_of_line[byte]);
            if ((c & 0xC0) != 0x80) {
                column += c >= 0xF0 ? 2 : 1;
            }
        }
        return column;
    };

    std::vector<HighlightSpan> spans;
    for (const Capture& capture : flatten(std::move(captures))) {
        const std::size_t begin = column_of(capture.begin);
        spans.push_back({begin, column_of(capture.end), capture.scope});
    }
    return spans;
}

std::string_view TreeSitterHighlighter::root_scope() const {
    return impl_->grammar->root_scope;
}

std::uint64_t TreeSitterHighlighter::version() const {
    return 1;
}

} // namespace anvil::core
