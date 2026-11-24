#include "anvil/core/brackets.hpp"

#include <algorithm>
#include <limits>
#include <string_view>

namespace anvil::core {
namespace {

constexpr std::size_t none = std::numeric_limits<std::size_t>::max();

constexpr std::string_view openers = "([{";
constexpr std::string_view closers = ")]}";

bool touches(std::size_t bracket, std::size_t offset) {
    return offset == bracket || offset == bracket + 1;
}

} // namespace

std::vector<BracketPair> bracket_pairs(std::string_view text) {
    struct Open {
        std::size_t kind;
        std::size_t pair;
    };
    std::vector<BracketPair> pairs;
    std::vector<Open> stack;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (const std::size_t kind = openers.find(text[i]); kind != std::string_view::npos) {
            pairs.push_back({i, none});
            stack.push_back({kind, pairs.size() - 1});
        } else if (const std::size_t closing = closers.find(text[i]);
                   closing != std::string_view::npos) {
            const auto match =
                std::find_if(stack.rbegin(), stack.rend(),
                             [closing](const Open& open) { return open.kind == closing; });
            if (match != stack.rend()) {
                pairs[match->pair].close = i;
                stack.erase(std::prev(match.base()), stack.end());
            }
        }
    }
    std::erase_if(pairs, [](const BracketPair& pair) { return pair.close == none; });
    return pairs;
}

std::size_t jump_to_bracket(std::string_view text, std::size_t offset) {
    const std::vector<BracketPair> pairs = bracket_pairs(text);

    const BracketPair* touched = nullptr;
    std::size_t touched_start = 0;
    for (const BracketPair& pair : pairs) {
        const std::size_t start = touches(pair.open, offset) ? pair.open : pair.close;
        if ((touches(pair.open, offset) || touches(pair.close, offset)) &&
            (!touched || start >= touched_start)) {
            touched = &pair;
            touched_start = start;
        }
    }
    if (touched) {
        if (touches(touched->open, offset) && !touches(touched->close, offset)) {
            return touched->close;
        }
        return touched->open;
    }

    const BracketPair* enclosing = nullptr;
    for (const BracketPair& pair : pairs) {
        if (pair.open < offset && offset < pair.close) {
            enclosing = &pair;
        }
    }
    if (enclosing) {
        return enclosing->close;
    }

    for (std::size_t i = std::min(offset, text.size()); i < text.size(); ++i) {
        if (openers.find(text[i]) != std::string_view::npos ||
            closers.find(text[i]) != std::string_view::npos) {
            return i;
        }
    }
    return offset;
}

} // namespace anvil::core
