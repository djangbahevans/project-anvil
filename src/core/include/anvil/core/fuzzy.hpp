#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace anvil::core {

// Fuzzy matching and ranking of labels, the way VS Code's quick pick does it: a port of the
// behavior of `src/vs/base/common/fuzzyScorer.ts` (`scoreFuzzy`, `scoreItemFuzzy`,
// `compareItemsByFuzzyScore`), so that results rank as they do there.
//
// Text is UTF-8. The algorithm runs on UTF-16 code units, as VS Code's does, and every position
// and range it reports counts UTF-16 code units, which is what a Qt string or an extension sees.
// Malformed UTF-8 reads as U+FFFD, one per bad byte.
//
// Differences from VS Code:
// - Case is folded for ASCII letters only. A non-ASCII letter matches its other-case form only
//   when Unicode case mapping exists in the core.
// - The last tie-break of `compare_items`, which VS Code decides with a locale collator and a
//   natural file name order, compares ASCII-folded strings code unit by code unit.

// A half-open range of UTF-16 code units.
struct MatchRange {
    std::size_t start = 0;
    std::size_t end = 0;

    friend bool operator==(const MatchRange&, const MatchRange&) = default;
};

// How file paths are written where the query is used. Queries that name a path are compared with
// the item's path using these, and paths are joined to descriptions with the separator.
struct PathConventions {
    char16_t separator = u'/';
    // Linux compares paths exactly; Windows and macOS ignore the case of ASCII letters.
    bool case_sensitive = true;
};

// One whitespace-free part of a query, normalized: quotes, `*`, `…` and whitespace removed.
struct QueryPiece {
    std::u16string normalized;
    std::u16string normalized_lowercase;
    // The part was wrapped in double quotes: it must occur as written, with no gaps.
    bool expect_contiguous = false;
};

// A query ready for scoring many items. A query with several words separated by spaces holds a
// piece for each, and an item must match all of them.
struct PreparedQuery : QueryPiece {
    // The query with the path separators of the other platform style replaced by `separator`.
    std::u16string path_normalized;
    bool contains_path_separator = false;
    // Set when there is more than one word.
    std::vector<QueryPiece> pieces;
    char16_t separator = u'/';
    bool case_sensitive_paths = true;
};

PreparedQuery prepare_query(std::string_view query, PathConventions conventions = {});

// The result of matching one string.
struct FuzzyMatch {
    // Zero is no match.
    int score = 0;
    // The index of each matched code unit of the target, ascending.
    std::vector<std::size_t> positions;
};

// Scores `target` against one query piece. Without `allow_non_contiguous` the piece must occur in
// the target as a whole.
FuzzyMatch score_fuzzy(std::string_view target, const QueryPiece& query, bool allow_non_contiguous);

// Something to rank: a label, and optionally a description and the path of a file. When there is
// a path, it is what a query that contains a path separator is matched against, and the
// description is the directory, as in the explorer's quick open.
struct FuzzyItem {
    std::string_view label;
    std::string_view description;
    std::string_view path;
};

struct ItemScore {
    // Zero is no match. A match on the label alone scores above 1 << 16, a prefix of the label
    // above 1 << 17, and a query that is the item's path exactly 1 << 18.
    int score = 0;
    std::vector<MatchRange> label_match;
    std::vector<MatchRange> description_match;
};

ItemScore score_item(const FuzzyItem& item, const PreparedQuery& query,
                     bool allow_non_contiguous = true);

// Orders two scored items: negative when `a` ranks first, positive when `b` does, zero when they
// are equal. Scores come from score_item with the same query. Sorting with it ranks the best
// match first, with ties broken by how compact the match is and how short the label is.
int compare_items(const FuzzyItem& a, const ItemScore& score_a, const FuzzyItem& b,
                  const ItemScore& score_b, const PreparedQuery& query);

} // namespace anvil::core
