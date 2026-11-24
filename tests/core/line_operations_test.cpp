// Tests of moving, copying and duplicating lines: the cases that VS Code's own tests state, and a
// model-based test that compares each command with a naive reference written on arrays of lines.
#include "anvil/core/editing.hpp"

#include <algorithm>
#include <gtest/gtest.h>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace anvil::core;

namespace {

// A position as VS Code writes it: 1-based line and column in characters; the texts of these tests
// are ASCII, so a column is a byte.
struct Pos {
    std::size_t line;
    std::size_t column;
};

struct Sel {
    Pos anchor;
    Pos active;
};

std::string join(const std::vector<std::string>& lines) {
    std::string text;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        text += (i ? "\n" : "") + lines[i];
    }
    return text;
}

struct Editor {
    Editor(const std::vector<std::string>& lines, const std::vector<Sel>& selections)
        : document(*Text::from_utf8(join(lines))), controller(document) {
        std::vector<Selection> offsets;
        for (const Sel& selection : selections) {
            offsets.push_back({offset(selection.anchor), offset(selection.active)});
        }
        EXPECT_TRUE(document.set_selections(SelectionSet::from(std::move(offsets))).has_value());
    }

    std::size_t offset(Pos position) const {
        return document.text().line_start(position.line - 1) + position.column - 1;
    }
    std::vector<std::string> lines() const {
        std::vector<std::string> result;
        const Text& text = document.text();
        for (std::size_t line = 0; line < text.line_count(); ++line) {
            result.push_back(text.line(line));
        }
        return result;
    }
    std::vector<Sel> selections() const {
        std::vector<Sel> result;
        const Text& text = document.text();
        const auto pos = [&](std::size_t offset) {
            const Position position = text.position_at(offset);
            return Pos{position.line + 1, position.column + 1};
        };
        for (const Selection& selection : document.selections().all()) {
            result.push_back({pos(selection.anchor), pos(selection.active)});
        }
        return result;
    }

    Document document;
    EditController controller;
};

using Lines = std::vector<std::string>;

const Lines five = {"first", "second line", "third line", "fourth line", "fifth"};

void expect_selections(const Editor& editor, const std::vector<Sel>& expected,
                       const std::string& what) {
    const auto actual = editor.selections();
    ASSERT_EQ(actual.size(), expected.size()) << what;
    for (std::size_t i = 0; i < actual.size(); ++i) {
        EXPECT_EQ(actual[i].anchor.line, expected[i].anchor.line) << what << " selection " << i;
        EXPECT_EQ(actual[i].anchor.column, expected[i].anchor.column) << what << " selection " << i;
        EXPECT_EQ(actual[i].active.line, expected[i].active.line) << what << " selection " << i;
        EXPECT_EQ(actual[i].active.column, expected[i].active.column) << what << " selection " << i;
    }
}

void check(const Lines& lines, const std::vector<Sel>& selections, const char* command,
           const Lines& expected_lines, const std::vector<Sel>& expected_selections) {
    Editor editor(lines, selections);
    const std::string name = command;
    if (name == "copy_down") {
        editor.controller.copy_lines(true);
    } else if (name == "copy_up") {
        editor.controller.copy_lines(false);
    } else if (name == "move_down") {
        editor.controller.move_lines(true);
    } else if (name == "move_up") {
        editor.controller.move_lines(false);
    } else {
        editor.controller.duplicate_selection();
    }
    EXPECT_EQ(editor.lines(), expected_lines) << name;
    expect_selections(editor, expected_selections, name);
}

} // namespace

TEST(CopyLines, FirstLineDownAndUp) {
    const Lines doubled = {"first", "first", "second line", "third line", "fourth line", "fifth"};
    check(five, {{{1, 3}, {1, 1}}}, "copy_down", doubled, {{{2, 3}, {2, 1}}});
    check(five, {{{1, 3}, {1, 1}}}, "copy_up", doubled, {{{1, 3}, {1, 1}}});
}

TEST(CopyLines, LastLineDownAndUp) {
    const Lines doubled = {"first", "second line", "third line", "fourth line", "fifth", "fifth"};
    check(five, {{{5, 3}, {5, 1}}}, "copy_down", doubled, {{{6, 3}, {6, 1}}});
    check(five, {{{5, 3}, {5, 1}}}, "copy_up", doubled, {{{5, 3}, {5, 1}}});
    check(five, {{{5, 6}, {5, 6}}}, "copy_up", doubled, {{{5, 6}, {5, 6}}});
}

TEST(CopyLines, ACaretAtTheEndOfALineStaysOnTheUpperCopy) {
    check(five, {{{3, 11}, {3, 11}}}, "copy_up",
          {"first", "second line", "third line", "third line", "fourth line", "fifth"},
          {{{3, 11}, {3, 11}}});
}

TEST(CopyLines, ManyLinesUp) {
    check(five, {{{4, 3}, {2, 1}}}, "copy_up",
          {"first", "second line", "third line", "fourth line", "second line", "third line",
           "fourth line", "fifth"},
          {{{4, 3}, {2, 1}}});
}

TEST(CopyLines, ASelectionThatEndsAtTheStartOfALineDoesNotCopyThatLine) {
    check(five, {{{2, 1}, {1, 1}}}, "copy_up",
          {"first", "first", "second line", "third line", "fourth line", "fifth"},
          {{{2, 1}, {1, 1}}});
    check(five, {{{2, 1}, {1, 1}}}, "copy_down",
          {"first", "first", "second line", "third line", "fourth line", "fifth"},
          {{{3, 1}, {2, 1}}});
}

TEST(CopyLines, ACaretOnAnEmptyLineMovesDownWithItWhenCopyingDown) {
    check({"a", "", "b"}, {{{2, 1}, {2, 1}}}, "copy_down", {"a", "", "", "b"}, {{{3, 1}, {3, 1}}});
    check({"a", "", "b"}, {{{2, 1}, {2, 1}}}, "copy_up", {"a", "", "", "b"}, {{{2, 1}, {2, 1}}});
}

TEST(CopyLines, TwoCaretsOnTheSameLineCopyItOnce) {
    check(five, {{{2, 2}, {2, 2}}, {{2, 5}, {2, 5}}}, "copy_down",
          {"first", "second line", "second line", "third line", "fourth line", "fifth"},
          {{{3, 2}, {3, 2}}, {{3, 5}, {3, 5}}});
}

TEST(CopyLines, CaretsOnDifferentLinesCopyTheirOwnLines) {
    check({"a", "b", "c", "d"}, {{{1, 1}, {1, 1}}, {{3, 1}, {3, 1}}}, "copy_down",
          {"a", "a", "b", "c", "c", "d"}, {{{2, 1}, {2, 1}}, {{5, 1}, {5, 1}}});
    check({"a", "b", "c", "d"}, {{{1, 1}, {1, 1}}, {{3, 1}, {3, 1}}}, "copy_up",
          {"a", "a", "b", "c", "c", "d"}, {{{1, 1}, {1, 1}}, {{4, 1}, {4, 1}}});
}

TEST(MoveLines, TheFirstLineCannotGoUpAndTheLastCannotGoDown) {
    check(five, {{{1, 1}, {1, 1}}}, "move_up", five, {{{1, 1}, {1, 1}}});
    check(five, {{{5, 1}, {5, 1}}}, "move_down", five, {{{5, 1}, {5, 1}}});
}

TEST(MoveLines, FirstLineDown) {
    check(five, {{{1, 4}, {1, 1}}}, "move_down",
          {"second line", "first", "third line", "fourth line", "fifth"}, {{{2, 4}, {2, 1}}});
}

TEST(MoveLines, SecondLineUpKeepsTheColumn) {
    const Lines moved = {"second line", "first", "third line", "fourth line", "fifth"};
    check(five, {{{2, 1}, {2, 1}}}, "move_up", moved, {{{1, 1}, {1, 1}}});
    check(five, {{{2, 12}, {2, 12}}}, "move_up", moved, {{{1, 12}, {1, 12}}});
}

TEST(MoveLines, LastLineUpAndFourthLineDown) {
    const Lines swapped = {"first", "second line", "third line", "fifth", "fourth line"};
    check(five, {{{5, 6}, {5, 6}}}, "move_up", swapped, {{{4, 6}, {4, 6}}});
    check(five, {{{5, 6}, {5, 1}}}, "move_up", swapped, {{{4, 6}, {4, 1}}});
    check(five, {{{5, 1}, {5, 1}}}, "move_up", swapped, {{{4, 1}, {4, 1}}});
    check(five, {{{4, 1}, {4, 1}}}, "move_down", swapped, {{{5, 1}, {5, 1}}});
}

TEST(MoveLines, SeveralLinesDown) {
    check(five, {{{4, 4}, {2, 2}}}, "move_down",
          {"first", "fifth", "second line", "third line", "fourth line"}, {{{5, 4}, {3, 2}}});
}

TEST(MoveLines, ASelectionThatEndsAtTheStartOfALineLeavesThatLine) {
    check(five, {{{2, 1}, {1, 1}}}, "move_down",
          {"second line", "first", "third line", "fourth line", "fifth"}, {{{3, 1}, {2, 1}}});
}

TEST(MoveLines, ACaretOnAnEmptyLineMovesWithIt) {
    check({"a", "", "b"}, {{{2, 1}, {2, 1}}}, "move_down", {"a", "b", ""}, {{{3, 1}, {3, 1}}});
    check({"a", "", ""}, {{{2, 1}, {2, 1}}}, "move_down", {"a", "", ""}, {{{3, 1}, {3, 1}}});
}

TEST(MoveLines, CaretsOnSeparateLinesAllMove) {
    check({"a", "b", "c", "d"}, {{{2, 1}, {2, 1}}, {{4, 1}, {4, 1}}}, "move_up",
          {"b", "a", "d", "c"}, {{{1, 1}, {1, 1}}, {{3, 1}, {3, 1}}});
}

TEST(MoveLines, OfTwoCaretsWhoseLinesWouldBeSwappedTogetherOnlyTheFirstMoves) {
    check({"a", "b", "c", "d"}, {{{2, 1}, {2, 1}}, {{3, 1}, {3, 1}}}, "move_up",
          {"b", "a", "c", "d"}, {{{1, 1}, {1, 1}}});
    check({"a", "b", "c", "d"}, {{{2, 1}, {2, 1}}, {{3, 1}, {3, 1}}}, "move_down",
          {"a", "c", "b", "d"}, {{{3, 1}, {3, 1}}});
}

TEST(MoveLines, EachMoveIsOneUndoStep) {
    Editor editor({"a", "b", "c", "d"}, {{{2, 1}, {2, 1}}, {{4, 1}, {4, 1}}});
    editor.controller.move_lines(false);
    ASSERT_EQ(editor.lines(), (Lines{"b", "a", "d", "c"}));
    editor.controller.undo();
    EXPECT_EQ(editor.lines(), (Lines{"a", "b", "c", "d"}));
    expect_selections(editor, {{{2, 1}, {2, 1}}, {{4, 1}, {4, 1}}}, "after undo");
}

TEST(MoveLines, AMoveThatChangesNoTextOnlyMovesTheSelections) {
    Editor editor({"a", "", ""}, {{{2, 1}, {2, 1}}});
    const std::uint64_t version = editor.document.version();
    const EditOutcome outcome = editor.controller.move_lines(true);
    EXPECT_FALSE(outcome.change.has_value());
    EXPECT_TRUE(outcome.selections_changed);
    EXPECT_EQ(editor.document.version(), version);
}

TEST(DuplicateSelection, CaretsCopyTheirLinesBelowThemAndMoveDown) {
    check(
        five, {{{2, 2}, {2, 2}}, {{3, 2}, {3, 2}}}, "duplicate",
        {"first", "second line", "second line", "third line", "third line", "fourth line", "fifth"},
        {{{3, 2}, {3, 2}}, {{5, 2}, {5, 2}}});
}

TEST(DuplicateSelection, ASelectionIsCopiedAfterItselfAndTheCopyIsSelected) {
    check(five, {{{2, 1}, {2, 4}}, {{3, 1}, {3, 4}}}, "duplicate",
          {"first", "secsecond line", "thithird line", "fourth line", "fifth"},
          {{{2, 4}, {2, 7}}, {{3, 4}, {3, 7}}});
}

TEST(DuplicateSelection, ASelectionOfSeveralLinesIsCopiedAfterItsEnd) {
    check({"ab", "cd"}, {{{1, 2}, {2, 1}}}, "duplicate", {"ab", "b", "cd"}, {{{2, 1}, {3, 1}}});
}

// --- model-based ---

namespace {

// The documents of the reference: arrays of lines, and positions as (line, byte column).
struct RefPos {
    std::size_t line = 0;
    std::size_t column = 0;
    bool operator==(const RefPos&) const = default;
    bool operator<(const RefPos& other) const {
        return line != other.line ? line < other.line : column < other.column;
    }
};
struct RefSel {
    RefPos anchor;
    RefPos active;
    RefPos start() const {
        return std::min(anchor, active);
    }
    RefPos end() const {
        return std::max(anchor, active);
    }
};
struct RefBlock {
    std::size_t first;
    std::size_t last;
    std::size_t end_line;
};

RefBlock ref_block(const RefSel& s) {
    RefBlock b{s.start().line, s.end().line, s.end().line};
    if (b.first < b.end_line && s.end().column == 0) {
        --b.last;
    }
    return b;
}

struct Result {
    std::vector<std::string> lines;
    std::vector<RefSel> selections;
    std::size_t primary = 0;
};

Result ref_move(std::vector<std::string> lines, const std::vector<RefSel>& selections,
                std::size_t primary, bool down) {
    const std::size_t n = selections.size();
    std::vector<RefBlock> blocks;
    std::vector<bool> moves(n);
    std::vector<std::size_t> from(n), to(n);
    for (std::size_t i = 0; i < n; ++i) {
        blocks.push_back(ref_block(selections[i]));
        moves[i] = down ? blocks[i].end_line + 1 < lines.size() : blocks[i].first > 0;
        from[i] = down ? blocks[i].first : blocks[i].first - (moves[i] ? 1 : 0);
        to[i] = down ? blocks[i].last + 1 : blocks[i].last;
    }
    std::vector<bool> dropped(n, false);
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = i + 1; j < n; ++j) {
            if (moves[i] && moves[j] && from[i] <= to[j] && from[j] <= to[i]) {
                dropped[j] = true;
            }
        }
    }
    Result result{lines, {}, 0};
    std::vector<std::pair<std::size_t, std::size_t>> regions;
    for (std::size_t i = 0; i < n; ++i) {
        if (!moves[i] || dropped[i]) {
            continue;
        }
        regions.push_back({from[i], to[i]});
        std::vector<std::string> region(lines.begin() + static_cast<std::ptrdiff_t>(from[i]),
                                        lines.begin() + static_cast<std::ptrdiff_t>(to[i]) + 1);
        if (down) {
            std::rotate(region.begin(), region.end() - 1, region.end());
        } else {
            std::rotate(region.begin(), region.begin() + 1, region.end());
        }
        std::copy(region.begin(), region.end(),
                  result.lines.begin() + static_cast<std::ptrdiff_t>(from[i]));
    }
    const auto shift = [&](RefPos p) { return RefPos{down ? p.line + 1 : p.line - 1, p.column}; };
    for (std::size_t i = 0; i < n; ++i) {
        if (i == primary) {
            result.primary = result.selections.size();
        }
        if (dropped[i]) {
            continue;
        }
        RefSel s = selections[i];
        if (moves[i]) {
            s = {shift(s.anchor), shift(s.active)};
        } else {
            for (RefPos* p : {&s.anchor, &s.active}) {
                for (const auto& [first, last] : regions) {
                    const bool at_start = p->line == first && p->column == 0;
                    const bool at_end = p->line == last && p->column == lines[last].size();
                    if (first <= p->line && p->line <= last) {
                        // The ends of the swapped lines stay where they are in the text; a
                        // position between them goes to the start.
                        *p = at_end     ? RefPos{last, result.lines[last].size()}
                             : at_start ? *p
                                        : RefPos{first, 0};
                    }
                }
            }
        }
        result.selections.push_back(s);
    }
    result.primary = std::min(result.primary, result.selections.size() - 1);
    return result;
}

Result ref_copy(std::vector<std::string> lines, const std::vector<RefSel>& selections,
                std::size_t primary, bool down) {
    const std::size_t n = selections.size();
    std::vector<RefBlock> blocks;
    for (const RefSel& s : selections) {
        blocks.push_back(ref_block(s));
    }
    std::vector<bool> copies(n, true);
    std::size_t copying = 0;
    for (std::size_t i = 1; i < n; ++i) {
        if (blocks[copying].end_line == blocks[i].first) {
            copies[i] = false;
        } else {
            copying = i;
        }
    }
    // The copies, from the last block to the first so that earlier line numbers stay valid; the
    // lines each position ends up on are counted with the blocks before it.
    Result result{lines, {}, primary};
    for (std::size_t k = n; k-- > 0;) {
        if (!copies[k]) {
            continue;
        }
        const std::vector<std::string> block(
            lines.begin() + static_cast<std::ptrdiff_t>(blocks[k].first),
            lines.begin() + static_cast<std::ptrdiff_t>(blocks[k].last) + 1);
        const std::size_t at = down ? blocks[k].first : blocks[k].last + 1;
        result.lines.insert(result.lines.begin() + static_cast<std::ptrdiff_t>(at), block.begin(),
                            block.end());
    }
    for (const RefSel& s : selections) {
        const auto map = [&](RefPos p) {
            std::size_t extra = 0;
            for (std::size_t k = 0; k < n; ++k) {
                if (!copies[k]) {
                    continue;
                }
                const std::size_t height = blocks[k].last - blocks[k].first + 1;
                // The line of the block and the start of the line after it, when a selection ends
                // there, belong to the block.
                const bool in_block = (p.line >= blocks[k].first && p.line <= blocks[k].last) ||
                                      (blocks[k].last < blocks[k].end_line &&
                                       p.line == blocks[k].end_line && p.column == 0);
                if (in_block) {
                    extra += down ? height : 0;
                } else if (p.line > blocks[k].last) {
                    extra += height;
                }
            }
            return RefPos{p.line + extra, p.column};
        };
        result.selections.push_back({map(s.anchor), map(s.active)});
    }
    return result;
}

Result ref_duplicate(std::vector<std::string> lines, const std::vector<RefSel>& selections,
                     std::size_t primary) {
    // Carets copy their line below it; a selection is copied after itself, which splits lines, so
    // this one works on the joined text.
    std::string text;
    std::vector<std::size_t> starts;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        starts.push_back(text.size());
        text += lines[i] + (i + 1 < lines.size() ? "\n" : "");
    }
    struct Insert {
        std::size_t at;
        std::string text;
        bool caret_line;
        std::size_t lo;
        std::size_t hi;
    };
    std::vector<Insert> inserts;
    const auto off = [&](RefPos p) { return starts[p.line] + p.column; };
    for (const RefSel& s : selections) {
        if (s.anchor == s.active) {
            inserts.push_back({starts[s.active.line], lines[s.active.line] + "\n", true,
                               starts[s.active.line],
                               starts[s.active.line] + lines[s.active.line].size()});
        } else {
            inserts.push_back({off(s.end()),
                               text.substr(off(s.start()), off(s.end()) - off(s.start())), false, 0,
                               0});
        }
    }
    const auto map = [&](std::size_t offset) {
        std::size_t result = offset;
        for (const Insert& in : inserts) {
            const bool owned = in.caret_line && in.lo <= offset && offset <= in.hi;
            if (owned || in.at < offset) {
                result += in.text.size();
            }
        }
        return result;
    };
    std::vector<std::pair<std::size_t, std::size_t>> after;
    for (std::size_t i = 0; i < selections.size(); ++i) {
        const RefSel& s = selections[i];
        if (s.anchor == s.active) {
            after.push_back({map(off(s.active)), map(off(s.active))});
        } else {
            std::size_t start = inserts[i].at;
            for (std::size_t j = 0; j < inserts.size(); ++j) {
                if (inserts[j].at < inserts[i].at || (inserts[j].at == inserts[i].at && j < i)) {
                    start += inserts[j].text.size();
                }
            }
            after.push_back({start, start + inserts[i].text.size()});
        }
    }
    std::string out = text;
    std::vector<std::size_t> order(inserts.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    // Texts inserted at the same place keep the order of their selections, so the later one goes in
    // first.
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return inserts[a].at != inserts[b].at ? inserts[a].at > inserts[b].at : a > b;
    });
    for (std::size_t i : order) {
        out.insert(inserts[i].at, inserts[i].text);
    }
    Result result{{}, {}, primary};
    std::string line;
    for (char c : out) {
        if (c == '\n') {
            result.lines.push_back(line);
            line.clear();
        } else {
            line += c;
        }
    }
    result.lines.push_back(line);
    const auto pos = [&](std::size_t offset) {
        std::size_t start = 0;
        for (std::size_t l = 0; l < result.lines.size(); ++l) {
            if (offset <= start + result.lines[l].size()) {
                return RefPos{l, offset - start};
            }
            start += result.lines[l].size() + 1;
        }
        return RefPos{};
    };
    for (const auto& [a, b] : after) {
        result.selections.push_back({pos(a), pos(b)});
    }
    return result;
}

class Random {
public:
    explicit Random(unsigned seed) : engine_(seed) {}
    std::size_t below(std::size_t bound) {
        return std::uniform_int_distribution<std::size_t>(0, bound - 1)(engine_);
    }

private:
    std::mt19937 engine_;
};

TEST(LineOperationsModel, EveryCommandAgreesWithTheReferenceOnRandomDocuments) {
    Random random(2026);
    int dropped_cases = 0;
    for (int round = 0; round < 6000; ++round) {
        std::vector<std::string> lines(1 + random.below(7));
        for (std::string& line : lines) {
            const std::size_t length = random.below(4);
            static const char* pieces[] = {"a", "b", " ", "c", "\xC3\xA9"};
            for (std::size_t i = 0; i < length; ++i) {
                line += pieces[random.below(std::size(pieces))];
            }
        }
        std::vector<RefSel> raw;
        const std::size_t count = 1 + random.below(3);
        for (std::size_t i = 0; i < count; ++i) {
            const auto position = [&] {
                const std::size_t line = random.below(lines.size());
                // A column on a character boundary.
                std::vector<std::size_t> boundaries;
                for (std::size_t i = 0; i <= lines[line].size(); ++i) {
                    if (i == lines[line].size() ||
                        (static_cast<unsigned char>(lines[line][i]) & 0xC0u) != 0x80u) {
                        boundaries.push_back(i);
                    }
                }
                return RefPos{line, boundaries[random.below(boundaries.size())]};
            };
            RefSel selection{position(), position()};
            if (random.below(2) == 0) {
                selection.active = selection.anchor;
            }
            raw.push_back(selection);
        }
        const std::size_t command = random.below(5);

        std::string text;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            text += lines[i] + (i + 1 < lines.size() ? "\n" : "");
        }
        Document document(*Text::from_utf8(text));
        std::vector<Selection> offsets;
        for (const RefSel& s : raw) {
            offsets.push_back({document.text().line_start(s.anchor.line) + s.anchor.column,
                               document.text().line_start(s.active.line) + s.active.column});
        }
        ASSERT_TRUE(document.set_selections(SelectionSet::from(std::move(offsets), 0)).has_value());
        // The selections as the document keeps them: ordered, and merged when they overlap.
        std::vector<RefSel> input;
        const auto ref_pos = [&](std::size_t offset) {
            const std::size_t line = document.text().position_at(offset).line;
            return RefPos{line, offset - document.text().line_start(line)};
        };
        for (const Selection& s : document.selections().all()) {
            input.push_back({ref_pos(s.anchor), ref_pos(s.active)});
        }
        const std::size_t primary = document.selections().primary_index();

        Result expected;
        EditController controller(document);
        switch (command) {
        case 0:
            expected = ref_move(lines, input, primary, true);
            controller.move_lines(true);
            break;
        case 1:
            expected = ref_move(lines, input, primary, false);
            controller.move_lines(false);
            break;
        case 2:
            expected = ref_copy(lines, input, primary, true);
            controller.copy_lines(true);
            break;
        case 3:
            expected = ref_copy(lines, input, primary, false);
            controller.copy_lines(false);
            break;
        default:
            expected = ref_duplicate(lines, input, primary);
            controller.duplicate_selection();
            break;
        }

        std::vector<std::string> actual_lines;
        for (std::size_t l = 0; l < document.text().line_count(); ++l) {
            actual_lines.push_back(document.text().line(l));
        }
        std::string input_trace = "\ninput:";
        for (const RefSel& s : input) {
            input_trace += " (" + std::to_string(s.anchor.line) + "," +
                           std::to_string(s.anchor.column) + ")->(" +
                           std::to_string(s.active.line) + "," + std::to_string(s.active.column) +
                           ")";
        }
        input_trace += "\nlines:";
        for (const std::string& l : lines) {
            input_trace += " [" + l + "]";
        }
        ASSERT_EQ(actual_lines, expected.lines)
            << "round " << round << " command " << command << input_trace;
        // The expected selections go through the same normalization as the document's.
        std::vector<Selection> expected_offsets;
        std::size_t expected_start = 0;
        std::vector<std::size_t> line_starts;
        for (const std::string& l : expected.lines) {
            line_starts.push_back(expected_start);
            expected_start += l.size() + 1;
        }
        for (const RefSel& s : expected.selections) {
            expected_offsets.push_back({line_starts[s.anchor.line] + s.anchor.column,
                                        line_starts[s.active.line] + s.active.column});
        }
        const SelectionSet expected_set =
            SelectionSet::from(std::move(expected_offsets), expected.primary);
        ASSERT_EQ(document.selections().all(), expected_set.all())
            << "round " << round << " command " << command << input_trace;
        if (expected.selections.size() == input.size()) {
            ASSERT_EQ(document.selections().primary(), expected_set.primary())
                << "round " << round << " command " << command;
        } else {
            ++dropped_cases;
        }
    }
    EXPECT_GT(dropped_cases, 50) << "the generator should reach the dropped selections";
}

} // namespace
