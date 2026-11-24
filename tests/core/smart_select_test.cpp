// Tests of Expand Selection and Shrink Selection: the ranges for fixed texts, written out as the
// text each step selects, and properties of the steps over random texts.
#include "anvil/core/editing.hpp"
#include "anvil/core/smart_select.hpp"

#include <algorithm>
#include <gtest/gtest.h>
#include <random>
#include <string>
#include <vector>

using namespace anvil::core;

namespace {

using Texts = std::vector<std::string>;

struct Editor {
    explicit Editor(std::string_view text,
                    std::vector<Selection> selections = {Selection::caret(0)})
        : document(*Text::from_utf8(text)), controller(document) {
        EXPECT_TRUE(document.set_selections(SelectionSet::from(std::move(selections))).has_value());
    }

    std::string text_of(const Selection& selection) const {
        return document.text().slice(selection.start(), selection.end() - selection.start());
    }
    Texts selected() const {
        Texts result;
        for (const Selection& selection : document.selections().all()) {
            result.push_back(text_of(selection));
        }
        return result;
    }

    Document document;
    EditController controller;
};

// The caret in `marked` is the `|`. Returns the text without it and the offset it stood at.
std::pair<std::string, std::size_t> caret_in(std::string marked) {
    const std::size_t at = marked.find('|');
    marked.erase(at, 1);
    return {marked, at};
}

// What each Expand Selection selects, from the caret in `marked` until nothing changes. The first
// entry is what the caret selects, which is nothing.
Texts expansions(const std::string& marked) {
    const auto [text, caret] = caret_in(marked);
    Editor editor(text, {Selection::caret(caret)});
    Texts result{editor.selected().front()};
    while (editor.controller.expand_selection().selections_changed) {
        result.push_back(editor.selected().front());
    }
    return result;
}

} // namespace

TEST(SmartSelect, ExpandsAnIdentifierBySubwordWordBracketsAndText) {
    EXPECT_EQ(expansions("call(fooB|ar, baz)"),
              (Texts{"", "Bar", "fooBar", "fooBar, baz", "(fooBar, baz)", "call(fooBar, baz)"}));
}

TEST(SmartSelect, SubwordsEndAtUnderscoresAndAtCaseChanges) {
    EXPECT_EQ(expansions("my_va|r_name"), (Texts{"", "var", "my_var_name"}));
    EXPECT_EQ(expansions("|fooBarBaz"), (Texts{"", "foo", "fooBarBaz"}));
    EXPECT_EQ(expansions("fooBarBaz|"), (Texts{"", "Baz", "fooBarBaz"}));
    EXPECT_EQ(expansions("fooBar|Baz"), (Texts{"", "Baz", "fooBarBaz"}));
}

TEST(SmartSelect, AWordWithoutSubwordsIsOneStep) {
    EXPECT_EQ(expansions("ab|cd efgh"), (Texts{"", "abcd", "abcd efgh"}));
}

TEST(SmartSelect, AddsTheLinesBetweenABlockAndWhatItSurrounds) {
    EXPECT_EQ(expansions("function f() {\n    return f|oo;\n}"),
              (Texts{"", "foo", "return foo;", "    return foo;", "\n    return foo;\n",
                     "{\n    return foo;\n}", "function f() {\n    return foo;\n}"}));
}

TEST(SmartSelect, AMultiLinePairTakesTheLineThatOpensIt) {
    EXPECT_EQ(expansions("if (x) {\n  y|;\n}\n"),
              (Texts{"", "y", "y;", "  y;", "\n  y;\n", "{\n  y;\n}", "if (x) {\n  y;\n}",
                     "if (x) {\n  y;\n}\n"}));
    EXPECT_EQ(expansions("    if (x) {\n        y|;\n    }\n"),
              (Texts{"", "y", "y;", "        y;", "\n        y;\n    ", "{\n        y;\n    }",
                     "if (x) {\n        y;\n    }", "    if (x) {\n        y;\n    }",
                     "    if (x) {\n        y;\n    }\n"}));
}

TEST(SmartSelect, ABraceOnItsOwnLineTakesTheLineAboveIt) {
    EXPECT_EQ(expansions("    if (x)\n    {\n        y|;\n    }"),
              (Texts{"", "y", "y;", "        y;", "\n        y;\n    ", "{\n        y;\n    }",
                     "    {\n        y;\n    }", "if (x)\n    {\n        y;\n    }",
                     "    if (x)\n    {\n        y;\n    }"}));
}

TEST(SmartSelect, APairInsideAnotherLineGivesTheTextThatLeadsToIt) {
    EXPECT_EQ(expansions("x = {\n  |1\n} + 2"), (Texts{"", "1", "  1", "\n  1\n", "{\n  1\n}",
                                                       "x = {\n  1\n}", "x = {\n  1\n} + 2"}));
    EXPECT_EQ(expansions("  x = {\n    |1\n  } + 2"),
              (Texts{"", "1", "    1", "\n    1\n  ", "{\n    1\n  }", "x = {\n    1\n  }",
                     "  x = {\n    1\n  }", "  x = {\n    1\n  } + 2"}));
}

TEST(SmartSelect, ARangeThatCrossesTheNextOneIsLeftOut) {
    EXPECT_EQ(expansions("f(a, {\n  |y\n})"),
              (Texts{"", "y", "  y", "\n  y\n", "{\n  y\n}", "a, {\n  y\n}", "(a, {\n  y\n})",
                     "f(a, {\n  y\n})"}));
}

TEST(SmartSelect, NestedPairsGrowOutwardAndEachKindOfBracketCounts) {
    EXPECT_EQ(expansions("a[(b|)]"), (Texts{"", "b", "(b)", "[(b)]", "a[(b)]"}));
}

TEST(SmartSelect, ACaretBesideABracketIsInsideThePairOnItsInnerSideOnly) {
    EXPECT_EQ(expansions("(|x) y"), (Texts{"", "x", "(x)", "(x) y"}));
    EXPECT_EQ(expansions("(x|) y"), (Texts{"", "x", "(x)", "(x) y"}));
    EXPECT_EQ(expansions("|(x) y"), (Texts{"", "(x) y"}));
    EXPECT_EQ(expansions("(x)| y"), (Texts{"", "(x) y"}));
}

TEST(SmartSelect, ABlankLineIsOneStepBeforeTheText) {
    EXPECT_EQ(expansions("a\n |  \nb"), (Texts{"", "   ", "a\n   \nb"}));
}

TEST(SmartSelect, AnEmptyLineGoesStraightToTheText) {
    EXPECT_EQ(expansions("a\n|\nb"), (Texts{"", "a\n\nb"}));
}

TEST(SmartSelect, UnmatchedBracketsGiveNoPair) {
    EXPECT_EQ(expansions("(a| b"), (Texts{"", "a", "(a b"}));
    EXPECT_EQ(expansions("a| b)"), (Texts{"", "a", "a b)"}));
}

TEST(SmartSelect, WideCharactersStayWhole) {
    EXPECT_EQ(expansions("(é|a)"), (Texts{"", "éa", "(éa)"}));
}

TEST(SmartSelect, ASelectionExpandsToTheNextRangeThatContainsIt) {
    Editor editor("call(fooBar, baz)", {Selection{5, 8}});
    EXPECT_TRUE(editor.controller.expand_selection().selections_changed);
    EXPECT_EQ(editor.selected(), (Texts{"fooBar"}));
    editor.controller.expand_selection();
    EXPECT_EQ(editor.selected(), (Texts{"fooBar, baz"}));
}

TEST(SmartSelect, ASelectionThatIsARangeAlreadyExpandsPastIt) {
    Editor editor("call(fooBar, baz)", {Selection{5, 11}});
    editor.controller.expand_selection();
    EXPECT_EQ(editor.selected(), (Texts{"fooBar, baz"}));
    editor.controller.shrink_selection();
    EXPECT_EQ(editor.document.selections().all(), (std::vector<Selection>{Selection{5, 11}}));
}

TEST(SmartSelect, AReversedSelectionExpandsToAForwardSelection) {
    Editor editor("foo bar baz", {Selection{7, 5}});
    editor.controller.expand_selection();
    EXPECT_EQ(editor.document.selections().all(), (std::vector<Selection>{Selection{4, 7}}));
}

TEST(SmartSelect, EverySelectionExpandsOnItsOwn) {
    Editor editor("(a b) (c d)", {Selection::caret(1), Selection::caret(7)});
    editor.controller.expand_selection();
    EXPECT_EQ(editor.selected(), (Texts{"a", "c"}));
    editor.controller.expand_selection();
    EXPECT_EQ(editor.selected(), (Texts{"a b", "c d"}));
    editor.controller.expand_selection();
    EXPECT_EQ(editor.selected(), (Texts{"(a b)", "(c d)"}));
    editor.controller.shrink_selection();
    EXPECT_EQ(editor.selected(), (Texts{"a b", "c d"}));
}

TEST(SmartSelect, ThePrimarySelectionStaysThePrimarySelection) {
    Editor editor("(a b) (c d)");
    ASSERT_TRUE(editor.document
                    .set_selections(SelectionSet::from(
                        {Selection::caret(1), Selection::caret(7), Selection::caret(10)}, 1))
                    .has_value());
    editor.controller.expand_selection();
    EXPECT_EQ(editor.document.selections().primary_index(), 1U);
    EXPECT_EQ(editor.text_of(editor.document.selections().primary()), "c");
    editor.controller.shrink_selection();
    EXPECT_EQ(editor.document.selections().primary_index(), 1U);
}

TEST(SmartSelect, SelectionsThatMeetAreMergedAndShrinkSeparatesThem) {
    Editor editor("foo bar\nbaz", {Selection::caret(1), Selection::caret(9)});
    editor.controller.expand_selection();
    editor.controller.expand_selection();
    EXPECT_EQ(editor.selected(), (Texts{"foo bar\nbaz"}));
    editor.controller.expand_selection();
    EXPECT_EQ(editor.selected(), (Texts{"foo bar\nbaz"}));
    editor.controller.shrink_selection();
    EXPECT_EQ(editor.selected(), (Texts{"foo bar", "baz"}));
    editor.controller.shrink_selection();
    EXPECT_EQ(editor.selected(), (Texts{"foo", ""}));
    editor.controller.shrink_selection();
    EXPECT_EQ(editor.document.selections().all(),
              (std::vector<Selection>{Selection::caret(1), Selection::caret(9)}));
}

TEST(SmartSelect, ShrinkGoesBackThroughTheRangesOfExpand) {
    Editor editor("call(fooBar, baz)", {Selection::caret(8)});
    for (int i = 0; i < 4; ++i) {
        editor.controller.expand_selection();
    }
    EXPECT_EQ(editor.selected(), (Texts{"(fooBar, baz)"}));
    editor.controller.shrink_selection();
    EXPECT_EQ(editor.selected(), (Texts{"fooBar, baz"}));
    editor.controller.shrink_selection();
    EXPECT_EQ(editor.selected(), (Texts{"fooBar"}));
    editor.controller.shrink_selection();
    EXPECT_EQ(editor.selected(), (Texts{"Bar"}));
    editor.controller.shrink_selection();
    EXPECT_EQ(editor.document.selections().all(), (std::vector<Selection>{Selection::caret(8)}));
}

TEST(SmartSelect, ShrinkAtTheStartAndExpandAtTheEndDoNothing) {
    Editor editor("a b", {Selection::caret(0)});
    EXPECT_FALSE(editor.controller.shrink_selection().selections_changed);
    editor.controller.expand_selection();
    editor.controller.expand_selection();
    EXPECT_EQ(editor.selected(), (Texts{"a b"}));
    EXPECT_FALSE(editor.controller.expand_selection().selections_changed);
}

TEST(SmartSelect, ShrinkAfterAnotherSelectionChangeDoesNothing) {
    Editor editor("call(fooBar, baz)", {Selection::caret(8)});
    editor.controller.expand_selection();
    editor.controller.expand_selection();
    editor.controller.set_caret(14);
    EXPECT_FALSE(editor.controller.shrink_selection().selections_changed);
    EXPECT_EQ(editor.document.selections().all(), (std::vector<Selection>{Selection::caret(14)}));
}

TEST(SmartSelect, ShrinkAfterAnEditThatLeavesTheSelectionsDoesNothing) {
    Editor editor("foo bar", {Selection::caret(1)});
    editor.controller.expand_selection();
    editor.controller.expand_selection();
    ASSERT_EQ(editor.selected(), (Texts{"foo bar"}));
    editor.controller.shrink_selection();
    ASSERT_EQ(editor.selected(), (Texts{"foo"}));
    editor.controller.replace_ranges({Edit{4, 7, "qux"}});
    ASSERT_EQ(editor.selected(), (Texts{"foo"}));
    EXPECT_FALSE(editor.controller.shrink_selection().selections_changed);
    EXPECT_EQ(editor.selected(), (Texts{"foo"}));
}

TEST(SmartSelect, ExpandAfterAnotherSelectionChangeStartsFromThatSelection) {
    Editor editor("foo bar", {Selection::caret(1)});
    editor.controller.expand_selection();
    editor.controller.set_caret(5);
    editor.controller.expand_selection();
    EXPECT_EQ(editor.selected(), (Texts{"bar"}));
}

namespace {

std::string random_text(std::mt19937& random) {
    static const std::vector<std::string> pieces = {
        "foo", "Bar", "baz_", "-", "x", " ", "  ", "\n", "\n", "\t",
        "(",   ")",   "[",    "]", "{", "}", ".",  ",",  "é",  "\n    ",
    };
    std::string text;
    const std::size_t count = random() % 40;
    for (std::size_t i = 0; i < count; ++i) {
        text += pieces[random() % pieces.size()];
    }
    return text;
}

} // namespace

TEST(SmartSelect, TheRangesAroundAnOffsetAreNestedAndEndWithTheText) {
    std::mt19937 random(7);
    for (int round = 0; round < 400; ++round) {
        const std::string source = random_text(random);
        const Text text = *Text::from_utf8(source);
        for (std::size_t offset = 0; offset <= text.size(); ++offset) {
            if (!text.is_char_boundary(offset)) {
                continue;
            }
            const std::vector<Selection> ranges = selection_ranges(text, offset);
            ASSERT_FALSE(ranges.empty()) << source;
            EXPECT_EQ(ranges.back(), (Selection{0, text.size()})) << source << " at " << offset;
            for (std::size_t i = 0; i < ranges.size(); ++i) {
                EXPECT_LE(ranges[i].start(), offset) << source;
                EXPECT_GE(ranges[i].end(), offset) << source;
                EXPECT_FALSE(ranges[i].reversed());
                EXPECT_TRUE(text.is_char_boundary(ranges[i].start()));
                EXPECT_TRUE(text.is_char_boundary(ranges[i].end()));
                if (i > 0) {
                    EXPECT_LE(ranges[i].start(), ranges[i - 1].start()) << source;
                    EXPECT_GE(ranges[i].end(), ranges[i - 1].end()) << source;
                    EXPECT_NE(ranges[i], ranges[i - 1]) << source << " at " << offset;
                }
            }
        }
    }
}

TEST(SmartSelect, ShrinkRetracesExpandOverRandomTextsAndCursors) {
    std::mt19937 random(11);
    for (int round = 0; round < 300; ++round) {
        const std::string source = random_text(random);
        Editor editor(source);
        std::vector<Selection> carets;
        for (std::size_t i = 0; i < 1 + random() % 3; ++i) {
            std::size_t offset = random() % (editor.document.text().size() + 1);
            while (!editor.document.text().is_char_boundary(offset)) {
                --offset;
            }
            carets.push_back(Selection::caret(offset));
        }
        ASSERT_TRUE(editor.document.set_selections(SelectionSet::from(carets)).has_value());

        std::vector<SelectionSet> trail{editor.document.selections()};
        for (int step = 0; step < 12; ++step) {
            if (!editor.controller.expand_selection().selections_changed) {
                break;
            }
            trail.push_back(editor.document.selections());
        }
        if (trail.size() > 1 && trail.back().all().size() == trail.front().all().size()) {
            for (std::size_t i = trail.size() - 1; i-- > 0;) {
                editor.controller.shrink_selection();
                ASSERT_EQ(editor.document.selections(), trail[i]) << source << " step " << i;
            }
        }
    }
}
