#include "anvil/core/tree_sitter_highlighter.hpp"

#include "anvil/core/capture_scopes.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include <tree_sitter/api.h>
#include <unordered_map>

extern "C" const TSLanguage* tree_sitter_json(void);
extern "C" const TSLanguage* tree_sitter_c(void);
extern "C" const TSLanguage* tree_sitter_cpp(void);
extern "C" const TSLanguage* tree_sitter_javascript(void);
extern "C" const TSLanguage* tree_sitter_typescript(void);
extern "C" const TSLanguage* tree_sitter_tsx(void);

namespace anvil::core::grammar_queries {
extern const std::string_view json;
extern const std::string_view c;
extern const std::string_view cpp;
extern const std::string_view javascript;
extern const std::string_view typescript;
extern const std::string_view typescriptreact;
} // namespace anvil::core::grammar_queries

namespace anvil::core {

namespace {

struct Grammar {
    std::string_view language_id;
    std::string_view root_scope;
    // What ends the scopes of the language, as the scopes of its TextMate grammar do: `json`,
    // `js.jsx`.
    std::string_view scope_suffix;
    const TSLanguage* (*language)();
    const std::string_view& highlights;
    std::span<const CaptureScope> (*scopes)();
};

const std::array<Grammar, 7>& grammars() {
    static const std::array<Grammar, 7> table = {
        Grammar{"json", "source.json", "json", tree_sitter_json, grammar_queries::json,
                json_capture_scopes},
        Grammar{"c", "source.c", "c", tree_sitter_c, grammar_queries::c, c_capture_scopes},
        Grammar{"cpp", "source.cpp", "cpp", tree_sitter_cpp, grammar_queries::cpp,
                cpp_capture_scopes},
        Grammar{"javascript", "source.js", "js", tree_sitter_javascript,
                grammar_queries::javascript, javascript_capture_scopes},
        Grammar{"javascriptreact", "source.js.jsx", "js.jsx", tree_sitter_javascript,
                grammar_queries::javascript, javascript_capture_scopes},
        Grammar{"typescript", "source.ts", "ts", tree_sitter_typescript,
                grammar_queries::typescript, typescript_capture_scopes},
        Grammar{"typescriptreact", "source.tsx", "tsx", tree_sitter_tsx,
                grammar_queries::typescriptreact, typescript_capture_scopes},
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

struct ReadState {
    const Text* text = nullptr;
    std::size_t* bytes_read = nullptr;
};

const char* read_chunk(void* payload, std::uint32_t byte, TSPoint, std::uint32_t* bytes_read) {
    const auto* state = static_cast<const ReadState*>(payload);
    const std::string_view chunk = state->text->chunk_at(byte);
    *bytes_read = static_cast<std::uint32_t>(chunk.size());
    *state->bytes_read += chunk.size();
    return chunk.data();
}

// The row and the byte column that tree-sitter's points use, not the UTF-16 column.
void describe(TSNode node, std::string& out) {
    const TSPoint start = ts_node_start_point(node);
    const TSPoint end = ts_node_end_point(node);
    out += std::string(ts_node_type(node)) + "[" + std::to_string(start.row) + ":" +
           std::to_string(start.column) + "-" + std::to_string(end.row) + ":" +
           std::to_string(end.column) + "]";
    const std::uint32_t children = ts_node_child_count(node);
    if (children == 0) {
        return;
    }
    out += "(";
    for (std::uint32_t i = 0; i < children; ++i) {
        describe(ts_node_child(node, i), out);
    }
    out += ")";
}

TSPoint point_at(const Text& text, std::size_t offset) {
    const std::size_t row = text.position_at(offset).line;
    return {static_cast<std::uint32_t>(row),
            static_cast<std::uint32_t>(offset - text.line_start(row))};
}

bool parse_cancelled(TSParseState* state) {
    return static_cast<const std::atomic<bool>*>(state->payload)->load();
}

TSTree* parse(TSParser& parser, const TSTree* old_tree, const TSInput& input,
              std::atomic<bool>& cancel) {
    return ts_parser_parse_with_options(&parser, old_tree, input,
                                        TSParseOptions{&cancel, parse_cancelled});
}

struct Capture {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::uint32_t pattern = 0;
    std::string_view scope;
    bool weak = false;
};

// Turns captures, which may nest and share ranges, into byte spans that do not overlap, where the
// innermost capture of a stretch gives it its scope and, for the same range, the pattern that
// comes first in the query, a weak capture after any other. Nodes of a tree nest properly, which
// lets a stack do it in one pass.
std::vector<Capture> flatten(std::vector<Capture> captures) {
    std::ranges::stable_sort(captures, [](const Capture& a, const Capture& b) {
        if (a.begin != b.begin) {
            return a.begin < b.begin;
        }
        if (a.end != b.end) {
            return a.end > b.end;
        }
        if (a.weak != b.weak) {
            return b.weak;
        }
        return a.pattern < b.pattern;
    });

    std::vector<Capture> out;
    std::vector<Capture> open;
    std::size_t position = 0;
    const auto close_until = [&](std::size_t end, const Capture& top) {
        if (position < end) {
            out.push_back({position, end, top.pattern, top.scope, top.weak});
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

// What the query of a grammar says about its captures, shared by the highlighter that parses and
// the snapshots that the editor reads, which run it concurrently.
struct RegexDeleter {
    void operator()(pcre2_code* code) const {
        pcre2_code_free(code);
    }
};

// A condition of a pattern on the text it captured, which the query cursor leaves to the caller:
// the match counts only when it holds.
// - `(#match? @capture "regex")` and `(#not-match? @capture "regex")`
// - `(#eq? @capture "text")` and `(#not-eq? @capture "text")`
// - `(#is? local)` never holds and `(#is-not? local)` always does, because the highlighter does
//   not track which identifiers are local variables.
struct TextPredicate {
    enum class Kind { regex, text, never };
    Kind kind = Kind::regex;
    std::uint32_t capture = 0;
    bool negated = false;
    std::shared_ptr<pcre2_code> regex;
    std::string text;
};

struct QueryData {
    std::unique_ptr<TSQuery, QueryDeleter> query;
    // The predicates of each pattern, by pattern index.
    std::vector<std::vector<TextPredicate>> predicates;
    // The scope of each capture of the query, by capture index, with the suffix of the language;
    // empty for a capture the table does not map. The views are into `scope_names`.
    std::vector<std::string_view> capture_scopes;
    std::vector<bool> capture_weak;
    std::vector<std::string> scope_names;
};

// Reads the predicates of every pattern of the query. Fails with a message for a predicate that is
// not understood, because a pattern whose condition is ignored would match what it should not.
std::expected<std::vector<std::vector<TextPredicate>>, std::string>
read_predicates(TSQuery& query) {
    std::vector<std::vector<TextPredicate>> all(ts_query_pattern_count(&query));
    for (std::uint32_t pattern = 0; pattern < all.size(); ++pattern) {
        std::uint32_t count = 0;
        const TSQueryPredicateStep* steps =
            ts_query_predicates_for_pattern(&query, pattern, &count);
        const auto string_of = [&](const TSQueryPredicateStep& step) {
            std::uint32_t length = 0;
            const char* value = ts_query_string_value_for_id(&query, step.value_id, &length);
            return std::string(value, length);
        };
        for (std::uint32_t begin = 0; begin < count;) {
            std::uint32_t end = begin;
            while (end < count && steps[end].type != TSQueryPredicateStepTypeDone) {
                ++end;
            }
            const std::string name = string_of(steps[begin]);
            const std::uint32_t arguments = end - begin - 1;
            const auto capture_and_string = [&] {
                return arguments == 2 && steps[begin + 1].type == TSQueryPredicateStepTypeCapture &&
                       steps[begin + 2].type == TSQueryPredicateStepTypeString;
            };
            if (name == "match?" || name == "not-match?") {
                if (!capture_and_string()) {
                    return std::unexpected("unsupported use of #" + name);
                }
                const std::string source = string_of(steps[begin + 2]);
                int error_code = 0;
                PCRE2_SIZE error_offset = 0;
                pcre2_code* code =
                    pcre2_compile(reinterpret_cast<PCRE2_SPTR>(source.data()), source.size(),
                                  PCRE2_UTF, &error_code, &error_offset, nullptr);
                if (!code) {
                    return std::unexpected("invalid regular expression in #" + name + ": " +
                                           source);
                }
                all[pattern].push_back({TextPredicate::Kind::regex,
                                        steps[begin + 1].value_id,
                                        name == "not-match?",
                                        std::shared_ptr<pcre2_code>(code, RegexDeleter{}),
                                        {}});
            } else if (name == "eq?" || name == "not-eq?") {
                if (!capture_and_string()) {
                    return std::unexpected("unsupported use of #" + name);
                }
                all[pattern].push_back({TextPredicate::Kind::text, steps[begin + 1].value_id,
                                        name == "not-eq?", nullptr, string_of(steps[begin + 2])});
            } else if (name == "is?" || name == "is-not?") {
                if (arguments != 1 || steps[begin + 1].type != TSQueryPredicateStepTypeString ||
                    string_of(steps[begin + 1]) != "local") {
                    return std::unexpected("unsupported use of #" + name);
                }
                if (name == "is?") {
                    all[pattern].push_back({TextPredicate::Kind::never, 0, false, nullptr, {}});
                }
            } else if (name.ends_with('?')) {
                return std::unexpected("unsupported predicate #" + name);
            }
            begin = end + 1;
        }
    }
    return all;
}

// Whether the captures of `match` satisfy the predicates of its pattern.
bool satisfies(const QueryData& data, const TSQueryMatch& match, const Text& text,
               pcre2_match_data* scratch) {
    for (const TextPredicate& predicate : data.predicates[match.pattern_index]) {
        if (predicate.kind == TextPredicate::Kind::never) {
            return false;
        }
        for (std::uint16_t i = 0; i < match.capture_count; ++i) {
            const TSQueryCapture& capture = match.captures[i];
            if (capture.index != predicate.capture) {
                continue;
            }
            const std::size_t begin = ts_node_start_byte(capture.node);
            const std::string subject = text.slice(begin, ts_node_end_byte(capture.node) - begin);
            const bool holds = predicate.kind == TextPredicate::Kind::text
                                   ? subject == predicate.text
                                   : pcre2_match(predicate.regex.get(),
                                                 reinterpret_cast<PCRE2_SPTR>(subject.data()),
                                                 subject.size(), 0, 0, scratch, nullptr) >= 0;
            if (holds == predicate.negated) {
                return false;
            }
        }
    }
    return true;
}

constexpr std::size_t no_line_limit = std::numeric_limits<std::size_t>::max();

// The spans of `line`, from the query run on its byte range. A line of more than `max_units`
// UTF-16 units has none.
std::vector<HighlightSpan> line_spans(const Text& text, const TSTree& tree, const QueryData& data,
                                      std::size_t line, std::size_t max_units) {
    if (line >= text.line_count()) {
        return {};
    }
    const std::size_t line_begin = text.line_start(line);
    const std::size_t line_end = text.line_end(line);
    if (line_begin == line_end) {
        return {};
    }
    if (line_end - line_begin > max_units &&
        text.utf16_offset(line_end) - text.utf16_offset(line_begin) > max_units) {
        return {};
    }

    std::unique_ptr<TSQueryCursor, CursorDeleter> cursor(ts_query_cursor_new());
    ts_query_cursor_set_byte_range(cursor.get(), static_cast<std::uint32_t>(line_begin),
                                   static_cast<std::uint32_t>(line_end));
    ts_query_cursor_exec(cursor.get(), data.query.get(), ts_tree_root_node(&tree));

    std::vector<Capture> captures;
    TSQueryMatch match;
    std::uint32_t index = 0;
    const std::unique_ptr<pcre2_match_data, void (*)(pcre2_match_data*)> scratch(
        pcre2_match_data_create(1, nullptr), pcre2_match_data_free);
    while (ts_query_cursor_next_capture(cursor.get(), &match, &index)) {
        if (!data.predicates[match.pattern_index].empty() &&
            !satisfies(data, match, text, scratch.get())) {
            ts_query_cursor_remove_match(cursor.get(), match.id);
            continue;
        }
        const TSQueryCapture& capture = match.captures[index];
        const std::string_view scope = data.capture_scopes[capture.index];
        if (scope.empty()) {
            continue;
        }
        const std::size_t begin =
            std::max<std::size_t>(ts_node_start_byte(capture.node), line_begin);
        const std::size_t end = std::min<std::size_t>(ts_node_end_byte(capture.node), line_end);
        if (begin < end) {
            captures.push_back({begin - line_begin, end - line_begin, match.pattern_index, scope,
                                data.capture_weak[capture.index]});
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

// A parsed text that the editor reads on its own thread while the highlighter that made it goes
// on parsing on another: it has a copy of the tree, which shares its nodes and is safe to use
// concurrently with the original, and the query data, which only reads.
class FrozenHighlighter final : public Highlighter {
public:
    FrozenHighlighter(Text text, std::unique_ptr<TSTree, TreeDeleter> tree,
                      std::shared_ptr<const QueryData> data, std::string_view root_scope)
        : text_(std::move(text)), tree_(std::move(tree)), data_(std::move(data)),
          root_scope_(root_scope) {}

    std::vector<HighlightSpan> spans(std::size_t line) const override {
        const auto found = cache_.find(line);
        if (found != cache_.end()) {
            return found->second;
        }
        return cache_
            .emplace(line, line_spans(text_, *tree_, *data_, line, max_highlighted_line_units))
            .first->second;
    }
    std::string_view root_scope() const override {
        return root_scope_;
    }
    std::uint64_t version() const override {
        return 1;
    }

private:
    Text text_;
    std::unique_ptr<TSTree, TreeDeleter> tree_;
    std::shared_ptr<const QueryData> data_;
    std::string_view root_scope_;
    mutable std::unordered_map<std::size_t, std::vector<HighlightSpan>> cache_;
};

// Compiles `source` as a highlights query of the grammar.
std::expected<std::shared_ptr<const QueryData>, Error> build_query_data(const Grammar& grammar,
                                                                        std::string_view source) {
    const std::string language_id(grammar.language_id);
    const TSLanguage* language = grammar.language();
    std::uint32_t error_offset = 0;
    TSQueryError error_type = TSQueryErrorNone;
    std::unique_ptr<TSQuery, QueryDeleter> query(
        ts_query_new(language, source.data(), static_cast<std::uint32_t>(source.size()),
                     &error_offset, &error_type));
    if (!query) {
        static constexpr const char* kinds[] = {"none",       "syntax error", "node type",
                                                "field name", "capture name", "structure",
                                                "language"};
        const std::string_view near = source.substr(error_offset, 40);
        return std::unexpected(Error{ErrorCode::environment_unavailable,
                                     "the highlights query of " + language_id + " has an invalid " +
                                         kinds[error_type] + " at byte " +
                                         std::to_string(error_offset) + ", near \"" +
                                         std::string(near.substr(0, near.find('\n'))) + "\""});
    }

    auto data = std::make_shared<QueryData>();
    data->query = std::move(query);
    auto predicates = read_predicates(*data->query);
    if (!predicates) {
        return std::unexpected(
            Error{ErrorCode::environment_unavailable,
                  "the highlights query of " + language_id + ": " + predicates.error()});
    }
    data->predicates = std::move(*predicates);
    const std::span<const CaptureScope> table = grammar.scopes();
    const std::uint32_t captures = ts_query_capture_count(data->query.get());
    std::vector<const CaptureScope*> rows;
    for (std::uint32_t i = 0; i < captures; ++i) {
        std::uint32_t length = 0;
        const char* name = ts_query_capture_name_for_id(data->query.get(), i, &length);
        rows.push_back(find_capture_scope(table, std::string_view(name, length)));
    }
    data->scope_names.reserve(captures);
    for (const CaptureScope* row : rows) {
        data->capture_weak.push_back(row && row->weak);
        if (row) {
            data->scope_names.push_back(std::string(row->scope) + "." +
                                        std::string(grammar.scope_suffix));
            data->capture_scopes.push_back(data->scope_names.back());
        } else {
            data->capture_scopes.emplace_back();
        }
    }
    return data;
}

// The embedded query of a grammar, compiled once for the process: compiling the query of a large
// grammar takes longer than parsing a small document, and the result only reads.
std::expected<std::shared_ptr<const QueryData>, Error> query_data_for(const Grammar& grammar) {
    static std::mutex mutex;
    static std::map<const Grammar*, std::shared_ptr<const QueryData>> cache;
    const std::lock_guard lock(mutex);
    if (const auto found = cache.find(&grammar); found != cache.end()) {
        return found->second;
    }
    auto data = build_query_data(grammar, grammar.highlights);
    if (data) {
        cache.emplace(&grammar, *data);
    }
    return data;
}

} // namespace

struct TreeSitterHighlighter::Impl {
    Text text;
    const Grammar* grammar = nullptr;
    std::unique_ptr<TSParser, ParserDeleter> parser;
    std::unique_ptr<TSTree, TreeDeleter> tree;
    std::shared_ptr<const QueryData> data;
    // The parser polls it while it parses and stops when it is set; a `std::stop_callback` sets it
    // from the thread that asks for the stop.
    std::atomic<bool> cancel{false};
    // The spans of each line once asked for, kept until an edit or a change of the tree reaches
    // the line. One slot per line of `text`.
    mutable std::vector<std::optional<std::vector<HighlightSpan>>> cache;
    std::uint64_t version = 1;
    std::size_t bytes_read = 0;
    mutable std::size_t line_queries = 0;
};

bool TreeSitterHighlighter::supports(std::string_view language_id) {
    return find_grammar(language_id) != nullptr;
}

std::expected<std::unique_ptr<TreeSitterHighlighter>, Error>
TreeSitterHighlighter::create(std::string_view language_id, Text text, std::stop_token stop) {
    return create_with_query(language_id, {}, std::move(text), stop);
}

std::expected<std::unique_ptr<TreeSitterHighlighter>, Error>
TreeSitterHighlighter::create_with_query(std::string_view language_id, std::string_view query,
                                         Text text, std::stop_token stop) {
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

    auto data = query.empty() ? query_data_for(*grammar) : build_query_data(*grammar, query);
    if (!data) {
        return std::unexpected(data.error());
    }

    auto impl = std::make_unique<Impl>();
    impl->text = std::move(text);
    impl->grammar = grammar;
    impl->data = std::move(*data);
    impl->parser = std::move(parser);
    const std::stop_callback cancel_on_stop(stop, [flag = &impl->cancel] { flag->store(true); });
    ReadState state{&impl->text, &impl->bytes_read};
    const TSInput input{&state, read_chunk, TSInputEncodingUTF8, nullptr};
    impl->tree.reset(parse(*impl->parser, nullptr, input, impl->cancel));
    if (!impl->tree) {
        return std::unexpected(Error{ErrorCode::environment_unavailable,
                                     impl->cancel.load() ? "cancelled" : "parsing failed"});
    }
    impl->cache.resize(impl->text.line_count());
    return std::unique_ptr<TreeSitterHighlighter>(new TreeSitterHighlighter(std::move(impl)));
}

TreeSitterHighlighter::TreeSitterHighlighter(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

TreeSitterHighlighter::~TreeSitterHighlighter() = default;

std::vector<HighlightSpan> TreeSitterHighlighter::spans(std::size_t line) const {
    if (line >= impl_->cache.size()) {
        return {};
    }
    auto& slot = impl_->cache[line];
    if (!slot) {
        ++impl_->line_queries;
        slot = impl_->tree
                   ? line_spans(impl_->text, *impl_->tree, *impl_->data, line, no_line_limit)
                   : std::vector<HighlightSpan>{};
    }
    return *slot;
}

std::shared_ptr<const Highlighter> TreeSitterHighlighter::snapshot() const {
    std::unique_ptr<TSTree, TreeDeleter> tree;
    if (impl_->tree) {
        tree.reset(ts_tree_copy(impl_->tree.get()));
    }
    return std::make_shared<FrozenHighlighter>(impl_->text, std::move(tree), impl_->data,
                                               impl_->grammar->root_scope);
}

std::vector<std::string> TreeSitterHighlighter::unmapped_captures(std::string_view language_id) {
    const Grammar* grammar = find_grammar(language_id);
    const auto data = grammar ? query_data_for(*grammar) : std::unexpected(Error{});
    std::vector<std::string> names;
    if (!data) {
        return names;
    }
    const TSQuery& query = *(*data)->query;
    for (std::uint32_t i = 0; i < (*data)->capture_scopes.size(); ++i) {
        if ((*data)->capture_scopes[i].empty()) {
            std::uint32_t length = 0;
            const char* name = ts_query_capture_name_for_id(&query, i, &length);
            names.emplace_back(name, length);
        }
    }
    return names;
}

std::string_view TreeSitterHighlighter::root_scope_of(std::string_view language_id) {
    const Grammar* grammar = find_grammar(language_id);
    return grammar ? grammar->root_scope : std::string_view{};
}

std::string_view TreeSitterHighlighter::root_scope() const {
    return impl_->grammar->root_scope;
}

std::uint64_t TreeSitterHighlighter::version() const {
    return impl_->version;
}

std::string TreeSitterHighlighter::describe_tree() const {
    std::string out;
    if (impl_->tree) {
        describe(ts_tree_root_node(impl_->tree.get()), out);
    }
    return out;
}

std::size_t TreeSitterHighlighter::bytes_read() const {
    return impl_->bytes_read;
}

std::size_t TreeSitterHighlighter::line_queries() const {
    return impl_->line_queries;
}

std::expected<void, Error> TreeSitterHighlighter::apply(const TextChange& change,
                                                        const Text& new_text) {
    return apply(std::span<const TextChange>(&change, 1), new_text);
}

std::expected<void, Error> TreeSitterHighlighter::apply(const DocumentChange& change,
                                                        const Text& new_text) {
    return apply(std::span<const TextChange>(change.changes), new_text);
}

std::expected<void, Error> TreeSitterHighlighter::apply(std::span<const TextChange> changes,
                                                        const Text& new_text,
                                                        std::stop_token stop) {
    if (!impl_->tree) {
        return std::unexpected(Error{ErrorCode::environment_unavailable, "the text is not parsed"});
    }
    if (new_text.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(
            Error{ErrorCode::limit_exceeded, "text is too large for tree-sitter"});
    }

    // Each change is described against the text the ones before it left, which the highlighter
    // rebuilds from its own text; the points of an edit need the lines of those texts.
    struct Step {
        TSInputEdit edit;
        std::size_t first_row = 0;
        std::size_t old_rows = 0;
        std::size_t new_rows = 0;
    };
    std::vector<Step> steps;
    steps.reserve(changes.size());
    Text current = impl_->text;
    for (const TextChange& change : changes) {
        const std::size_t old_end = change.offset + change.removed_bytes;
        auto next = current.replaced(change.offset, old_end, change.text);
        if (!next) {
            return std::unexpected(next.error());
        }
        const std::size_t new_end = change.offset + change.text.size();
        Step step;
        step.edit = TSInputEdit{static_cast<std::uint32_t>(change.offset),
                                static_cast<std::uint32_t>(old_end),
                                static_cast<std::uint32_t>(new_end),
                                point_at(current, change.offset),
                                point_at(current, old_end),
                                point_at(*next, new_end)};
        step.first_row = step.edit.start_point.row;
        step.old_rows = step.edit.old_end_point.row - step.first_row + 1;
        step.new_rows = step.edit.new_end_point.row - step.first_row + 1;
        steps.push_back(step);
        current = std::move(*next);
    }
    if (current.size() != new_text.size()) {
        return std::unexpected(
            Error{ErrorCode::invalid_argument, "the changes do not lead to the new text"});
    }

    for (const Step& step : steps) {
        ts_tree_edit(impl_->tree.get(), &step.edit);
        auto& cache = impl_->cache;
        const auto first =
            cache.begin() + static_cast<std::ptrdiff_t>(std::min(step.first_row, cache.size()));
        const auto last =
            first + static_cast<std::ptrdiff_t>(
                        std::min(step.old_rows, static_cast<std::size_t>(cache.end() - first)));
        cache.erase(first, last);
        cache.insert(cache.begin() +
                         static_cast<std::ptrdiff_t>(std::min(step.first_row, cache.size())),
                     step.new_rows, std::nullopt);
    }

    impl_->text = new_text;
    const std::stop_callback cancel_on_stop(stop, [flag = &impl_->cancel] { flag->store(true); });
    ReadState state{&impl_->text, &impl_->bytes_read};
    const TSInput input{&state, read_chunk, TSInputEncodingUTF8, nullptr};
    std::unique_ptr<TSTree, TreeDeleter> tree(
        parse(*impl_->parser, impl_->tree.get(), input, impl_->cancel));
    if (!tree) {
        impl_->tree.reset();
        impl_->cache.assign(impl_->text.line_count(), std::nullopt);
        ++impl_->version;
        return std::unexpected(Error{ErrorCode::environment_unavailable,
                                     impl_->cancel.load() ? "cancelled" : "parsing failed"});
    }

    std::uint32_t range_count = 0;
    TSRange* ranges = ts_tree_get_changed_ranges(impl_->tree.get(), tree.get(), &range_count);
    impl_->tree = std::move(tree);
    impl_->cache.resize(impl_->text.line_count());
    for (std::uint32_t i = 0; i < range_count; ++i) {
        const std::size_t last_row =
            std::min<std::size_t>(ranges[i].end_point.row, impl_->cache.size() - 1);
        for (std::size_t row = ranges[i].start_point.row; row <= last_row; ++row) {
            impl_->cache[row].reset();
        }
    }
    std::free(ranges);
    ++impl_->version;
    return {};
}

} // namespace anvil::core
