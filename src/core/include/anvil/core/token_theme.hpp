#pragma once

#include "anvil/core/color_theme.hpp"

#include <cstdint>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace anvil::core {

// The bits of a font style.
struct FontStyle {
    static constexpr std::uint8_t italic = 1;
    static constexpr std::uint8_t bold = 2;
    static constexpr std::uint8_t underline = 4;
    static constexpr std::uint8_t strikethrough = 8;
};

// What a theme says about how a token looks. A field that is not set means the theme has no
// opinion, and the editor's own foreground, background and plain style apply.
struct TokenStyle {
    std::optional<Rgba> foreground;
    std::optional<Rgba> background;
    // A set of FontStyle bits. Set and empty, written `"fontStyle": ""`, clears what an enclosing
    // scope set.
    std::optional<std::uint8_t> font_style;

    friend bool operator==(const TokenStyle&, const TokenStyle&) = default;
};

// The `tokenColors` of a theme: rules that give a style to the scopes of the tokens a highlighter
// produces. It reproduces how VS Code styles TextMate tokens, which is `vscode-textmate`'s theme:
// - A rule's `scope` is a string of comma-separated selectors or an array of selectors, or absent;
//   a selector, or none, which is the empty scope, applies to every token and is how a theme gives
//   its default style. A selector is words separated by one space: the last is the scope the token
//   must have, and the words before it must name ancestors, in that order and not necessarily next
//   to each other; `>` before a word asks for the very next ancestor.
// - A selector word names a scope when it equals the scope or is a prefix that ends at a `.`:
//   `entity.name` names `entity.name.function`, and `entity.namex` does not.
// - Several rules can name a scope. The more specific wins: the more segments in the rule's scope,
//   then the longer words of its ancestors, then the more ancestors; rules that tie keep the order
//   of their scope in the sorted theme and then the order they have in the file, so a later rule of
//   the same scope wins. A rule leaves the fields it does not set to the rules of shorter scopes
//   that name the same selector, as `entity.name.function` leaves its color to `entity.name`.
// - The style of a token is the default style with the matching rule of each scope of its stack
//   applied from the outermost to the innermost, so a scope without a rule shows what its
//   enclosing scopes gave it.
// - `fontStyle` is a space-separated subset of `italic`, `bold`, `underline` and `strikethrough`;
//   colors are `#rgb`, `#rgba`, `#rrggbb` or `#rrggbbaa`, and an invalid one is ignored.
//
// Unlike `vscode-textmate`, a theme with no default rule leaves the default foreground and
// background unset instead of making them black and white, because the editor knows its colors.
class TokenTheme {
public:
    // Reads the rules, as `ColorTheme::token_colors()` returns them. An entry that is not an object
    // or a scope that is not a string is skipped, and a color that is not valid is ignored; each is
    // recorded in `warnings()`.
    static TokenTheme from_json(const nlohmann::json& rules);

    TokenTheme(TokenTheme&&) noexcept;
    TokenTheme& operator=(TokenTheme&&) noexcept;
    ~TokenTheme();

    // The style of a token whose scopes are `scope_stack`, from the outermost to the innermost.
    // Does not allocate.
    TokenStyle style_for(std::span<const std::string_view> scope_stack) const;

    // The style of a token that has no rule at all: what the rules without a scope say.
    const TokenStyle& defaults() const {
        return defaults_;
    }

    const std::vector<std::string>& warnings() const {
        return warnings_;
    }

private:
    struct Node;

    TokenTheme();

    std::unique_ptr<Node> root_;
    TokenStyle defaults_;
    std::vector<std::string> warnings_;
};

} // namespace anvil::core
