#include "anvil/core/when_expression.hpp"

#include <array>
#include <chrono>
#include <gtest/gtest.h>
#include <limits>
#include <optional>
#include <random>
#include <vector>

using namespace anvil::core;
using Strings = std::vector<std::string>;

namespace {

ContextKeys keys_of(std::initializer_list<std::pair<std::string, ContextValue>> items) {
    ContextKeys keys;
    for (const auto& [key, value] : items) {
        keys.set(key, value);
    }
    return keys;
}

WhenExpression parse_ok(std::string_view text) {
    auto result = WhenExpression::parse(text);
    EXPECT_TRUE(result.has_value())
        << "'" << text << "': " << (result ? "" : result.error().message);
    return result ? *result : WhenExpression();
}

bool eval(std::string_view text, const ContextKeys& keys) {
    return parse_ok(text).evaluate(keys);
}

std::string canonical_of(std::string_view text) {
    return parse_ok(text).canonical();
}

} // namespace

TEST(ContextKeys, FindSetAndUnset) {
    ContextKeys keys;
    EXPECT_EQ(keys.find("a"), nullptr);
    keys.set("a", true);
    ASSERT_NE(keys.find("a"), nullptr);
    EXPECT_EQ(std::get<bool>(*keys.find("a")), true);
    keys.set("a", std::string("text"));
    EXPECT_EQ(std::get<std::string>(*keys.find("a")), "text");
    keys.unset("a");
    EXPECT_EQ(keys.find("a"), nullptr);
    keys.unset("never-set");
}

TEST(WhenExpression, EmptyAndWhiteSpaceAreTrue) {
    const ContextKeys none;
    for (const std::string_view text : {"", " ", " \t\r\n", "\xC2\xA0"}) {
        EXPECT_TRUE(eval(text, none)) << "'" << text << "'";
        EXPECT_EQ(canonical_of(text), "true");
    }
    EXPECT_TRUE(WhenExpression().evaluate(none));
    EXPECT_EQ(WhenExpression().canonical(), "true");
}

// The ten spellings that contextkey.test.ts evaluates for every key, in this order: the bare
// key, `== true`, `!= true`, `== false`, `!= false`, `== 5`, `!= 5`, the negation, `=~ /d.*/`
// and `=~ /D/i`. The expected values are what JavaScript gives for the same operands.
TEST(WhenExpression, EveryOperatorOnEveryKindOfValue) {
    struct Case {
        std::string key;
        std::array<bool, 10> expected;
    };
    const ContextKeys keys =
        keys_of({{"a", true}, {"b", false}, {"c", std::string("5")}, {"d", std::string("d")}});
    const std::vector<Case> cases = {
        {"a", {true, true, false, false, true, false, true, false, false, false}},
        {"b", {false, false, true, true, false, false, true, true, false, false}},
        {"c", {true, true, false, false, true, true, false, false, false, false}},
        {"d", {true, true, false, false, true, false, true, false, true, true}},
        // An unset key is converted to the text "undefined" by a regular expression test.
        {"z", {false, false, true, true, false, false, true, true, true, true}},
    };
    for (const Case& c : cases) {
        const std::string k = c.key;
        const std::vector<std::string> spellings = {
            k,           k + " == true", k + " != true", k + " == false", k + " != false",
            k + " == 5", k + " != 5",    "!" + k,        k + " =~ /d.*/", k + " =~ /D/i"};
        for (std::size_t i = 0; i < spellings.size(); ++i) {
            EXPECT_EQ(eval(spellings[i], keys), c.expected[i]) << spellings[i];
        }
    }
}

TEST(WhenExpression, LiteralsAndPrecedence) {
    const ContextKeys keys =
        keys_of({{"a", true}, {"b", false}, {"c", std::string("5")}, {"d", std::string("d")}});
    EXPECT_TRUE(eval("true", keys));
    EXPECT_FALSE(eval("false", keys));
    EXPECT_TRUE(eval("a && !b", keys));
    EXPECT_FALSE(eval("a && b", keys));
    EXPECT_TRUE(eval("a && !b && c == 5", keys));
    EXPECT_FALSE(eval("d =~ /e.*/", keys));
    // `&&` binds tighter than `||`: false && true || true is true, not false && (true || true).
    EXPECT_TRUE(eval("b && a || a", keys));
    EXPECT_TRUE(eval("a || b", keys));
    EXPECT_FALSE(eval("b || b", keys));
    EXPECT_FALSE(eval("b && a || a && b", keys));
    EXPECT_FALSE(eval("b && (a || a)", keys));
    EXPECT_TRUE(eval("!(a && b)", keys));
    EXPECT_FALSE(eval("!(a || b)", keys));
}

TEST(WhenExpression, TruthinessOfEachKindOfValue) {
    const ContextKeys keys = keys_of({{"yes", true},
                                      {"no", false},
                                      {"one", 1.0},
                                      {"zero", 0.0},
                                      {"nan", std::numeric_limits<double>::quiet_NaN()},
                                      {"text", std::string("0")},
                                      {"blank", std::string("")},
                                      {"list", Strings{"x"}},
                                      {"none", Strings{}}});
    for (const char* truthy : {"yes", "one", "text", "list", "none"}) {
        EXPECT_TRUE(eval(truthy, keys)) << truthy;
        EXPECT_FALSE(eval(std::string("!") + truthy, keys)) << truthy;
    }
    for (const char* falsy : {"no", "zero", "nan", "blank", "unset"}) {
        EXPECT_FALSE(eval(falsy, keys)) << falsy;
        EXPECT_TRUE(eval(std::string("!") + falsy, keys)) << falsy;
    }
}

TEST(WhenExpression, EqualityCoercesLikeJavaScript) {
    struct Case {
        const char* expression;
        ContextValue value;
        bool expected;
    };
    const std::vector<Case> cases = {
        {"k == abc", std::string("abc"), true},
        {"k == abc", std::string("ABC"), false},
        {"k != abc", std::string("ABC"), true},
        {"k == 5", 5.0, true},
        {"k == 5.0", 5.0, true},
        {"k == 5", std::string("5"), true},
        {"k == 5", std::string("05"), false},
        {"k == 0x10", 16.0, true},
        {"k == 1e2", 100.0, true},
        {"k == 5", true, false},
        {"k == 1", true, true},
        {"k == 0", false, true},
        {"k == ", false, true},
        {"k == ", 0.0, true},
        {"k == ", std::string(""), true},
        {"k == ", std::string("x"), false},
        {"k == x", 5.0, false},
        {"k != x", 5.0, true},
        // A list is compared as its items joined by commas.
        {"k == x,y", Strings{"x", "y"}, true},
        {"k == x", Strings{"x"}, true},
        {"k == x", Strings{"x", "y"}, false},
        // Quoting keeps `true` and `false` as text.
        {"k == 'true'", std::string("true"), true},
        {"k == 'true'", true, false},
        {"k != 'true'", true, true},
        {"k == 'a b'", std::string("a b"), true},
        {"k == \"q\"", std::string("\"q\""), true},
        {"k === abc", std::string("abc"), true},
        {"k !== abc", std::string("abc"), false},
    };
    for (const Case& c : cases) {
        EXPECT_EQ(eval(c.expression, keys_of({{"k", c.value}})), c.expected)
            << c.expression << " with value index " << c.value.index();
    }
    const ContextKeys none;
    EXPECT_FALSE(eval("k == abc", none));
    EXPECT_FALSE(eval("k == ", none));
    EXPECT_TRUE(eval("k != abc", none));
    EXPECT_TRUE(eval("k != ", none));
}

TEST(WhenExpression, InAndNotIn) {
    const WhenExpression in = parse_ok("a in b");
    const WhenExpression not_in = parse_ok("a not in b");
    struct Case {
        ContextKeys keys;
        bool in;
    };
    const std::vector<Case> cases = {
        {keys_of({{"a", std::string("x")}, {"b", Strings{"x"}}}), true},
        {keys_of({{"a", std::string("x")}, {"b", Strings{"w", "x", "y"}}}), true},
        {keys_of({{"a", std::string("x")}, {"b", Strings{"y"}}}), false},
        {keys_of({{"a", std::string("x")}, {"b", Strings{}}}), false},
        {keys_of({{"a", std::string("x")}}), false},
        {keys_of({{"b", Strings{"x"}}}), false},
        {keys_of({}), false},
        // Only a string can be an item of a list of strings.
        {keys_of({{"a", 3.0}, {"b", Strings{"3"}}}), false},
        {keys_of({{"a", true}, {"b", Strings{"true"}}}), false},
        // The collection must be a list.
        {keys_of({{"a", std::string("x")}, {"b", std::string("x")}}), false},
        {keys_of({{"a", std::string("x")}, {"b", true}}), false},
    };
    for (std::size_t i = 0; i < cases.size(); ++i) {
        EXPECT_EQ(in.evaluate(cases[i].keys), cases[i].in) << "case " << i;
        EXPECT_EQ(not_in.evaluate(cases[i].keys), !cases[i].in) << "case " << i;
    }
}

TEST(WhenExpression, NumericComparisons) {
    struct Case {
        const char* expression;
        std::optional<ContextValue> value;
        bool expected;
    };
    const std::vector<Case> cases = {
        {"a > 1", std::nullopt, false},
        {"a > 1", 0.0, false},
        {"a > 1", 1.0, false},
        {"a > 1", 2.0, true},
        {"a > 1", std::string("0"), false},
        {"a > 1", std::string("1"), false},
        {"a > 1", std::string("2"), true},
        {"a > 1", std::string("a"), false},
        {"a > 10", 2.0, false},
        {"a > 10", 11.0, true},
        {"a > 10", std::string("11"), true},
        {"a > 10", std::string("2"), false},
        {"a > 1.1", 1.0, false},
        {"a > 1.1", 2.0, true},
        {"a > 1.1", std::string("1.1"), false},
        {"a > 1.1", std::string("11"), true},
        {"a > b", std::string("b"), false},
        {"a > b", std::string("c"), false},
        {"a > b", 1000.0, false},
        {"a >= 2", std::string("1"), false},
        {"a >= 2", std::string("2"), true},
        {"a >= 2", std::string("3"), true},
        {"a < 2", std::string("1"), true},
        {"a < 2", std::string("2"), false},
        {"a < 2", std::string("3"), false},
        {"a <= 2", std::string("1"), true},
        {"a <= 2", std::string("2"), true},
        {"a <= 2", std::string("3"), false},
        {"a >= -1", -1.0, true},
        {"a < -1", -2.0, true},
        // The key is read with parseFloat: the numeric prefix counts, a boolean does not.
        {"a > 1", std::string("3px"), true},
        {"a > 1", std::string("  3"), true},
        {"a > 1", true, false},
        {"a < 1", std::string("x"), false},
        {"a >= 0", Strings{"3"}, true},
        // The operand is read with parseFloat too.
        {"a > 1px", 2.0, true},
    };
    for (const Case& c : cases) {
        ContextKeys keys;
        if (c.value) {
            keys.set("a", *c.value);
        }
        EXPECT_EQ(eval(c.expression, keys), c.expected)
            << c.expression
            << (c.value ? " with value index " + std::to_string(c.value->index())
                        : " with no value");
    }
}

TEST(WhenExpression, RegularExpressions) {
    struct Case {
        const char* expression;
        ContextValue value;
        bool expected;
    };
    const std::vector<Case> cases = {
        {"k =~ /^b.*z$/", std::string("buzz"), true},
        {"k =~ /^b.*z$/", std::string("buzzy"), false},
        // A pattern is not anchored.
        {"k =~ /uz/", std::string("buzz"), true},
        {"k =~ /UZ/", std::string("buzz"), false},
        {"k =~ /UZ/i", std::string("buzz"), true},
        {"k =~ /\xC3\xA9/i", std::string("\xC3\x89"), true},
        {"k =~ /^y/", std::string("x\ny"), false},
        {"k =~ /^y/m", std::string("x\ny"), true},
        {"k =~ /x.y/", std::string("x\ny"), false},
        {"k =~ /x.y/s", std::string("x\ny"), true},
        {"k =~ /x$/", std::string("x\n"), false},
        {"k =~ /\\u0041/", std::string("A"), true},
        {"k =~ /\\d+/", std::string("\xD9\xA3"), false},
        {"k =~ /(a)|\\1b/", std::string("b"), true},
        {"k =~ /a\\/b/", std::string("a/b"), true},
        {"k =~ /[/]x/", std::string("/x"), true},
        {"k =~ /\\.md$/", std::string("readme.md"), true},
        {"k =~ /\\.md$/", std::string("readme.mdx"), false},
        {"k =~ /x/gy", std::string("x"), true},
        // The subject is the value converted to text, as in JavaScript.
        {"k =~ /^5$/", 5.0, true},
        {"k =~ /^5.5$/", 5.5, true},
        {"k =~ /^true$/", true, true},
        {"k =~ /^false$/", false, true},
        {"k =~ /^x,y$/", Strings{"x", "y"}, true},
        {"!(k =~ /^x/)", std::string("xy"), false},
        {"!(k =~ /^x/)", std::string("y"), true},
    };
    for (const Case& c : cases) {
        EXPECT_EQ(eval(c.expression, keys_of({{"k", c.value}})), c.expected)
            << c.expression << " with value index " << c.value.index();
    }
    EXPECT_TRUE(eval("k =~ /^undefined$/", ContextKeys()));
}

TEST(WhenExpression, RegularExpressionThatBacktracksTooMuchDoesNotMatch) {
    const WhenExpression expression = parse_ok("k =~ /(a+)+$/");
    const ContextKeys keys = keys_of({{"k", std::string(40, 'a') + "b"}});
    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(expression.evaluate(keys));
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(5));
}

TEST(WhenExpression, ReadsContextKeysAtEveryEvaluation) {
    const WhenExpression expression = parse_ok("a && b == x");
    ContextKeys keys;
    EXPECT_FALSE(expression.evaluate(keys));
    keys.set("a", true);
    keys.set("b", std::string("x"));
    EXPECT_TRUE(expression.evaluate(keys));
    keys.set("b", std::string("y"));
    EXPECT_FALSE(expression.evaluate(keys));
}

namespace {

struct Normalization {
    const char* input;
    const char* canonical;
};

// Expected results are the serializations that VS Code's parser and normalizer tests state.
const std::vector<Normalization> normalizations = {
    {" foo", "foo"},
    {"!foo", "!foo"},
    {"foo =~ /bar/", "foo =~ /bar/"},
    {"foo || (foo =~ /bar/ && baz)", "foo || baz && foo =~ /bar/"},
    {"foo || (foo =~ /bar/ || baz)", "baz || foo || foo =~ /bar/"},
    {"(foo || bar) && (jee || jar)", "bar && jar || bar && jee || foo && jar || foo && jee"},
    {"foo && foo =~ /zee/i", "foo && foo =~ /zee/i"},
    {"foo.bar==enabled", "foo.bar == 'enabled'"},
    {"b == a && a == z", "a == 'z' && b == 'a'"},
    {"foo.bar == 'enabled'", "foo.bar == 'enabled'"},
    {"foo.bar:zed==completed", "foo.bar:zed == 'completed'"},
    {"a && b || c", "c || a && b"},
    {"fooBar && baz.jar && fee.bee<K-loo+1>", "baz.jar && fee.bee<K-loo+1> && fooBar"},
    {"foo.barBaz<C-r> < 2", "foo.barBaz<C-r> < 2"},
    {"foo.bar >= -1", "foo.bar >= -1"},
    {"foo.bar <= -1", "foo.bar <= -1"},
    {"view == vsc-packages-activitybar-folders && vsc-packages-folders-loaded",
     "vsc-packages-folders-loaded && view == 'vsc-packages-activitybar-folders'"},
    {"!cmake:hideBuildCommand && cmake:enableFullFeatureSet",
     "cmake:enableFullFeatureSet && !cmake:hideBuildCommand"},
    {"!(foo && bar)", "!bar || !foo"},
    {"!(foo && bar || boar) || deer", "deer || !bar && !boar || !boar && !foo"},
    {"!(!foo)", "foo"},
    {"debugState == \"stopped\"", "debugState == '\"stopped\"'"},
    {"resourcePath =~ /.md(.yml|.txt)*$/giym", "resourcePath =~ /.md(.yml|.txt)*$/im"},
    {"resource =~ /\\/foo\\/(a|b)$/", "resource =~ /\\/foo\\/(a|b)$/"},
    {"editorTextFocus && vim.active && vim.use<C-r>",
     "editorTextFocus && vim.active && vim.use<C-r>"},

    // Booleans, `==` and `!=` against true and false, and the constants.
    {"key1 == true", "key1"},
    {"key1 != false", "key1"},
    {"key1 == false", "!key1"},
    {"key1 != true", "!key1"},
    {"key1 == 'true'", "key1 == 'true'"},
    {"key1 != 'false'", "key1 != 'false'"},
    {"key1 === x", "key1 == 'x'"},
    {"key1 !== x", "key1 != 'x'"},
    {"languageId == in", "languageId == 'in'"},
    {"k ==", "k == ''"},
    {"true", "true"},
    {"!true", "false"},
    {"false", "false"},
    {"!false", "true"},
    {"a && true", "a"},
    {"a && false", "false"},
    {"a || true", "true"},
    {"a || false", "a"},
    {"true && true", "true"},
    {"false || false", "false"},
    {"(true)", "true"},

    // Duplicates and complementary terms.
    {"A || B || A", "A || B"},
    {"A && B && A", "A && B"},
    {"A || !A", "true"},
    {"A && !A", "false"},
    {"k == x || k != x", "true"},
    {"k =~ /x/ && !(k =~ /x/)", "false"},
    {"a < 1 && a >= 1", "false"},
    {"a in b && a not in b", "false"},
    {"k =~ /x/i && k =~ /x/", "k =~ /x/ && k =~ /x/i"},

    // `&&` is distributed over `||`.
    {"a && b", "a && b"},
    {"(a || b) && c", "a && c || b && c"},
    {"(a || b) && (c || d)", "a && c || a && d || b && c || b && d"},
    {"(a || b) && c && d", "a && c && d || b && c && d"},
    {"(a || b) && (c && d || e)", "a && e || b && e || a && c && d || b && c && d"},
    {"(a || b) && c > 1", "a && c > 1 || b && c > 1"},
    {"(a || b) && c in d", "a && c in 'd' || b && c in 'd'"},
    {"a && b && (!b || !b && c)", "false"},
    {"a && b && (!b || a && c)", "a && b && c"},
    {"A && (B1 || B2)", "A && B1 || A && B2"},

    // Negation.
    {"!(true)", "false"},
    {"!(false)", "true"},
    {"!(a)", "!a"},
    {"!(a && b || c)", "!a && !c || !b && !c"},
    {"!(a && b || c || d)", "!a && !c && !d || !b && !c && !d"},
    {"!(!a && !b || !c && !d)", "a && c || a && d || b && c || b && d"},
    {"!(!a && !b || !c && !d || !e && !f)",
     "a && c && e || a && c && f || a && d && e || a && d && f || b && c && e || b && c && f || "
     "b && d && e || b && d && f"},
    {"!(A && (B1 || B2))", "!A || !A && !B1 || !A && !B2 || !B1 && !B2"},
    {"!(!(A && (B1 || B2)))", "A && B1 || A && B2"},
    {"!(!(!(A && (B1 || B2))))", "!A || !A && !B1 || !A && !B2 || !B1 && !B2"},
    {"!(a > 1)", "a <= 1"},
    {"!(a > 1.1)", "a <= 1.1"},
    {"!(a > b)", "a <= b"},
    {"!(a >= 1)", "a < 1"},
    {"!(a >= b)", "a < b"},
    {"!(a < 1)", "a >= 1"},
    {"!(a < b)", "a >= b"},
    {"!(a <= 1)", "a > 1"},
    {"!(a <= b)", "a > b"},
    {"!(a in b)", "a not in 'b'"},
    {"!(a not in b)", "a in 'b'"},
    {"!(k == x)", "k != 'x'"},
    {"!(k =~ /x/i)", "!(k =~ /x/i)"},
    {"!(!(k =~ /x/i))", "k =~ /x/i"},

    // Numbers are written in one form.
    {"a > 5.0", "a > 5"},
    {"a > 5", "a > 5"},
    {"a >= 0.50", "a >= 0.5"},
    {"a < 1e2", "a < 100"},
    {"a <= 0", "a <= 0"},
    {"a > .5", "a > 0.5"},
    {"a > 5.", "a > 5"},
    {"a > 1px", "a > 1"},
    {"a > b", "a > b"},
};

} // namespace

TEST(WhenExpression, NormalizesToTheCanonicalString) {
    for (const Normalization& n : normalizations) {
        EXPECT_EQ(canonical_of(n.input), n.canonical) << n.input;
    }
}

TEST(WhenExpression, CanonicalStringParsesToTheSameExpression) {
    for (const Normalization& n : normalizations) {
        const WhenExpression expression = parse_ok(n.input);
        const WhenExpression again = parse_ok(expression.canonical());
        EXPECT_EQ(again.canonical(), expression.canonical()) << n.input;
        EXPECT_EQ(again, expression) << n.input;
    }
}

TEST(WhenExpression, SpellingsOfOneExpressionAreEqual) {
    const WhenExpression a =
        parse_ok("a1 && and.a && a2 && d3 =~ /d.*/ && d4 =~ /\\*\\*3*/ && "
                 "b1 == bb1 && b2 == bb2 && c1 != cc1 && c2 != cc2 && !d1 && !d2");
    const WhenExpression b = parse_ok("b2 == bb2 && c1 != cc1 && !d1 && d4 =~ /\\*\\*3*/ && "
                                      "c2 != cc2 && a2 && b1 == bb1 && d3 =~ /d.*/ && a1 && "
                                      "(and.a == true) && !d2");
    EXPECT_EQ(a, b);
    EXPECT_EQ(parse_ok("a && b"), parse_ok("b&&a"));
    EXPECT_EQ(parse_ok("a || b"), parse_ok("(b) || (a)"));
    EXPECT_EQ(parse_ok("!(a || b)"), parse_ok("!a && !b"));
    EXPECT_EQ(parse_ok("a == 'x'"), parse_ok("a==x"));
    EXPECT_EQ(parse_ok("a > 1"), parse_ok("a > 1.0"));
    EXPECT_FALSE(parse_ok("a && b") == parse_ok("a || b"));
    EXPECT_FALSE(parse_ok("a == x") == parse_ok("a == y"));
    EXPECT_FALSE(parse_ok("k =~ /x/") == parse_ok("k =~ /x/i"));
    EXPECT_FALSE(parse_ok("a > 1") == parse_ok("a >= 1"));
}

namespace {

struct ParseFailure {
    const char* input;
    std::size_t offset;
};

} // namespace

TEST(WhenExpression, ParseErrorsCarryThePositionOfTheProblem) {
    const std::vector<ParseFailure> failures = {
        {"/foo", 0},
        {"!b == 'true'", 3},
        {"!foo &&  in bar", 9},
        {"vim<c-r> == 1 && vim<2<=3", 23},
        {"foo && 'bar", 7},
        {"config.foo &&  &&bar =~ /^foo$|^bar-foo$|^joo$|^jar$/ && !foo", 15},
        {"!foo == 'test'", 5},
        {"!!foo", 1},
        {" viewItem == VSCode WorkSpace", 20},
        {"a &&", 4},
        {"a ||", 4},
        {"&& a", 0},
        {"!", 1},
        {"(a", 2},
        {"(a && b", 7},
        {"a)", 1},
        {"()", 1},
        {"!(a", 3},
        {"a b", 2},
        {"a == b c", 7},
        {"a not b", 6},
        {"a not", 5},
        {"a =~ b", 5},
        {"a =~", 4},
        {"a =~ 'x'", 5},
        {"a = b", 2},
        {"a & b", 2},
        {"a | b", 2},
        {"a ~ b", 2},
        {"a { b", 2},
        {"a =~ /x/d", 8},
        {"a == 'x' 'y'", 9},
        {"a =~ /unterminated", 5},
        {"in", 0},
        {"not a", 0},
        {"== a", 0},
    };
    for (const ParseFailure& failure : failures) {
        const auto result = WhenExpression::parse(failure.input);
        ASSERT_FALSE(result.has_value())
            << "'" << failure.input << "' parsed as " << result->canonical();
        EXPECT_EQ(result.error().code, anvil::ErrorCode::invalid_argument) << failure.input;
        const std::string prefix = "offset " + std::to_string(failure.offset) + ": ";
        EXPECT_TRUE(result.error().message.starts_with(prefix))
            << "'" << failure.input << "': " << result.error().message;
    }
}

TEST(WhenExpression, ParseErrorMessagesSayWhatWasFound) {
    const auto message = [](std::string_view text) {
        const auto result = WhenExpression::parse(text);
        EXPECT_FALSE(result.has_value()) << text;
        return result ? std::string() : result.error().message;
    };
    EXPECT_EQ(message("a = b"), "offset 2: unexpected '='; did you mean '==' or '=~'?");
    EXPECT_EQ(message("a b"), "offset 2: unexpected 'b'; did you forget '&&' or '||' before it?");
    EXPECT_EQ(message("a &&"),
              "offset 4: unexpected end of the expression; did you forget to write a context "
              "key?");
    EXPECT_EQ(message("(a"), "offset 2: unexpected end of the expression; expected ')'");
    EXPECT_NE(message("a =~ /(/").find("invalid regular expression"), std::string::npos);
    EXPECT_NE(message("k =~ /x/ii").find("flag 'i' is repeated"), std::string::npos);
    EXPECT_NE(message("k =~ /x").find("escape"), std::string::npos);
}

TEST(WhenExpression, InvalidRegularExpressionsAreRejected) {
    for (const char* text : {"k =~ /(/", "k =~ /a{2,1}/", "k =~ /\\C/", "k =~ /(?<n/"}) {
        const auto result = WhenExpression::parse(text);
        ASSERT_FALSE(result.has_value()) << text;
        EXPECT_TRUE(result.error().message.starts_with("offset 5: invalid regular expression"))
            << text << ": " << result.error().message;
    }
}

TEST(WhenExpression, ExpressionsAreCheapToCopyAndReadFromSeveralThreads) {
    const WhenExpression original = parse_ok("a && b == x || c =~ /^y/");
    const WhenExpression copy = original;
    const ContextKeys keys = keys_of({{"a", true}, {"b", std::string("x")}});
    EXPECT_TRUE(copy.evaluate(keys));
    EXPECT_EQ(&copy.canonical(), &original.canonical());
}

namespace {

// A random expression over the boolean keys a to d, written with every group in parentheses, and
// the obviously correct evaluation of the same tree.
struct Tree {
    enum class Op { key, constant, negate, both, either } op = Op::key;
    int key = 0;
    bool value = false;
    std::vector<Tree> children;

    bool evaluate(unsigned assignment) const {
        switch (op) {
        case Op::key:
            return (assignment >> key) & 1u;
        case Op::constant:
            return value;
        case Op::negate:
            return !children[0].evaluate(assignment);
        case Op::both:
            return children[0].evaluate(assignment) && children[1].evaluate(assignment);
        case Op::either:
            return children[0].evaluate(assignment) || children[1].evaluate(assignment);
        }
        return false;
    }

    std::string text() const {
        switch (op) {
        case Op::key:
            return std::string(1, static_cast<char>('a' + key));
        case Op::constant:
            return value ? "true" : "false";
        case Op::negate:
            return "!(" + children[0].text() + ")";
        case Op::both:
            return "(" + children[0].text() + " && " + children[1].text() + ")";
        case Op::either:
            return "(" + children[0].text() + " || " + children[1].text() + ")";
        }
        return {};
    }
};

Tree random_tree(std::mt19937& random, int depth) {
    Tree tree;
    const auto pick = static_cast<unsigned>(random() % 10);
    if (depth == 0 || pick < 3) {
        if (pick == 0) {
            tree.op = Tree::Op::constant;
            tree.value = random() % 2 == 0;
        } else {
            tree.key = static_cast<int>(random() % 4);
        }
        return tree;
    }
    tree.op = pick < 5 ? Tree::Op::negate : pick < 8 ? Tree::Op::both : Tree::Op::either;
    tree.children.push_back(random_tree(random, depth - 1));
    if (tree.op != Tree::Op::negate) {
        tree.children.push_back(random_tree(random, depth - 1));
    }
    return tree;
}

} // namespace

TEST(WhenExpression, NormalizationPreservesMeaning) {
    std::mt19937 random(12345);
    for (int round = 0; round < 400; ++round) {
        const Tree tree = random_tree(random, 4);
        const std::string text = tree.text();
        const WhenExpression expression = parse_ok(text);
        const WhenExpression reparsed = parse_ok(expression.canonical());
        EXPECT_EQ(reparsed.canonical(), expression.canonical()) << text;
        for (unsigned assignment = 0; assignment < 16; ++assignment) {
            ContextKeys keys;
            for (int key = 0; key < 4; ++key) {
                keys.set(std::string(1, static_cast<char>('a' + key)),
                         ((assignment >> key) & 1u) != 0);
            }
            const bool expected = tree.evaluate(assignment);
            EXPECT_EQ(expression.evaluate(keys), expected) << text << " with " << assignment;
            EXPECT_EQ(reparsed.evaluate(keys), expected) << text << " with " << assignment;
        }
    }
}
