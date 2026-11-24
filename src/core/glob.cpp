#include "anvil/core/glob.hpp"

#include <string>
#include <vector>

namespace anvil::core {
namespace {

// The index of the `}` that closes the `{` at `open`, or npos.
std::size_t closing_brace(std::string_view pattern, std::size_t open) {
    int depth = 0;
    for (std::size_t i = open; i < pattern.size(); ++i) {
        if (pattern[i] == '{') {
            ++depth;
        } else if (pattern[i] == '}' && --depth == 0) {
            return i;
        }
    }
    return std::string_view::npos;
}

// Replaces the first group of alternatives by each alternative in turn, until none is left.
void expand_braces(const std::string& pattern, std::vector<std::string>& out) {
    for (std::size_t open = pattern.find('{'); open != std::string::npos;
         open = pattern.find('{', open + 1)) {
        const std::size_t close = closing_brace(pattern, open);
        if (close == std::string_view::npos) {
            continue;
        }
        const std::string head = pattern.substr(0, open);
        const std::string tail = pattern.substr(close + 1);
        std::size_t start = open + 1;
        int depth = 0;
        for (std::size_t i = open + 1; i <= close; ++i) {
            if (pattern[i] == '{') {
                ++depth;
            } else if (pattern[i] == '}' && i != close) {
                --depth;
            } else if ((pattern[i] == ',' && depth == 0) || i == close) {
                expand_braces(head + pattern.substr(start, i - start) + tail, out);
                start = i + 1;
            }
        }
        return;
    }
    out.push_back(pattern);
}

std::vector<std::string_view> segments(std::string_view text) {
    std::vector<std::string_view> result;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('/', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        if (end > start) {
            result.push_back(text.substr(start, end - start));
        }
        start = end + 1;
    }
    return result;
}

// Matches one pattern segment against one path segment.
bool match_segment(std::string_view pattern, std::string_view text) {
    if (pattern.empty()) {
        return text.empty();
    }
    switch (pattern[0]) {
    case '*':
        for (std::size_t skip = 0; skip <= text.size(); ++skip) {
            if (match_segment(pattern.substr(1), text.substr(skip))) {
                return true;
            }
        }
        return false;
    case '?':
        return !text.empty() && match_segment(pattern.substr(1), text.substr(1));
    case '[': {
        const std::size_t close = pattern.find(']', 2);
        if (close == std::string_view::npos) {
            break;
        }
        if (text.empty()) {
            return false;
        }
        std::string_view set = pattern.substr(1, close - 1);
        const bool negated = !set.empty() && (set[0] == '!' || set[0] == '^');
        if (negated) {
            set.remove_prefix(1);
        }
        bool found = false;
        for (std::size_t i = 0; i < set.size(); ++i) {
            if (i + 2 < set.size() && set[i + 1] == '-') {
                found = found || (set[i] <= text[0] && text[0] <= set[i + 2]);
                i += 2;
            } else {
                found = found || set[i] == text[0];
            }
        }
        return found != negated && match_segment(pattern.substr(close + 1), text.substr(1));
    }
    default:
        break;
    }
    return !text.empty() && pattern[0] == text[0] &&
           match_segment(pattern.substr(1), text.substr(1));
}

bool match_segments(const std::vector<std::string_view>& pattern, std::size_t p,
                    const std::vector<std::string_view>& path, std::size_t s) {
    if (p == pattern.size()) {
        return s == path.size();
    }
    if (pattern[p] == "**") {
        for (std::size_t skip = s; skip <= path.size(); ++skip) {
            if (match_segments(pattern, p + 1, path, skip)) {
                return true;
            }
        }
        return false;
    }
    return s < path.size() && match_segment(pattern[p], path[s]) &&
           match_segments(pattern, p + 1, path, s + 1);
}

} // namespace

bool glob_match(std::string_view pattern, std::string_view path) {
    std::vector<std::string> alternatives;
    expand_braces(std::string(pattern), alternatives);
    const std::vector<std::string_view> path_segments = segments(path);
    for (const std::string& alternative : alternatives) {
        if (match_segments(segments(alternative), 0, path_segments, 0)) {
            return true;
        }
    }
    return false;
}

} // namespace anvil::core
