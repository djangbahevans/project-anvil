#include "anvil/core/editing.hpp"
#include "anvil/core/row_map.hpp"
#include "anvil/core/visible_column.hpp"

#include <algorithm>
#include <gtest/gtest.h>
#include <optional>
#include <random>

namespace anvil::core {
namespace {

// A document with a controller whose lines wrap, the map brought up to date when asked for.
struct Wrapped {
    Wrapped(const std::string& text, WrapOptions options)
        : document(Text::from_utf8(text).value()), controller(document), options(options) {
        controller.set_tab_size(options.tab_size);
        controller.set_row_source([this] {
            map.emplace(document.text(), this->options);
            return &*map;
        });
    }
    Selection primary() const {
        return document.selections().primary();
    }
    std::size_t at() const {
        return primary().active;
    }

    Document document;
    EditController controller;
    WrapOptions options;
    std::optional<RowMap> map;
};

constexpr WrapOptions five{4, 5, 2};

TEST(RowMotion, EndGoesToTheEndOfTheRowAndThenOfTheLine) {
    Wrapped w("aaa bbb ccc\nnext", {4, 5, 2});
    w.controller.set_caret(1);
    w.controller.move(Motion::line_end);
    EXPECT_EQ(w.at(), 4U);
    EXPECT_TRUE(w.controller.ends_row(4));
    w.controller.move(Motion::line_end);
    EXPECT_EQ(w.at(), 11U);
    EXPECT_FALSE(w.controller.ends_row(11));
}

TEST(RowMotion, HomeGoesToTheStartOfTheRowAndThenAsOnTheLine) {
    Wrapped w("  aaa bbb ccc", {4, 8, 2});
    // The rows are "  aaa ", "bbb " and "ccc", the later ones indented by two columns.
    ASSERT_EQ(w.controller.set_caret(8).selections_changed, true);
    w.controller.move(Motion::home);
    EXPECT_EQ(w.at(), 6U);
    EXPECT_FALSE(w.controller.ends_row(6));
    w.controller.move(Motion::home);
    EXPECT_EQ(w.at(), 2U) << "the first character of the line";
    w.controller.move(Motion::home);
    EXPECT_EQ(w.at(), 0U);
}

TEST(RowMotion, HomeAtTheEndOfARowGoesToTheStartOfThatRow) {
    Wrapped w("aaa bbb ccc", five);
    w.controller.set_caret(8, false, true);
    ASSERT_TRUE(w.controller.ends_row(8));
    w.controller.move(Motion::home);
    EXPECT_EQ(w.at(), 4U);
}

TEST(RowMotion, UpAndDownKeepTheColumnOnTheRows) {
    Wrapped w("abcde fg hijkl", {4, 6, 2});
    // The rows are "abcde ", "fg " and "hijkl".
    w.controller.set_caret(4);
    w.controller.move(Motion::down);
    EXPECT_EQ(w.at(), 9U) << "the end of the short row";
    EXPECT_TRUE(w.controller.ends_row(9));
    w.controller.move(Motion::down);
    EXPECT_EQ(w.at(), 13U);
    EXPECT_FALSE(w.controller.ends_row(13));
    w.controller.move(Motion::up);
    w.controller.move(Motion::up);
    EXPECT_EQ(w.at(), 4U);
}

TEST(RowMotion, TheIndentOfARowCountsAsColumns) {
    Wrapped w("    abc defg", {4, 9, 2});
    // "    abc " and "defg" indented by four columns.
    w.controller.set_caret(6);
    w.controller.move(Motion::down);
    EXPECT_EQ(w.at(), 10U) << "column 6 is two into the row";
    w.controller.set_caret(1);
    w.controller.move(Motion::down);
    EXPECT_EQ(w.at(), 8U) << "a column in the indent is the start of the row";
}

TEST(RowMotion, TheSideOfARowIsForgottenWhenTheSelectionsOrTheTextChange) {
    Wrapped w("aaa bbb ccc", five);
    w.controller.set_caret(4, false, true);
    ASSERT_TRUE(w.controller.ends_row(4));
    ASSERT_TRUE(w.document.set_selections(SelectionSet::from({Selection::caret(1)})).has_value());
    EXPECT_FALSE(w.controller.ends_row(4));

    w.controller.set_caret(4, false, true);
    w.controller.type("x");
    EXPECT_FALSE(w.controller.ends_row(4));
    EXPECT_FALSE(w.controller.ends_row(5));

    w.controller.set_caret(4, false, true);
    w.controller.move(Motion::left);
    EXPECT_FALSE(w.controller.ends_row(3));
}

TEST(RowMotion, TheSameOffsetOnTheOtherRowIsAChange) {
    Wrapped w("aaa bbb ccc", five);
    w.controller.set_caret(4);
    EXPECT_TRUE(w.controller.set_caret(4, false, true).selections_changed);
    EXPECT_FALSE(w.controller.set_caret(4, false, true).selections_changed);
    EXPECT_TRUE(w.controller.set_caret(4).selections_changed);
    EXPECT_FALSE(w.controller.ends_row(4));
}

TEST(RowMotion, EveryCursorKeepsItsOwnSideOfItsRow) {
    Wrapped w("aaa bbb ccc ddd", five);
    w.controller.set_caret(4, false, true);
    w.controller.add_caret(12, true);
    EXPECT_TRUE(w.controller.ends_row(4));
    EXPECT_TRUE(w.controller.ends_row(12));
    w.controller.set_caret(14, true);
    EXPECT_TRUE(w.controller.ends_row(4)) << "extending moves the primary selection alone";
    w.controller.set_caret(12, true, true);
    w.controller.keep_primary_selection();
    EXPECT_TRUE(w.controller.ends_row(12));
    EXPECT_FALSE(w.controller.ends_row(4));
}

TEST(RowMotion, SeveralCursorsMoveByRows) {
    Wrapped w("aaa bbb ccc\nddd eee", five);
    w.controller.set_caret(1);
    w.controller.add_caret(13);
    w.controller.move(Motion::down);
    EXPECT_EQ(w.document.selections().all(),
              (std::vector<Selection>{Selection::caret(5), Selection::caret(17)}));
    w.controller.move(Motion::line_end);
    EXPECT_TRUE(w.controller.ends_row(8));
    EXPECT_FALSE(w.controller.ends_row(19)) << "the last row of its line";
}

TEST(RowMotion, CursorsAreAddedByLinesAsInVsCode) {
    Wrapped w("aaa bbb ccc\nddd eee", five);
    w.controller.set_caret(1);
    w.controller.add_cursors_vertically(true);
    EXPECT_EQ(w.document.selections().all(),
              (std::vector<Selection>{Selection::caret(1), Selection::caret(13)}));
}

// --- model-based ---

// A row of the reference: the offsets it spans, its indent and whether its line goes on.
struct Row {
    std::size_t line = 0;
    std::size_t begin = 0;
    std::size_t end = 0;
    std::size_t indent = 0;
    bool continues = false;
};

struct Reference {
    std::string text = {};
    std::size_t tab_size = 4;
    std::vector<Row> rows = {};
    std::size_t offset = 0;
    bool ends_row = false;
    std::optional<std::size_t> goal = {};

    std::size_t row() const {
        std::size_t found = 0;
        for (std::size_t r = 0; r < rows.size(); ++r) {
            if (rows[r].begin <= offset && offset <= rows[r].end) {
                found = r;
                // An offset where a line breaks is on two rows.
                if (ends_row || !rows[r].continues || offset < rows[r].end) {
                    break;
                }
            }
        }
        return found;
    }
    std::string shown(const Row& r) const {
        return std::string(r.indent, ' ') + text.substr(r.begin, r.end - r.begin);
    }
    void vertical(int direction) {
        const Row& from = rows[row()];
        const std::size_t column =
            goal ? *goal : visible_column(shown(from), from.indent + offset - from.begin, tab_size);
        const bool at_edge = offset == (direction < 0 ? 0 : text.size());
        const std::size_t current = row();
        if (direction < 0 && current == 0) {
            offset = 0;
            ends_row = false;
        } else if (direction > 0 && current + 1 == rows.size()) {
            offset = text.size();
            ends_row = false;
        } else {
            const Row& to = rows[direction < 0 ? current - 1 : current + 1];
            const std::size_t at = offset_at_visible_column(shown(to), column, tab_size);
            offset = to.begin + (at > to.indent ? at - to.indent : 0);
            ends_row = to.continues && offset == to.end;
        }
        goal = at_edge ? std::nullopt : std::optional(column);
    }
    std::size_t line_begin(std::size_t line) const {
        return std::ranges::find(rows, line, &Row::line)->begin;
    }
    std::size_t line_end() const {
        const std::size_t at = text.find('\n', offset);
        return at == std::string::npos ? text.size() : at;
    }
    void home() {
        const Row& r = rows[row()];
        const auto first_text = [&](std::size_t from, std::size_t to) {
            while (from < to && (text[from] == ' ' || text[from] == '\t')) {
                ++from;
            }
            return from;
        };
        const bool first_row = r.begin == line_begin(r.line);
        const std::size_t found = first_text(r.begin, r.end);
        const bool by_row = !first_row && (found == r.end || offset != found);
        if (by_row) {
            offset = found == r.end ? r.begin : found;
        } else {
            const std::size_t start = line_begin(r.line);
            std::size_t end = start;
            while (end < text.size() && text[end] != '\n') {
                ++end;
            }
            const std::size_t word = first_text(start, end);
            offset = word == end || offset == word ? start : word;
        }
        ends_row = false;
        goal.reset();
    }
    void end() {
        const Row& r = rows[row()];
        if (r.continues && offset != r.end) {
            offset = r.end;
            ends_row = true;
        } else {
            offset = line_end();
            ends_row = false;
        }
        goal.reset();
    }
};

TEST(RowMotion, RandomMotionsAgreeWithAListOfRows) {
    std::mt19937 random(11);
    const auto below = [&random](std::size_t bound) {
        return std::uniform_int_distribution<std::size_t>(0, bound - 1)(random);
    };
    const std::vector<std::string> pieces{"a",
                                          "b",
                                          "c",
                                          " ",
                                          " ",
                                          " ",
                                          "\t",
                                          ".",
                                          "\n",
                                          "(",
                                          "-",
                                          "d",
                                          "\xC3\xA9",
                                          "\xE4\xB8\xAD",
                                          "\xF0\x9F\x98\x80"};
    std::size_t checked_row_ends = 0;
    for (int round = 0; round < 600; ++round) {
        std::string text = below(3) == 0 ? "    " : "";
        for (std::size_t i = below(60); i > 0; --i) {
            text += pieces[below(pieces.size())];
        }
        const WrapOptions options{1 + below(6), 1 + below(14), 2};
        Wrapped w(text, options);
        Reference model{text, options.tab_size};
        const Text& snapshot = w.document.text();
        for (std::size_t line = 0; line < snapshot.line_count(); ++line) {
            const std::string content = snapshot.line(line);
            const LineBreaks breaks = break_line(content, options);
            std::size_t begin = 0;
            for (std::size_t part = 0; part <= breaks.starts.size(); ++part) {
                const bool continues = part < breaks.starts.size();
                const std::size_t end = continues ? breaks.starts[part] : content.size();
                model.rows.push_back({line, snapshot.line_start(line) + begin,
                                      snapshot.line_start(line) + end,
                                      part == 0 ? 0 : breaks.indent, continues});
                begin = end;
            }
        }
        std::string steps;
        for (int step = 0; step < 25; ++step) {
            const std::size_t choice = below(6);
            if (choice == 0) {
                w.controller.move(Motion::up);
                model.vertical(-1);
                steps += " up";
            } else if (choice == 1) {
                w.controller.move(Motion::down);
                model.vertical(1);
                steps += " down";
            } else if (choice == 2) {
                w.controller.move(Motion::home);
                model.home();
                steps += " home";
            } else if (choice == 3) {
                w.controller.move(Motion::line_end);
                model.end();
                steps += " end";
            } else {
                // A caret somewhere, often at the end of a row that goes on.
                const Row& r = model.rows[below(model.rows.size())];
                const bool at_end = choice == 4 && r.continues;
                std::size_t offset = at_end ? r.end : r.begin + below(r.end - r.begin + 1);
                while (!snapshot.is_char_boundary(offset)) {
                    --offset;
                }
                w.controller.set_caret(offset, false, at_end);
                model.offset = offset;
                model.ends_row = at_end;
                model.goal.reset();
                steps += at_end ? " row-end:" : " at:";
                steps += std::to_string(offset);
            }
            ASSERT_EQ(w.at(), model.offset)
                << "round " << round << " column " << options.column << " tab " << options.tab_size
                << " text " << testing::PrintToString(text) << steps;
            ASSERT_EQ(w.controller.ends_row(w.at()), model.ends_row)
                << "round " << round << " column " << options.column << " text "
                << testing::PrintToString(text) << steps;
            checked_row_ends += model.ends_row ? 1 : 0;
        }
    }
    EXPECT_GT(checked_row_ends, 500U);
}

} // namespace
} // namespace anvil::core
