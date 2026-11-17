#include "anvil/core/search.hpp"

#include "anvil/core/char_class.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <functional>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

namespace anvil::core {
namespace {

// Bounds the backtracking of a single match attempt, so a pattern such as (a+)+$ fails with an
// error instead of freezing the caller.
constexpr std::uint32_t match_limit = 10'000'000;
constexpr std::size_t jit_stack_start = 32 * 1024;
constexpr std::size_t jit_stack_max = 1024 * 1024;

template <auto Free> struct Deleter {
    void operator()(auto* pointer) const {
        Free(pointer);
    }
};

using Code = std::unique_ptr<pcre2_code, Deleter<pcre2_code_free>>;
using MatchData = std::unique_ptr<pcre2_match_data, Deleter<pcre2_match_data_free>>;
using MatchContext = std::unique_ptr<pcre2_match_context, Deleter<pcre2_match_context_free>>;
using JitStack = std::unique_ptr<pcre2_jit_stack, Deleter<pcre2_jit_stack_free>>;

std::string engine_message(int code) {
    switch (code) {
    case PCRE2_ERROR_MATCHLIMIT:
    case PCRE2_ERROR_DEPTHLIMIT:
    case PCRE2_ERROR_HEAPLIMIT:
    case PCRE2_ERROR_JIT_STACKLIMIT:
        return "the regular expression is too complex to run on this text";
    default:
        break;
    }
    PCRE2_UCHAR buffer[256];
    pcre2_get_error_message(code, buffer, sizeof buffer);
    return reinterpret_cast<const char*>(buffer);
}

std::size_t char_length(char lead) {
    const auto byte = static_cast<unsigned char>(lead);
    return byte < 0x80 ? 1 : byte >= 0xF0 ? 4 : byte >= 0xE0 ? 3 : 2;
}

std::size_t previous_char_start(std::string_view s, std::size_t offset) {
    do {
        --offset;
    } while (offset > 0 && (static_cast<unsigned char>(s[offset]) & 0xC0u) == 0x80u);
    return offset;
}

bool word_before(std::string_view s, std::size_t offset) {
    return is_word_character(decode_code_point(s, previous_char_start(s, offset)));
}

bool word_at(std::string_view s, std::size_t offset) {
    return is_word_character(decode_code_point(s, offset));
}

bool is_whole_word(std::string_view s, std::size_t begin, std::size_t end) {
    const bool left = begin == 0 || !word_before(s, begin) || (end > begin && !word_at(s, begin));
    const bool right = end == s.size() || !word_at(s, end) || (end > begin && !word_before(s, end));
    return left && right;
}

} // namespace

namespace detail {

struct SearchImpl {
    Query query;
    // Null for an empty pattern, which matches nothing.
    Code code;
    bool jit = false;
    std::size_t window_bytes = Search::default_window_bytes;
    // An upper bound on the length of a match, as bytes, so that a window reaching that far past
    // a position holds every match that begins there. Each character of the pattern matches
    // characters of at most four bytes, whatever its own length: "k" matches the three-byte
    // Kelvin sign when case is ignored.
    std::size_t max_match_bytes = 0;
};

} // namespace detail

namespace {

using detail::SearchImpl;

// A match handed to a visitor, with access to its groups.
struct Found {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::string_view subject;
    const PCRE2_SIZE* ovector = nullptr;
    std::size_t pairs = 0;

    std::string_view group(std::size_t index) const {
        if (index >= pairs || ovector[2 * index] == PCRE2_UNSET) {
            return {};
        }
        return subject.substr(ovector[2 * index], ovector[2 * index + 1] - ovector[2 * index]);
    }
};

// Returns false to end the scan.
using Visitor = std::function<bool(const Found&)>;

// Walks the text in windows and reports the matches. One scanner serves one operation.
class Scanner {
public:
    Scanner(const SearchImpl& impl, const Text& text, std::stop_token stop)
        : impl_(impl), text_(text), stop_(std::move(stop)) {
        data_.reset(pcre2_match_data_create_from_pattern(impl.code.get(), nullptr));
        context_.reset(pcre2_match_context_create(nullptr));
        pcre2_set_match_limit(context_.get(), match_limit);
        if (impl.jit) {
            jit_stack_.reset(pcre2_jit_stack_create(jit_stack_start, jit_stack_max, nullptr));
            pcre2_jit_stack_assign(context_.get(), nullptr, jit_stack_.get());
        }
    }

    // Reports the matches that begin in [from, start_limit), where no match extends past
    // `bound`, in order and without overlap. An offset inside a character moves to the next
    // character start for `from` and back to the previous one for `bound`.
    std::expected<void, Error> scan(std::size_t from, std::size_t start_limit, std::size_t bound,
                                    const Visitor& visit) {
        bound = std::min(bound, text_.size());
        while (!text_.is_char_boundary(bound)) {
            --bound;
        }
        if (from > bound) {
            return {};
        }
        from = align_forward(from);
        start_limit = std::min(start_limit, bound + 1);
        return impl_.query.regex ? scan_lines(from, start_limit, bound, visit)
                                 : scan_windows(from, start_limit, bound, visit);
    }

private:
    struct Progress {
        bool stopped = false;
        // Where scanning continues, as an absolute offset.
        std::size_t resume = 0;
    };

    std::size_t align_forward(std::size_t offset) const {
        while (offset < text_.size() && !text_.is_char_boundary(offset)) {
            ++offset;
        }
        return offset;
    }

    // A literal pattern may span line breaks, so it is searched in windows of the text. Each
    // window reaches `max_match_bytes` past the positions it owns, so that a match is never
    // cut by the end of a window, and one character back so that whole-word checks can see the
    // character before the first position.
    std::expected<void, Error> scan_windows(std::size_t from, std::size_t start_limit,
                                            std::size_t bound, const Visitor& visit) {
        std::size_t position = from;
        while (position < start_limit) {
            if (stop_.stop_requested()) {
                return {};
            }
            const std::size_t block_end =
                align_forward(std::min(bound, position + impl_.window_bytes));
            const std::size_t window_end =
                align_forward(std::min(bound, block_end + impl_.max_match_bytes));
            const std::size_t window_begin = position == 0 ? 0 : previous_boundary(position);
            const bool reaches_bound = window_end == bound;
            const std::size_t own_limit =
                reaches_bound ? start_limit : std::min(start_limit, block_end);

            const std::string subject = text_.slice(window_begin, window_end - window_begin);
            auto progress =
                match_subject(subject, window_begin, position - window_begin, own_limit, visit);
            if (!progress) {
                return std::unexpected(std::move(progress.error()));
            }
            if (progress->stopped || reaches_bound) {
                return {};
            }
            position = progress->resume;
        }
        return {};
    }

    std::size_t previous_boundary(std::size_t offset) const {
        do {
            --offset;
        } while (offset > 0 && !text_.is_char_boundary(offset));
        return offset;
    }

    // Regular expressions match one line at a time, in blocks of lines read together.
    std::expected<void, Error> scan_lines(std::size_t from, std::size_t start_limit,
                                          std::size_t bound, const Visitor& visit) {
        const std::size_t last_line = text_.position_at(bound).line;
        std::size_t line = text_.position_at(from).line;
        while (line <= last_line) {
            if (stop_.stop_requested()) {
                return {};
            }
            const std::size_t block_begin = text_.line_start(line);
            if (block_begin >= start_limit) {
                return {};
            }
            const std::size_t block_end_line =
                std::min(last_line,
                         text_.position_at(std::min(bound, block_begin + impl_.window_bytes)).line);
            const std::size_t block_end = std::min(bound, text_.line_end(block_end_line));
            const std::string block = text_.slice(block_begin, block_end - block_begin);

            std::size_t offset = 0;
            while (true) {
                const std::size_t newline = block.find('\n', offset);
                const std::size_t line_end = newline == std::string::npos ? block.size() : newline;
                const std::size_t line_begin = block_begin + offset;
                if (line_begin >= start_limit) {
                    return {};
                }
                const std::size_t start = from > line_begin ? from - line_begin : 0;
                auto progress =
                    match_subject(std::string_view(block).substr(offset, line_end - offset),
                                  line_begin, start, start_limit, visit);
                if (!progress) {
                    return std::unexpected(std::move(progress.error()));
                }
                if (progress->stopped) {
                    return {};
                }
                if (newline == std::string::npos) {
                    break;
                }
                offset = newline + 1;
            }
            line = block_end_line + 1;
        }
        return {};
    }

    // Matches within one subject, whose first byte is at absolute offset `base`, beginning at
    // `position` and reporting the matches that begin before `own_limit`.
    std::expected<Progress, Error> match_subject(std::string_view subject, std::size_t base,
                                                 std::size_t position, std::size_t own_limit,
                                                 const Visitor& visit) {
        const auto* bytes = reinterpret_cast<PCRE2_SPTR>(subject.data());
        while (position <= subject.size()) {
            // The text is valid UTF-8 and every position is on a character boundary.
            const int result = pcre2_match(impl_.code.get(), bytes, subject.size(), position,
                                           PCRE2_NO_UTF_CHECK, data_.get(), context_.get());
            if (result == PCRE2_ERROR_NOMATCH) {
                break;
            }
            if (result < 0) {
                return std::unexpected(Error{ErrorCode::invalid_argument, engine_message(result)});
            }
            const PCRE2_SIZE* ovector = pcre2_get_ovector_pointer(data_.get());
            const std::size_t begin = ovector[0];
            const std::size_t end = ovector[1];
            if (base + begin >= own_limit) {
                break;
            }
            if (impl_.query.whole_word && !is_whole_word(subject, begin, end)) {
                if (begin >= subject.size()) {
                    break;
                }
                position = begin + char_length(subject[begin]);
                continue;
            }
            const Found found{base + begin, base + end, subject, ovector,
                              pcre2_get_ovector_count(data_.get())};
            if (!visit(found)) {
                return Progress{true, base + end};
            }
            if (end > begin) {
                position = end;
            } else {
                position =
                    end < subject.size() ? end + char_length(subject[end]) : subject.size() + 1;
            }
        }
        return Progress{false, std::max(own_limit, base + std::min(position, subject.size()))};
    }

    const SearchImpl& impl_;
    const Text& text_;
    std::stop_token stop_;
    MatchData data_;
    MatchContext context_;
    JitStack jit_stack_;
};

struct ReplacementPiece {
    std::string literal;
    // A reference to a group, as written: `one` is the single-digit reading and `two` the
    // two-digit reading, or -1.
    int one = -1;
    int two = -1;
    // The text as it was written, used when the group does not exist.
    std::string written;
};

std::vector<ReplacementPiece> parse_replacement(std::string_view replacement) {
    std::vector<ReplacementPiece> pieces;
    const auto append_literal = [&pieces](std::string_view text) {
        if (pieces.empty() || pieces.back().one >= 0) {
            pieces.emplace_back();
        }
        pieces.back().literal += text;
    };
    for (std::size_t i = 0; i < replacement.size(); ++i) {
        const char c = replacement[i];
        const char next = i + 1 < replacement.size() ? replacement[i + 1] : '\0';
        if (c == '\\' && next == 'n') {
            append_literal("\n");
            ++i;
        } else if (c == '\\' && next == 't') {
            append_literal("\t");
            ++i;
        } else if (c == '\\' && next == '\\') {
            append_literal("\\");
            ++i;
        } else if (c == '$' && next == '$') {
            append_literal("$");
            ++i;
        } else if (c == '$' && next == '&') {
            pieces.push_back({{}, 0, -1, "$&"});
            ++i;
        } else if (c == '$' && next >= '0' && next <= '9') {
            ReplacementPiece piece;
            piece.one = next - '0';
            piece.written = std::string("$") + next;
            const char after = i + 2 < replacement.size() ? replacement[i + 2] : '\0';
            if (after >= '0' && after <= '9') {
                piece.two = piece.one * 10 + (after - '0');
                piece.written += after;
            }
            i += piece.two >= 0 ? 2 : 1;
            pieces.push_back(std::move(piece));
        } else {
            append_literal(std::string_view(&replacement[i], 1));
        }
    }
    return pieces;
}

std::string expand(const std::vector<ReplacementPiece>& pieces, const Found& found) {
    std::string result;
    for (const ReplacementPiece& piece : pieces) {
        result += piece.literal;
        if (piece.one < 0) {
            continue;
        }
        const auto count = static_cast<int>(found.pairs);
        if (piece.two >= 0 && piece.two < count) {
            result += found.group(static_cast<std::size_t>(piece.two));
        } else if (piece.one < count) {
            result += found.group(static_cast<std::size_t>(piece.one));
            if (piece.two >= 0) {
                result += piece.written.back();
            }
        } else {
            result += piece.written;
        }
    }
    return result;
}

} // namespace

std::expected<Search, Error> Search::create(Query query, std::size_t window_bytes) {
    auto impl = std::make_shared<detail::SearchImpl>();
    impl->window_bytes = std::max<std::size_t>(window_bytes, 1);
    if (!query.pattern.empty()) {
        std::uint32_t options = PCRE2_UTF;
        if (!query.match_case) {
            options |= PCRE2_CASELESS;
        }
        // \C would match single bytes and could end a match inside a character.
        options |= query.regex ? PCRE2_NEVER_BACKSLASH_C : PCRE2_LITERAL;
        int error_code = 0;
        PCRE2_SIZE error_offset = 0;
        impl->code.reset(pcre2_compile(reinterpret_cast<PCRE2_SPTR>(query.pattern.data()),
                                       query.pattern.size(), options, &error_code, &error_offset,
                                       nullptr));
        if (!impl->code) {
            std::string message = engine_message(error_code);
            if (query.regex) {
                message += std::format(" (at offset {})", error_offset);
            }
            return std::unexpected(Error{ErrorCode::invalid_argument, std::move(message)});
        }
        impl->jit = pcre2_jit_compile(impl->code.get(), PCRE2_JIT_COMPLETE) == 0;
        // Four bytes for the character after a match, which whole-word checks look at.
        impl->max_match_bytes = 4 * query.pattern.size() + 4;
    }
    impl->query = std::move(query);
    return Search(std::move(impl));
}

const Query& Search::query() const {
    return impl_->query;
}

std::expected<std::optional<Hit>, Error> Search::find_next(const Text& text, std::size_t from,
                                                           bool wrap) const {
    if (!impl_->code) {
        return std::nullopt;
    }
    Scanner scanner(*impl_, text, {});
    std::optional<Match> found;
    const auto first = [&found](const Found& match) {
        found = Match{match.begin, match.end};
        return false;
    };
    const std::size_t end = text.size();
    if (auto scanned = scanner.scan(from, end + 1, end, first); !scanned) {
        return std::unexpected(std::move(scanned.error()));
    }
    if (found) {
        return Hit{*found, false};
    }
    if (wrap && from > 0) {
        if (auto scanned = scanner.scan(0, from, end, first); !scanned) {
            return std::unexpected(std::move(scanned.error()));
        }
        if (found) {
            return Hit{*found, true};
        }
    }
    return std::nullopt;
}

std::expected<std::optional<Hit>, Error> Search::find_previous(const Text& text, std::size_t before,
                                                               bool wrap) const {
    if (!impl_->code) {
        return std::nullopt;
    }
    Scanner scanner(*impl_, text, {});
    const std::size_t end = text.size();
    std::optional<Match> found;
    const auto latest = [&found](const Found& match) {
        found = Match{match.begin, match.end};
        return true;
    };

    // Scans backwards in blocks that start on a line start, so that the matches of a block are
    // the same whatever lies before it.
    const auto last_before = [&](std::size_t limit) -> std::expected<bool, Error> {
        limit = std::min(limit, end + 1);
        while (limit > 0) {
            const std::size_t probe = std::min(limit, end);
            const std::size_t block_begin = text.line_start(
                text.position_at(probe - std::min(probe, impl_->window_bytes)).line);
            if (auto scanned = scanner.scan(block_begin, limit, end, latest); !scanned) {
                return std::unexpected(std::move(scanned.error()));
            }
            if (found) {
                return true;
            }
            limit = block_begin;
        }
        return false;
    };

    auto result = last_before(before);
    if (!result) {
        return std::unexpected(std::move(result.error()));
    }
    if (*result) {
        return Hit{*found, false};
    }
    if (wrap && before <= end) {
        result = last_before(end + 1);
        if (!result) {
            return std::unexpected(std::move(result.error()));
        }
        if (*result) {
            return Hit{*found, true};
        }
    }
    return std::nullopt;
}

std::expected<MatchList, Error> Search::find_all(const Text& text, std::size_t begin,
                                                 std::size_t end, std::size_t limit,
                                                 std::stop_token stop) const {
    MatchList list;
    if (!impl_->code) {
        return list;
    }
    end = std::min(end, text.size());
    Scanner scanner(*impl_, text, std::move(stop));
    const auto collect = [&list, limit](const Found& match) {
        if (list.matches.size() >= limit) {
            list.truncated = true;
            return false;
        }
        list.matches.push_back({match.begin, match.end});
        return true;
    };
    if (auto scanned = scanner.scan(begin, end + 1, end, collect); !scanned) {
        return std::unexpected(std::move(scanned.error()));
    }
    return list;
}

std::expected<std::optional<std::string>, Error>
Search::replacement_for(const Text& text, Match match, std::string_view replacement) const {
    if (!impl_->code || match.begin > text.size()) {
        return std::nullopt;
    }
    const auto pieces =
        impl_->query.regex ? parse_replacement(replacement) : std::vector<ReplacementPiece>{};
    Scanner scanner(*impl_, text, {});
    std::optional<std::string> result;
    const auto visit = [&](const Found& found) {
        if (found.begin == match.begin && found.end == match.end) {
            result = impl_->query.regex ? expand(pieces, found) : std::string(replacement);
        }
        return false;
    };
    if (auto scanned = scanner.scan(match.begin, match.begin + 1, text.size(), visit); !scanned) {
        return std::unexpected(std::move(scanned.error()));
    }
    return result;
}

std::expected<std::vector<Edit>, Error>
Search::replace_all(const Text& text, std::string_view replacement, std::stop_token stop) const {
    std::vector<Edit> edits;
    if (!impl_->code) {
        return edits;
    }
    const auto pieces =
        impl_->query.regex ? parse_replacement(replacement) : std::vector<ReplacementPiece>{};
    Scanner scanner(*impl_, text, std::move(stop));
    const auto visit = [&](const Found& found) {
        edits.push_back({found.begin, found.end,
                         impl_->query.regex ? expand(pieces, found) : std::string(replacement)});
        return true;
    };
    if (auto scanned = scanner.scan(0, text.size() + 1, text.size(), visit); !scanned) {
        return std::unexpected(std::move(scanned.error()));
    }
    return edits;
}

} // namespace anvil::core
