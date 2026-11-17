#pragma once

#include "anvil/core/document.hpp"
#include "anvil/core/error.hpp"
#include "anvil/core/text.hpp"

#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace anvil::core {

namespace detail {
struct SearchImpl;
}

struct Query {
    std::string pattern{};
    // Without it, characters match their other-case forms. Case folding is Unicode simple case
    // folding, so "é" matches "É" but "ß" does not match "SS".
    bool match_case = false;
    // A match must not be directly preceded or followed by a word character, using the classes
    // of word motion (see classify). A match that starts or ends with a non-word character is
    // not checked on that side.
    bool whole_word = false;
    // The pattern is a Perl-compatible regular expression (PCRE2 syntax, UTF mode) instead of
    // literal text. Matches never contain a line break: each line is matched on its own, so `^`
    // and `$` match at the ends of lines, and `\n` never matches.
    bool regex = false;
};

// A byte range of the text, on character boundaries.
struct Match {
    std::size_t begin = 0;
    std::size_t end = 0;

    bool operator==(const Match&) const = default;
};

// A match found by a search that may start over at the other end of the text.
struct Hit {
    Match match;
    // True when the search passed the end (or start) of the text to find this match.
    bool wrapped = false;
};

struct MatchList {
    std::vector<Match> matches;
    // True when more matches exist than the requested limit.
    bool truncated = false;
};

// A compiled query. Copying is cheap and the copies share the compiled pattern, which is safe to
// use from several threads at once. The text is read through `Text` snapshots in bounded windows,
// so searching never copies the whole text.
//
// Matches are leftmost and non-overlapping: searching "aa" in "aaa" finds one match. An empty
// pattern never matches. A regular expression that can match the empty string yields empty
// matches, and the search continues one character after each.
//
// A search can fail while running, for example when a regular expression backtracks beyond the
// engine's limits; the error then says so and no matches are returned.
class Search {
public:
    static constexpr std::size_t default_window_bytes = 64 * 1024;

    // Fails with ErrorCode::invalid_argument when a regular expression does not compile; the
    // message says why. `window_bytes` is the size of the text windows the search reads at a
    // time and exists so tests can exercise window edges with small texts.
    static std::expected<Search, Error> create(Query query,
                                               std::size_t window_bytes = default_window_bytes);

    const Query& query() const;

    // The first match that begins at or after `from`. With `wrap`, a search that finds nothing
    // continues from the start of the text and may find a match that begins before `from`.
    std::expected<std::optional<Hit>, Error> find_next(const Text& text, std::size_t from,
                                                       bool wrap = false) const;

    // The last match that begins before `before`, taken from the matches found by scanning
    // forward from the start of a line. With `wrap`, a search that finds nothing continues from
    // the end of the text.
    std::expected<std::optional<Hit>, Error> find_previous(const Text& text, std::size_t before,
                                                           bool wrap = false) const;

    // The matches that lie inside [begin, end), in order. At most `limit` are returned. A stopped
    // search returns the matches found so far. The text after `end` is invisible to the pattern,
    // so `$` and look-ahead stop there; the text before `begin` stays visible (to the start of
    // its line for a regular expression, one character for a plain query), so that a range does
    // not change what counts as a word boundary. An offset inside a character moves to the next
    // character start for `begin` and to the previous one for `end`.
    std::expected<MatchList, Error> find_all(const Text& text, std::size_t begin, std::size_t end,
                                             std::size_t limit, std::stop_token stop = {}) const;

    // The text that replaces `match`, or std::nullopt when this search has no match that begins
    // at `match.begin` and ends at `match.end`. The search starts at `match.begin`, so a range
    // that scanning from the start of the text would skip because an earlier match overlaps it,
    // such as the second "aa" of "aaa", is accepted.
    //
    // A plain query uses `replacement` as it is. For a regular expression, `$0` or
    // `$&` insert the match, `$1` to `$99` insert a group (nothing for a group that did not
    // take part, and the text as written for a group the pattern does not have), `$$` inserts a
    // dollar sign, and `\n`, `\t` and `\\` insert a line break, a tab and a backslash.
    std::expected<std::optional<std::string>, Error>
    replacement_for(const Text& text, Match match, std::string_view replacement) const;

    // Edits that replace every match in the text, ready for Document::apply_edits. A stopped
    // search returns the edits found so far.
    std::expected<std::vector<Edit>, Error>
    replace_all(const Text& text, std::string_view replacement, std::stop_token stop = {}) const;

private:
    explicit Search(std::shared_ptr<const detail::SearchImpl> impl) : impl_(std::move(impl)) {}

    std::shared_ptr<const detail::SearchImpl> impl_;
};

} // namespace anvil::core
