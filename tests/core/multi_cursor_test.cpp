// Tests of adding cursors above and below, at line ends, and at the previous occurrence, with
// model-based comparisons against naive references.
#include "anvil/core/editing.hpp"

#include <algorithm>
#include <gtest/gtest.h>
#include <random>
#include <string>
#include <vector>

using namespace anvil::core;

namespace {

struct Editor {
    explicit Editor(std::string_view text,
                    std::vector<Selection> selections = {Selection::caret(0)},
                    std::size_t primary = 0)
        : document(*Text::from_utf8(text)), controller(document) {
        EXPECT_TRUE(document.set_selections(SelectionSet::from(std::move(selections), primary))
                        .has_value());
    }
    std::vector<Selection> selections() const {
        return document.selections().all();
    }

    Document document;
    EditController controller;
};

using Carets = std::vector<Selection>;

} // namespace

TEST(AddCursorsVertically, AddsACaretOnTheNextLineAtTheSameColumn) {
    Editor editor("abcdef\nabcdef\nabcdef", {Selection::caret(3)});
    editor.controller.add_cursors_vertically(true);
    EXPECT_EQ(editor.selections(), (Carets{Selection::caret(3), Selection::caret(10)}));
    editor.controller.add_cursors_vertically(true);
    EXPECT_EQ(editor.selections(),
              (Carets{Selection::caret(3), Selection::caret(10), Selection::caret(17)}));
}

TEST(AddCursorsVertically, AddsACaretAboveToo) {
    Editor editor("abcdef\nabcdef\nabcdef", {Selection::caret(17)});
    editor.controller.add_cursors_vertically(false);
    EXPECT_EQ(editor.selections(), (Carets{Selection::caret(10), Selection::caret(17)}));
}

TEST(AddCursorsVertically, ACaretAtTheFirstOrLastLineAddsNothing) {
    Editor top("ab\ncd", {Selection::caret(1)});
    EXPECT_FALSE(top.controller.add_cursors_vertically(false).changed_anything());
    EXPECT_EQ(top.selections(), (Carets{Selection::caret(1)}));
    Editor bottom("ab\ncd", {Selection::caret(4)});
    EXPECT_FALSE(bottom.controller.add_cursors_vertically(true).changed_anything());
}

TEST(AddCursorsVertically, ACursorOnAShortLineStopsAtItsEndAndRemembersTheColumn) {
    Editor editor("abcdef\nab\nabcdef", {Selection::caret(5)});
    editor.controller.add_cursors_vertically(true);
    EXPECT_EQ(editor.selections(), (Carets{Selection::caret(5), Selection::caret(9)}))
        << "the short line has two characters";
    editor.controller.add_cursors_vertically(true);
    EXPECT_EQ(editor.selections(),
              (Carets{Selection::caret(5), Selection::caret(9), Selection::caret(15)}))
        << "back at column 5 on the long line";
}

TEST(AddCursorsVertically, AColumnInsideATabIsWantedAgainOnTheNextLine) {
    // Visible column 3 is nearer the end of the tab than its start.
    Editor editor("abcdef\n\tx\nabcdef", {Selection::caret(3)});
    editor.controller.add_cursors_vertically(true);
    editor.controller.add_cursors_vertically(true);
    EXPECT_EQ(editor.selections(),
              (Carets{Selection::caret(3), Selection::caret(8), Selection::caret(13)}));
}

TEST(AddCursorsVertically, ChangingTheSelectionsForgetsTheRememberedColumn) {
    Editor editor("abcdef\nab\nabcdef", {Selection::caret(5)});
    editor.controller.add_cursors_vertically(true);
    ASSERT_EQ(editor.selections(), (Carets{Selection::caret(5), Selection::caret(9)}));
    // The caret on the short line is put there again by something else: it wants column 2.
    editor.controller.select(Selection::caret(9));
    editor.controller.add_cursors_vertically(true);
    EXPECT_EQ(editor.selections(), (Carets{Selection::caret(9), Selection::caret(12)}));
}

TEST(AddCursorsVertically, ASelectionIsCopiedWithBothEndsMoved) {
    Editor editor("abcdef\nabcdef\nabcdef", {Selection{1, 4}});
    editor.controller.add_cursors_vertically(true);
    EXPECT_EQ(editor.selections(), (Carets{Selection{1, 4}, Selection{8, 11}}));
    EXPECT_EQ(editor.document.selections().primary(), (Selection{1, 4}));
}

TEST(AddCursorsVertically, EveryCursorGetsACopyAndCopiesThatMeetAreMerged) {
    Editor editor("ab\ncd\nef\ngh", {Selection::caret(1), Selection::caret(4)});
    editor.controller.add_cursors_vertically(true);
    EXPECT_EQ(editor.selections(),
              (Carets{Selection::caret(1), Selection::caret(4), Selection::caret(7)}));
}

TEST(AddCursorsToLineEnds, PutsACaretAtTheEndOfEachSelectedLine) {
    Editor editor("ab\ncd\nef", {Selection{1, 7}});
    EXPECT_TRUE(editor.controller.add_cursors_to_line_ends().changed_anything());
    EXPECT_EQ(editor.selections(),
              (Carets{Selection::caret(2), Selection::caret(5), Selection::caret(7)}));
}

TEST(AddCursorsToLineEnds, ASelectionThatEndsAtTheStartOfALineLeavesThatLineOut) {
    Editor editor("ab\ncd\nef", {Selection{1, 6}});
    editor.controller.add_cursors_to_line_ends();
    EXPECT_EQ(editor.selections(), (Carets{Selection::caret(2), Selection::caret(5)}));
}

TEST(AddCursorsToLineEnds, CaretsAndNothingSelectedChangeNothing) {
    Editor editor("ab\ncd", {Selection::caret(1), Selection::caret(4)});
    EXPECT_FALSE(editor.controller.add_cursors_to_line_ends().changed_anything());
    EXPECT_EQ(editor.selections(), (Carets{Selection::caret(1), Selection::caret(4)}));
}

TEST(AddCursorsToLineEnds, ASelectionOnOneLineGetsACaretAtItsEnd) {
    Editor editor("abcd", {Selection{1, 3}});
    editor.controller.add_cursors_to_line_ends();
    EXPECT_EQ(editor.selections(), (Carets{Selection::caret(3)}));
}

namespace {

Search plain(std::string pattern) {
    return *Search::create({.pattern = std::move(pattern), .match_case = true});
}

} // namespace

TEST(AddPreviousOccurrence, AddsTheMatchBeforeThePrimarySelection) {
    Editor editor("foo bar foo baz foo", {Selection{16, 19}});
    const Search search = plain("foo");
    editor.controller.add_previous_occurrence(search);
    EXPECT_EQ(editor.selections(), (Carets{Selection{8, 11}, Selection{16, 19}}));
    EXPECT_EQ(editor.document.selections().primary(), (Selection{8, 11}));
    editor.controller.add_previous_occurrence(search);
    EXPECT_EQ(editor.selections(), (Carets{Selection{0, 3}, Selection{8, 11}, Selection{16, 19}}));
    EXPECT_EQ(editor.document.selections().primary(), (Selection{0, 3}));
}

TEST(AddPreviousOccurrence, WrapsAroundFromTheEndAndStopsWhenEveryMatchIsTaken) {
    Editor editor("foo bar foo", {Selection{0, 3}});
    const Search search = plain("foo");
    editor.controller.add_previous_occurrence(search);
    EXPECT_EQ(editor.selections(), (Carets{Selection{0, 3}, Selection{8, 11}}));
    EXPECT_FALSE(editor.controller.add_previous_occurrence(search).changed_anything());
}

TEST(AddPreviousOccurrence, DoesNothingWithoutAnotherMatch) {
    Editor editor("foo bar", {Selection{0, 3}});
    EXPECT_FALSE(editor.controller.add_previous_occurrence(plain("foo")).changed_anything());
    Editor none("bar", {Selection{0, 3}});
    EXPECT_FALSE(none.controller.add_previous_occurrence(plain("zzz")).changed_anything());
}

// --- model-based ---

namespace {

class Random {
public:
    explicit Random(unsigned seed) : engine_(seed) {}
    std::size_t below(std::size_t bound) {
        return std::uniform_int_distribution<std::size_t>(0, bound - 1)(engine_);
    }

private:
    std::mt19937 engine_;
};

// The cursors of the reference are kept as line and column of the characters of the lines, with the
// column that each end wants.
struct End {
    std::size_t line = 0;
    std::size_t column = 0;
    std::size_t wanted = 0;
};
struct Cursor {
    End anchor;
    End active;
};

TEST(AddCursorsVertically, RepeatedAdditionsAgreeWithAReferenceThatKeepsTheWantedColumn) {
    Random random(77);
    for (int round = 0; round < 3000; ++round) {
        // Lines of characters that are one UTF-16 unit and one or two bytes long.
        std::vector<std::vector<std::string>> lines(2 + random.below(6));
        for (auto& line : lines) {
            const std::size_t length = random.below(7);
            for (std::size_t i = 0; i < length; ++i) {
                line.push_back(random.below(4) == 0 ? "\xC3\xA9" : "a");
            }
        }
        std::string text;
        std::vector<std::size_t> starts;
        for (std::size_t l = 0; l < lines.size(); ++l) {
            starts.push_back(text.size());
            for (const std::string& c : lines[l]) {
                text += c;
            }
            text += l + 1 < lines.size() ? "\n" : "";
        }
        const auto offset_of = [&](const End& end) {
            std::size_t offset = starts[end.line];
            for (std::size_t i = 0; i < end.column; ++i) {
                offset += lines[end.line][i].size();
            }
            return offset;
        };
        const auto end_at = [&](std::size_t offset) {
            std::size_t line = 0;
            while (line + 1 < lines.size() && starts[line + 1] <= offset) {
                ++line;
            }
            std::size_t column = 0;
            for (std::size_t at = starts[line]; at < offset; at += lines[line][column++].size()) {
            }
            return End{line, column, column};
        };

        std::vector<Selection> initial;
        const std::size_t count = 1 + random.below(3);
        for (std::size_t i = 0; i < count; ++i) {
            const auto random_end = [&] {
                const std::size_t line = random.below(lines.size());
                return End{line, random.below(lines[line].size() + 1), 0};
            };
            End a = random_end();
            End b = random.below(2) ? a : random_end();
            initial.push_back({offset_of(a), offset_of(b)});
        }
        Editor editor(text, initial);
        std::vector<Cursor> model;
        for (const Selection& selection : editor.selections()) {
            model.push_back({end_at(selection.anchor), end_at(selection.active)});
        }

        for (int step = 0; step < 4; ++step) {
            const bool down = random.below(2) == 0;
            if (random.below(5) == 0) {
                // Moving the cursors forgets what the ends wanted.
                editor.controller.move(Motion::right);
                model.clear();
                for (const Selection& selection : editor.selections()) {
                    model.push_back({end_at(selection.anchor), end_at(selection.active)});
                }
            }
            const std::size_t primary = editor.document.selections().primary_index();
            std::vector<Cursor> grown;
            for (const Cursor& cursor : model) {
                grown.push_back(cursor);
                Cursor moved = cursor;
                for (End* end : {&moved.anchor, &moved.active}) {
                    const bool beyond = down ? end->line + 1 >= lines.size() : end->line == 0;
                    if (beyond) {
                        const bool at_edge = down ? end->line + 1 == lines.size() &&
                                                        end->column == lines[end->line].size()
                                                  : end->line == 0 && end->column == 0;
                        end->wanted = at_edge ? end->column : end->wanted;
                    } else {
                        end->line = down ? end->line + 1 : end->line - 1;
                        end->column = std::min(end->wanted, lines[end->line].size());
                    }
                }
                grown.push_back(moved);
            }
            editor.controller.add_cursors_vertically(down);

            std::vector<Selection> offsets;
            for (const Cursor& cursor : grown) {
                offsets.push_back({offset_of(cursor.anchor), offset_of(cursor.active)});
            }
            const SelectionSet expected = SelectionSet::from(std::move(offsets), 2 * primary);
            ASSERT_EQ(editor.selections(), expected.all())
                << "round " << round << " step " << step << (down ? " down" : " up");
            ASSERT_EQ(editor.document.selections().primary(), expected.primary())
                << "round " << round << " step " << step;
            model.clear();
            for (const Selection& selection : expected.all()) {
                // The cursor that made a selection keeps the column it wanted.
                Cursor found{end_at(selection.anchor), end_at(selection.active)};
                for (const Cursor& cursor : grown) {
                    if (offset_of(cursor.anchor) == selection.anchor &&
                        offset_of(cursor.active) == selection.active) {
                        found = cursor;
                        break;
                    }
                }
                model.push_back(found);
            }
        }
    }
}

TEST(AddPreviousOccurrence, AgreesWithANaiveSearchOnRandomTexts) {
    Random random(5);
    for (int round = 0; round < 3000; ++round) {
        std::string text;
        const std::size_t length = random.below(24);
        for (std::size_t i = 0; i < length; ++i) {
            text += "abc\n"[random.below(4)];
        }
        const std::string pattern = "ab";
        std::vector<Selection> initial;
        for (std::size_t i = 0, count = 1 + random.below(3); i < count; ++i) {
            const std::size_t a = random.below(text.size() + 1);
            const std::size_t b = random.below(2) ? a : random.below(text.size() + 1);
            initial.push_back({a, b});
        }
        Editor editor(text, initial, random.below(initial.size()));
        const auto selections = editor.selections();
        const std::size_t origin = editor.document.selections().primary().start();

        std::vector<std::size_t> starts;
        for (std::size_t at = text.find(pattern); at != std::string::npos;
             at = text.find(pattern, at + 1)) {
            const bool overlaps = std::ranges::any_of(selections, [&](const Selection& s) {
                return s.empty() ? at <= s.start() && s.start() <= at + pattern.size()
                                 : at < s.end() && s.start() < at + pattern.size();
            });
            if (!overlaps) {
                starts.push_back(at);
            }
        }
        std::optional<std::size_t> expected;
        for (std::size_t i = starts.size(); i-- > 0;) {
            if (starts[i] < origin) {
                expected = starts[i];
                break;
            }
        }
        if (!expected && !starts.empty()) {
            expected = starts.back();
        }
        const Search search = plain(pattern);
        editor.controller.add_previous_occurrence(search);
        if (!expected) {
            ASSERT_EQ(editor.selections(), selections) << "round " << round;
            continue;
        }
        const auto after = editor.document.selections();
        ASSERT_EQ(after.primary(), (Selection{*expected, *expected + pattern.size()}))
            << "round " << round << " text [" << text << "] origin " << origin;
    }
}

} // namespace

TEST(JumpToBracket, MovesTheCaretToThePartnerOfTheBracketAtIt) {
    Editor editor("f(a, [b])", {Selection::caret(1)});
    EXPECT_TRUE(editor.controller.jump_to_bracket().changed_anything());
    EXPECT_EQ(editor.selections(), (Carets{Selection::caret(8)}));
    editor.controller.jump_to_bracket();
    EXPECT_EQ(editor.selections(), (Carets{Selection::caret(1)}));
}

TEST(JumpToBracket, StartsFromTheStartOfASelectionAndLeavesACaret) {
    Editor editor("f(a, [b])", {Selection{6, 1}});
    editor.controller.jump_to_bracket();
    EXPECT_EQ(editor.selections(), (Carets{Selection::caret(8)}));

    Editor reversed("f(a, [b])", {Selection{1, 6}});
    reversed.controller.jump_to_bracket();
    EXPECT_EQ(reversed.selections(), (Carets{Selection::caret(8)}));
}

TEST(JumpToBracket, EveryCursorJumpsAndCursorsThatMeetBecomeOne) {
    Editor editor("(a) (b) x", {Selection::caret(0), Selection::caret(4), Selection::caret(8)}, 1);
    editor.controller.jump_to_bracket();
    EXPECT_EQ(editor.selections(),
              (Carets{Selection::caret(2), Selection::caret(6), Selection::caret(8)}));
    EXPECT_EQ(editor.document.selections().primary(), Selection::caret(6));

    Editor same("(a) b", {Selection::caret(0), Selection::caret(1)});
    same.controller.jump_to_bracket();
    EXPECT_EQ(same.selections(), (Carets{Selection::caret(2)}));
}

TEST(JumpToBracket, ChangesNothingWhereThereIsNoBracketAhead) {
    Editor editor("plain text", {Selection::caret(3)});
    EXPECT_FALSE(editor.controller.jump_to_bracket().changed_anything());
    EXPECT_EQ(editor.selections(), (Carets{Selection::caret(3)}));
    EXPECT_EQ(editor.document.text().to_string(), "plain text");
}
