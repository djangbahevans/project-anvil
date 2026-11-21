#pragma once

#include "anvil/core/error.hpp"

#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace anvil::core {

// The value of a context key. A list holds the strings that the `in` operator searches.
using ContextValue = std::variant<bool, double, std::string, std::vector<std::string>>;

// The context keys that `when` expressions read. A key that was never set has no value and is
// falsy.
class ContextKeys {
public:
    void set(std::string key, ContextValue value);
    void unset(std::string_view key);
    const ContextValue* find(std::string_view key) const;

private:
    std::map<std::string, ContextValue, std::less<>> values_;
};

namespace detail {
struct WhenImpl;
}

// A `when` expression, the condition that decides whether a keybinding, menu item or command
// applies: `editorTextFocus && !editorReadonly`, `resourceExtname == .md`,
// `config.editor.minimap.enabled`. The syntax and the evaluation follow VS Code's context key
// expressions, which loosely coerce operands the way JavaScript does:
//
//  - A bare key is true when its value is truthy: `true`, a non-zero number, a non-empty string,
//    or any list.
//  - `==` and `===` are the same operator, and so are `!=` and `!==`. The operands are compared
//    as with JavaScript's `==`: the value of the key is compared with the written text, converting
//    booleans and numbers to numbers, and a list to its items joined by commas. An unset key
//    equals nothing. `key == true` and `key == false` mean `key` and `!key`; a quoted
//    `key == 'true'` compares text.
//  - `<`, `<=`, `>` and `>=` read the value of the key as a number, the way `parseFloat` does,
//    and compare it with a number. A comparison with a value that is not a number is false.
//  - `key =~ /pattern/flags` is true when the pattern matches somewhere in the value of the key
//    converted to text (an unset key becomes "undefined"). The pattern is a PCRE2 regular
//    expression with the JavaScript differences that matter for patterns: `$` matches only at the
//    end, `\uXXXX` is allowed, and a reference to a group that did not take part matches the
//    empty string. The flags `i`, `m`, `s` and `u` are accepted, `g` and `y` are accepted and
//    ignored, and `u` has no effect. A pattern that does not compile is a parse error.
//  - `key in listKey` is true when the value of `key` is a string that is an item of the list in
//    `listKey`; `key not in listKey` is its negation.
//
// Precedence is `!`, then `&&`, then `||`. `!` applies to a key, `true`, `false` or a
// parenthesized expression. An expression that is empty or only white space is true.
//
// Expressions are normalized as they are parsed: `&&` is distributed over `||` so that the result
// is an `||` of `&&` groups, negation is pushed down to the keys, terms are sorted and
// de-duplicated, and `a && !a` and `a || !a` collapse to `false` and `true`. Two spellings of the
// same expression therefore have the same canonical string.
//
// Copying is cheap, and an expression is safe to evaluate from several threads at once.
class WhenExpression {
public:
    // The expression that is always true.
    WhenExpression();

    // Fails with ErrorCode::invalid_argument. The message begins with the byte offset of the
    // problem, "offset N: ", counted from zero.
    static std::expected<WhenExpression, Error> parse(std::string_view text);

    bool evaluate(const ContextKeys& keys) const;

    // The normalized expression, which parses to an equal expression.
    const std::string& canonical() const;

    bool operator==(const WhenExpression& other) const {
        return canonical() == other.canonical();
    }

private:
    explicit WhenExpression(std::shared_ptr<const detail::WhenImpl> impl)
        : impl_(std::move(impl)) {}

    std::shared_ptr<const detail::WhenImpl> impl_;
};

} // namespace anvil::core
