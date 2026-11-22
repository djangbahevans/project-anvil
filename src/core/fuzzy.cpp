#include "anvil/core/fuzzy.hpp"

#include <algorithm>
#include <cmath>

namespace anvil::core {
namespace {

using Text16 = std::u16string;
using View16 = std::u16string_view;

constexpr int path_identity_score = 1 << 18;
constexpr int label_prefix_score_threshold = 1 << 17;
constexpr int label_score_threshold = 1 << 16;

Text16 to_utf16(std::string_view utf8) {
    static constexpr char32_t minimum[] = {0, 0, 0x80, 0x800, 0x10000};
    Text16 out;
    out.reserve(utf8.size());
    std::size_t i = 0;
    while (i < utf8.size()) {
        const auto lead = static_cast<unsigned char>(utf8[i]);
        std::size_t length = 1;
        char32_t code_point = lead;
        if (lead >= 0x80) {
            if ((lead & 0xE0) == 0xC0) {
                length = 2;
                code_point = lead & 0x1Fu;
            } else if ((lead & 0xF0) == 0xE0) {
                length = 3;
                code_point = lead & 0x0Fu;
            } else if ((lead & 0xF8) == 0xF0) {
                length = 4;
                code_point = lead & 0x07u;
            } else {
                length = 0;
            }
        }
        bool valid = length != 0 && i + length <= utf8.size();
        for (std::size_t k = 1; valid && k < length; ++k) {
            const auto next = static_cast<unsigned char>(utf8[i + k]);
            valid = (next & 0xC0) == 0x80;
            code_point = (code_point << 6) | (next & 0x3Fu);
        }
        valid = valid && code_point >= minimum[length] && code_point <= 0x10FFFF &&
                (code_point < 0xD800 || code_point > 0xDFFF);
        if (!valid) {
            out.push_back(u'�');
            ++i;
            continue;
        }
        if (code_point >= 0x10000) {
            code_point -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (code_point >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (code_point & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(code_point));
        }
        i += length;
    }
    return out;
}

char16_t fold(char16_t c) {
    return c >= u'A' && c <= u'Z' ? static_cast<char16_t>(c + 32) : c;
}

Text16 folded(View16 text) {
    Text16 out(text);
    std::ranges::transform(out, out.begin(), fold);
    return out;
}

bool is_upper(char16_t c) {
    return c >= u'A' && c <= u'Z';
}

bool equals_ignore_case(View16 a, View16 b) {
    return a.size() == b.size() &&
           std::ranges::equal(a, b, [](char16_t x, char16_t y) { return fold(x) == fold(y); });
}

bool starts_with_ignore_case(View16 text, View16 prefix) {
    return prefix.size() <= text.size() &&
           equals_ignore_case(text.substr(0, prefix.size()), prefix);
}

// The whitespace of a JavaScript regular expression's `\s`.
bool is_script_space(char16_t c) {
    switch (c) {
    case u'\t':
    case u'\n':
    case u'\v':
    case u'\f':
    case u'\r':
    case u' ':
    case u' ':
    case u' ':
    case u' ':
    case u' ':
    case u' ':
    case u' ':
    case u'　':
    case u'﻿':
        return true;
    default:
        return c >= u' ' && c <= u' ';
    }
}

struct NormalizedQuery {
    Text16 path_normalized;
    Text16 normalized;
};

// Quotes mark an exact match, wildcards stand for fuzzy matching, whitespace separates queries,
// the ellipsis abbreviates path segments, and a trailing `#` is a query modifier some language
// servers use.
NormalizedQuery normalize_query(View16 original, PathConventions conventions) {
    const char16_t other = conventions.separator == u'/' ? u'\\' : u'/';
    NormalizedQuery result;
    result.path_normalized.assign(original);
    std::ranges::replace(result.path_normalized, other, conventions.separator);
    for (const char16_t c : result.path_normalized) {
        if (c != u'*' && c != u'…' && c != u'"' && !is_script_space(c)) {
            result.normalized.push_back(c);
        }
    }
    if (result.normalized.size() > 1 && result.normalized.back() == u'#') {
        result.normalized.pop_back();
    }
    return result;
}

bool is_quoted(View16 text) {
    return text.starts_with(u'"') && text.ends_with(u'"');
}

int separator_bonus(char16_t c) {
    switch (c) {
    case u'/':
    case u'\\':
        return 5;
    case u'_':
    case u'-':
    case u'.':
    case u' ':
    case u'\'':
    case u'"':
    case u':':
        return 4;
    default:
        return 0;
    }
}

bool consider_as_equal(char16_t a, char16_t b) {
    if (a == b) {
        return true;
    }
    if (a == u'/' || a == u'\\') {
        return b == u'/' || b == u'\\';
    }
    return false;
}

int compute_char_score(char16_t query_char, char16_t query_lower, View16 target,
                       View16 target_lower, std::size_t target_index, int sequence_length) {
    if (!consider_as_equal(query_lower, target_lower[target_index])) {
        return 0;
    }
    int score = 1;
    // Sequences up to 3 get the full bonus and the rest half of it, so long runs do not dominate.
    if (sequence_length > 0) {
        score += std::min(sequence_length, 3) * 6 + std::max(0, sequence_length - 3) * 3;
    }
    if (query_char == target[target_index]) {
        score += 1;
    }
    if (target_index == 0) {
        score += 8;
    } else if (const int bonus = separator_bonus(target[target_index - 1]); bonus != 0) {
        score += bonus;
    } else if (is_upper(target[target_index]) && sequence_length == 0) {
        // A hump inside a word ("NPE" in "NullPointerException"), but not inside a run of capitals.
        score += 2;
    }
    return score;
}

FuzzyMatch score_text(View16 target, View16 query, View16 query_lower, bool allow_non_contiguous) {
    if (target.empty() || query.empty() || target.size() < query.size()) {
        return {};
    }
    const Text16 target_lower_text = folded(target);
    const View16 target_lower = target_lower_text;
    const std::size_t target_length = target.size();
    const std::size_t query_length = query.size();

    // For each query character and target position: the best score of a match that ends there, and
    // the length of the run of consecutive characters it ends.
    std::vector<int> scores(query_length * target_length);
    std::vector<int> runs(query_length * target_length);
    for (std::size_t qi = 0; qi < query_length; ++qi) {
        const std::size_t row = qi * target_length;
        for (std::size_t ti = 0; ti < target_length; ++ti) {
            const std::size_t current = row + ti;
            const int left_score = ti > 0 ? scores[current - 1] : 0;
            const bool has_diagonal = qi > 0 && ti > 0;
            const int diagonal_score = has_diagonal ? scores[current - target_length - 1] : 0;
            const int run = has_diagonal ? runs[current - target_length - 1] : 0;

            // Past the first query character a match needs a previous one to continue, or "de"
            // would score a match of "e" at the start of "ede" with the start-of-word bonus.
            const int score =
                diagonal_score == 0 && qi > 0
                    ? 0
                    : compute_char_score(query[qi], query_lower[qi], target, target_lower, ti, run);
            const bool extends = score != 0 && diagonal_score + score >= left_score &&
                                 (allow_non_contiguous || qi > 0 ||
                                  target_lower.substr(ti).starts_with(query_lower));
            if (extends) {
                runs[current] = run + 1;
                scores[current] = diagonal_score + score;
            } else {
                runs[current] = 0;
                scores[current] = left_score;
            }
        }
    }

    FuzzyMatch result;
    result.score = scores[query_length * target_length - 1];
    auto qi = static_cast<std::ptrdiff_t>(query_length) - 1;
    auto ti = static_cast<std::ptrdiff_t>(target_length) - 1;
    while (qi >= 0 && ti >= 0) {
        const auto current =
            static_cast<std::size_t>(qi) * target_length + static_cast<std::size_t>(ti);
        if (runs[current] == 0) {
            --ti;
        } else {
            result.positions.push_back(static_cast<std::size_t>(ti));
            --qi;
            --ti;
        }
    }
    std::ranges::reverse(result.positions);
    return result;
}

std::vector<MatchRange> to_ranges(const std::vector<std::size_t>& positions) {
    std::vector<MatchRange> ranges;
    for (const std::size_t position : positions) {
        if (!ranges.empty() && ranges.back().end == position) {
            ++ranges.back().end;
        } else {
            ranges.push_back({position, position + 1});
        }
    }
    return ranges;
}

// Sorts by start and merges ranges that overlap or touch.
std::vector<MatchRange> normalize_ranges(std::vector<MatchRange> ranges) {
    std::ranges::stable_sort(ranges, {}, &MatchRange::start);
    std::vector<MatchRange> merged;
    for (const MatchRange& range : ranges) {
        if (!merged.empty() && merged.back().end >= range.start &&
            range.end >= merged.back().start) {
            merged.back().start = std::min(merged.back().start, range.start);
            merged.back().end = std::max(merged.back().end, range.end);
        } else {
            merged.push_back(range);
        }
    }
    return merged;
}

struct Item16 {
    Text16 label;
    Text16 description;
    Text16 path;
};

ItemScore score_piece(const Item16& item, const QueryPiece& piece, bool prefer_label_matches,
                      char16_t separator, bool allow_non_contiguous) {
    const bool fuzzy = allow_non_contiguous && !piece.expect_contiguous;

    if (prefer_label_matches || item.description.empty()) {
        const FuzzyMatch label =
            score_text(item.label, piece.normalized, piece.normalized_lowercase, fuzzy);
        if (label.score != 0) {
            // A prefix of the label ranks above any other match in it, and a shorter label above a
            // longer one, so that "window" finds window.ts before windowActions.ts.
            const bool prefix =
                !piece.normalized.empty() && starts_with_ignore_case(item.label, piece.normalized);
            int base = label_score_threshold;
            std::vector<MatchRange> ranges;
            if (prefix) {
                const double share = static_cast<double>(piece.normalized.size()) /
                                     static_cast<double>(item.label.size());
                base =
                    label_prefix_score_threshold + static_cast<int>(std::floor(share * 100 + 0.5));
                ranges.push_back({0, piece.normalized.size()});
            } else {
                ranges = to_ranges(label.positions);
            }
            return {base + label.score, std::move(ranges), {}};
        }
    }

    if (!item.description.empty()) {
        Text16 prefix = item.description;
        if (!item.path.empty()) {
            prefix.push_back(separator);
        }
        const Text16 combined = prefix + item.label;
        const FuzzyMatch match =
            score_text(combined, piece.normalized, piece.normalized_lowercase, fuzzy);
        if (match.score != 0) {
            ItemScore result;
            result.score = match.score;
            for (const MatchRange& range : to_ranges(match.positions)) {
                if (range.start < prefix.size() && range.end > prefix.size()) {
                    result.label_match.push_back({0, range.end - prefix.size()});
                    result.description_match.push_back({range.start, prefix.size()});
                } else if (range.start >= prefix.size()) {
                    result.label_match.push_back(
                        {range.start - prefix.size(), range.end - prefix.size()});
                } else {
                    result.description_match.push_back(range);
                }
            }
            return result;
        }
    }
    return {};
}

ItemScore score_pieces(const Item16& item, const std::vector<QueryPiece>& pieces,
                       bool prefer_label_matches, char16_t separator, bool allow_non_contiguous) {
    int total = 0;
    std::vector<MatchRange> label_matches;
    std::vector<MatchRange> description_matches;
    for (const QueryPiece& piece : pieces) {
        ItemScore score =
            score_piece(item, piece, prefer_label_matches, separator, allow_non_contiguous);
        if (score.score == 0) {
            return {};
        }
        total += score.score;
        label_matches.insert(label_matches.end(), score.label_match.begin(),
                             score.label_match.end());
        description_matches.insert(description_matches.end(), score.description_match.begin(),
                                   score.description_match.end());
    }
    return {total, normalize_ranges(std::move(label_matches)),
            normalize_ranges(std::move(description_matches))};
}

int compare_by_match_length(const std::vector<MatchRange>& a, const std::vector<MatchRange>& b) {
    if (a.empty() && b.empty()) {
        return 0;
    }
    if (b.empty()) {
        return -1;
    }
    if (a.empty()) {
        return 1;
    }
    const std::size_t length_a = a.back().end - a.front().start;
    const std::size_t length_b = b.back().end - b.front().start;
    return length_a == length_b ? 0 : length_b < length_a ? 1 : -1;
}

// The span from the first to the last matched code unit of label and description together, with
// the description counted as lying before the label.
long match_distance(const ItemScore& score, std::size_t description_length) {
    long start = -1;
    long end = -1;
    if (!score.description_match.empty()) {
        start = static_cast<long>(score.description_match.front().start);
    } else if (!score.label_match.empty()) {
        start = static_cast<long>(score.label_match.front().start);
    }
    if (!score.label_match.empty()) {
        end = static_cast<long>(score.label_match.back().end);
        if (!score.description_match.empty()) {
            end += static_cast<long>(description_length);
        }
    } else if (!score.description_match.empty()) {
        end = static_cast<long>(score.description_match.back().end);
    }
    return end - start;
}

int compare_by_prefix(View16 a, View16 b, View16 look_for) {
    const bool prefix_a = a.starts_with(look_for);
    const bool prefix_b = b.starts_with(look_for);
    if (prefix_a != prefix_b) {
        return prefix_a ? -1 : 1;
    }
    if (prefix_a && prefix_b && a.size() != b.size()) {
        return a.size() < b.size() ? -1 : 1;
    }
    return 0;
}

int compare_anything(View16 one, View16 other, View16 look_for) {
    const Text16 a = folded(one);
    const Text16 b = folded(other);
    if (const int by_prefix = compare_by_prefix(a, b, look_for); by_prefix != 0) {
        return by_prefix;
    }
    const bool suffix_a = View16(a).ends_with(look_for);
    const bool suffix_b = View16(b).ends_with(look_for);
    if (suffix_a != suffix_b) {
        return suffix_a ? -1 : 1;
    }
    return a < b ? -1 : a > b ? 1 : 0;
}

int sign_of_difference(std::size_t a, std::size_t b) {
    return a < b ? -1 : a > b ? 1 : 0;
}

int fallback_compare(const Item16& a, const Item16& b, const PreparedQuery& query) {
    const std::size_t length_a = a.label.size() + a.description.size();
    const std::size_t length_b = b.label.size() + b.description.size();
    if (length_a != length_b) {
        return sign_of_difference(length_a, length_b);
    }
    if (!a.path.empty() && !b.path.empty() && a.path.size() != b.path.size()) {
        return sign_of_difference(a.path.size(), b.path.size());
    }
    if (a.label != b.label) {
        return compare_anything(a.label, b.label, query.normalized);
    }
    if (!a.description.empty() && !b.description.empty() && a.description != b.description) {
        return compare_anything(a.description, b.description, query.normalized);
    }
    if (!a.path.empty() && !b.path.empty() && a.path != b.path) {
        return compare_anything(a.path, b.path, query.normalized);
    }
    return 0;
}

Item16 to_item16(const FuzzyItem& item) {
    return {to_utf16(item.label), to_utf16(item.description), to_utf16(item.path)};
}

} // namespace

PreparedQuery prepare_query(std::string_view query, PathConventions conventions) {
    const Text16 original = to_utf16(query);
    NormalizedQuery whole = normalize_query(original, conventions);

    PreparedQuery prepared;
    prepared.normalized_lowercase = folded(whole.normalized);
    prepared.normalized = std::move(whole.normalized);
    prepared.path_normalized = std::move(whole.path_normalized);
    prepared.expect_contiguous = is_quoted(original);
    prepared.contains_path_separator =
        prepared.path_normalized.find(conventions.separator) != Text16::npos;
    prepared.separator = conventions.separator;
    prepared.case_sensitive_paths = conventions.case_sensitive;

    if (original.find(u' ') != Text16::npos) {
        for (std::size_t start = 0; start <= original.size();) {
            const std::size_t space = std::min(original.find(u' ', start), original.size());
            const View16 word = View16(original).substr(start, space - start);
            NormalizedQuery normalized = normalize_query(word, conventions);
            if (!normalized.normalized.empty()) {
                QueryPiece piece;
                piece.normalized_lowercase = folded(normalized.normalized);
                piece.normalized = std::move(normalized.normalized);
                piece.expect_contiguous = is_quoted(word);
                prepared.pieces.push_back(std::move(piece));
            }
            start = space + 1;
        }
    }
    return prepared;
}

FuzzyMatch score_fuzzy(std::string_view target, const QueryPiece& query,
                       bool allow_non_contiguous) {
    return score_text(to_utf16(target), query.normalized, query.normalized_lowercase,
                      allow_non_contiguous);
}

ItemScore score_item(const FuzzyItem& item, const PreparedQuery& query, bool allow_non_contiguous) {
    if (item.label.empty() || query.normalized.empty()) {
        return {};
    }
    const Item16 text = to_item16(item);
    const bool prefer_label_matches = text.path.empty() || !query.contains_path_separator;

    if (!text.path.empty()) {
        const bool identical = query.case_sensitive_paths
                                   ? query.path_normalized == text.path
                                   : equals_ignore_case(query.path_normalized, text.path);
        if (identical) {
            ItemScore score;
            score.score = path_identity_score;
            score.label_match.push_back({0, text.label.size()});
            if (!text.description.empty()) {
                score.description_match.push_back({0, text.description.size()});
            }
            return score;
        }
    }
    if (query.pieces.size() > 1) {
        return score_pieces(text, query.pieces, prefer_label_matches, query.separator,
                            allow_non_contiguous);
    }
    return score_piece(text, query, prefer_label_matches, query.separator, allow_non_contiguous);
}

int compare_items(const FuzzyItem& a, const ItemScore& score_a, const FuzzyItem& b,
                  const ItemScore& score_b, const PreparedQuery& query) {
    const int a_score = score_a.score;
    const int b_score = score_b.score;

    if (a_score == path_identity_score || b_score == path_identity_score) {
        if (a_score != b_score) {
            return a_score == path_identity_score ? -1 : 1;
        }
    }

    const Item16 text_a = to_item16(a);
    const Item16 text_b = to_item16(b);

    // A match in the label beats one that needed the description.
    if (a_score > label_score_threshold || b_score > label_score_threshold) {
        if (a_score != b_score) {
            return a_score > b_score ? -1 : 1;
        }
        // A tighter match wins, except among prefix matches, where the longer prefix wins.
        if (a_score < label_prefix_score_threshold && b_score < label_prefix_score_threshold) {
            if (const int by_length =
                    compare_by_match_length(score_a.label_match, score_b.label_match);
                by_length != 0) {
                return by_length;
            }
        }
        if (text_a.label.size() != text_b.label.size()) {
            return sign_of_difference(text_a.label.size(), text_b.label.size());
        }
    }

    if (a_score != b_score) {
        return a_score > b_score ? -1 : 1;
    }

    const bool a_in_label = !score_a.label_match.empty();
    const bool b_in_label = !score_b.label_match.empty();
    if (a_in_label != b_in_label) {
        return a_in_label ? -1 : 1;
    }

    const long distance_a = match_distance(score_a, text_a.description.size());
    const long distance_b = match_distance(score_b, text_b.description.size());
    if (distance_a != 0 && distance_b != 0 && distance_a != distance_b) {
        return distance_b > distance_a ? -1 : 1;
    }

    return fallback_compare(text_a, text_b, query);
}

} // namespace anvil::core
