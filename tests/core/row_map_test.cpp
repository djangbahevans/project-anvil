#include "anvil/core/row_map.hpp"

#include <algorithm>
#include <gtest/gtest.h>
#include <random>

namespace anvil::core {
namespace {

Document document_of(const std::string& text) {
    return Document(Text::from_utf8(text).value());
}

// The rows of every line, computed one line at a time.
std::vector<RowMap::Row> all_rows(const Text& text, const WrapOptions& options) {
    std::vector<RowMap::Row> rows;
    for (std::size_t line = 0; line < text.line_count(); ++line) {
        const std::string content = text.line(line);
        const LineBreaks breaks = break_line(content, options);
        std::size_t begin = 0;
        for (std::size_t part = 0; part <= breaks.starts.size(); ++part) {
            const std::size_t end =
                part < breaks.starts.size() ? breaks.starts[part] : content.size();
            rows.push_back({line, begin, end, part == 0 ? 0 : breaks.indent});
            begin = end;
        }
    }
    return rows;
}

// Compares every answer of the map, or for a long text those near `near` and at both ends.
void expect_rows(const RowMap& map, const Text& text, std::size_t near = 0) {
    const std::vector<RowMap::Row> rows = all_rows(text, map.options());
    ASSERT_EQ(map.line_count(), text.line_count());
    ASSERT_EQ(map.row_count(), rows.size());
    std::vector<std::size_t> first(text.line_count(), rows.size());
    for (std::size_t row = rows.size(); row-- > 0;) {
        first[rows[row].line] = row;
    }
    const std::size_t lines = text.line_count();
    for (std::size_t line = 0; line < lines; ++line) {
        if (lines > 100 && line > 2 && line + 3 < lines && (line + 40 < near || line > near + 40) &&
            line % 97 != 0) {
            continue;
        }
        ASSERT_EQ(map.first_row(line), first[line]) << "line " << line;
        const std::size_t end = line + 1 < lines ? first[line + 1] : rows.size();
        ASSERT_EQ(map.rows_of(line), end - first[line]) << "line " << line;
        for (std::size_t row = first[line]; row < end; ++row) {
            ASSERT_EQ(map.line_of(row), line) << "row " << row;
            ASSERT_EQ(map.row(text, row), rows[row]) << "row " << row;
        }
    }
    EXPECT_EQ(map.first_row(lines), rows.size());
    EXPECT_EQ(map.line_of(rows.size()), lines - 1);
    EXPECT_EQ(map.line_of(rows.size() + 7), lines - 1);
}

TEST(RowMap, AnEmptyTextHasOneRow) {
    const RowMap map(Text{}, {4, 10, 2});
    EXPECT_EQ(map.line_count(), 1u);
    EXPECT_EQ(map.row_count(), 1u);
    expect_rows(map, Text{});
}

TEST(RowMap, ALineTakesTheRowsOfItsBreaks) {
    const Text text = Text::from_utf8("one two three\n\n    indented words here\n").value();
    const RowMap map(text, {4, 10, 2});
    EXPECT_EQ(map.rows_of(0), 2u);
    EXPECT_EQ(map.first_row(2), 3u);
    // The indent leaves six columns, too few for the word after it, which is split.
    EXPECT_EQ(map.row(text, 3), (RowMap::Row{2, 0, 10, 0}));
    EXPECT_EQ(map.row(text, 4), (RowMap::Row{2, 10, 13, 4}));
    expect_rows(map, text);
}

TEST(RowMap, ALineLongerThanAChunkOfStorageIsWholeToTheRows) {
    std::string long_line;
    for (int i = 0; i < 3000; ++i) {
        long_line += "word ";
    }
    const Text text = Text::from_utf8("a\n" + long_line + "\nb").value();
    ASSERT_GT(text.chunk_count(), 2u);
    const RowMap map(text, {4, 20, 2});
    EXPECT_EQ(map.rows_of(1), 750u);
    expect_rows(map, text);
}

TEST(RowMap, TypingInALineComputesThatLineAlone) {
    std::string lines;
    for (int i = 0; i < 2000; ++i) {
        lines += "some words\n";
    }
    Document document = document_of(lines);
    RowMap map(document.text(), {4, 12, 2});
    const std::size_t at_first = map.lines_computed();
    const std::size_t offset = document.text().line_start(1000);
    const Edit edit{offset, offset, "more and more "};
    map.apply(document.apply_edits(std::span(&edit, 1)).value(), document.text());
    EXPECT_EQ(map.lines_computed(), at_first + 1);
    EXPECT_EQ(map.rows_of(1000), 3u);
    expect_rows(map, document.text(), 1000);
}

TEST(RowMap, SeveralEditsComputeOnlyTheLinesTheyTouch) {
    std::string lines;
    for (int i = 0; i < 1000; ++i) {
        lines += "some words\n";
    }
    Document document = document_of(lines);
    RowMap map(document.text(), {4, 12, 2});
    const std::size_t at_first = map.lines_computed();
    // A line split in three near the start, and two lines joined near the end.
    const std::size_t near_start = document.text().line_start(10) + 4;
    const std::size_t join = document.text().line_end(900);
    const std::vector<Edit> edits{{near_start, near_start, "\nxx\n"}, {join, join + 1, " "}};
    map.apply(document.apply_edits(edits).value(), document.text());
    // The three lines and the joined one; the lines between them are as they were.
    EXPECT_EQ(map.lines_computed(), at_first + 4);
    expect_rows(map, document.text(), 10);
    expect_rows(map, document.text(), 902);
}

TEST(RowMap, ARedoneGroupOfEditsListsThemFromTheTopOfTheTextDown) {
    std::string lines;
    for (int i = 0; i < 1000; ++i) {
        lines += "some words\n";
    }
    Document document = document_of(lines);
    RowMap map(document.text(), {4, 12, 2});
    const auto type_at = [&](std::size_t line, bool join) {
        const std::size_t offset = document.text().line_start(line);
        const Edit edit{offset, offset, "more words\nand "};
        map.apply(document.apply_edits(std::span(&edit, 1), {.join_previous = join}).value(),
                  document.text());
    };
    type_at(100, false);
    type_at(900, true);
    map.apply(document.undo().value().value(), document.text());
    expect_rows(map, document.text(), 100);
    const DocumentChange redone = document.redo().value().value();
    ASSERT_EQ(redone.changes.size(), 2u);
    ASSERT_LT(redone.changes[0].range.start.line, redone.changes[1].range.start.line);
    map.apply(redone, document.text());
    expect_rows(map, document.text(), 100);
    expect_rows(map, document.text(), 901);
}

TEST(RowMap, RandomEditsLeaveTheRowsOfAMapComputedAnew) {
    std::mt19937 random(7);
    const auto below = [&random](std::size_t bound) {
        return std::uniform_int_distribution<std::size_t>(0, bound - 1)(random);
    };
    const std::vector<std::string> pieces{
        "a", "b", " ", " ", "\t", ".", "\n", "(", "\xC3\xA9", "\xE4\xB8\xAD", "\xF0\x9F\x98\x80"};
    const auto random_text = [&](std::size_t length) {
        std::string text;
        for (std::size_t i = 0; i < length; ++i) {
            text += pieces[below(pieces.size())];
        }
        return text;
    };
    for (int round = 0; round < 120; ++round) {
        // Every third text has enough lines for several blocks of the map.
        std::string initial = random_text(below(120));
        if (round % 3 == 0) {
            for (int i = 0; i < 600; ++i) {
                initial += random_text(below(14)) + "\n";
            }
        }
        Document document = document_of(initial);
        const WrapOptions options{1 + below(8), 1 + below(20), below(2) == 0 ? 2.0 : 1.6};
        RowMap map(document.text(), options);
        for (int step = 0; step < 30; ++step) {
            const Text& text = document.text();
            const auto boundary = [&](std::size_t offset) {
                while (!text.is_char_boundary(offset)) {
                    --offset;
                }
                return offset;
            };
            // Up to four edits that do not overlap, so that one batch has several changes.
            std::vector<std::size_t> offsets;
            const std::size_t count = 1 + below(4);
            for (std::size_t i = 0; i < 2 * count; ++i) {
                offsets.push_back(boundary(below(text.size() + 1)));
            }
            std::ranges::sort(offsets);
            std::vector<Edit> edits;
            for (std::size_t i = 0; i < count; ++i) {
                const std::size_t begin = offsets[2 * i];
                // Mostly short removals, so that the text does not vanish.
                const std::size_t end =
                    below(4) == 0
                        ? offsets[2 * i + 1]
                        : std::min(offsets[2 * i + 1], boundary(std::min(begin + 3, text.size())));
                if (!edits.empty() && begin < edits.back().end) {
                    continue;
                }
                edits.push_back({begin, std::max(begin, end), random_text(below(10))});
            }
            const auto change = below(8) == 0 && document.can_undo()
                                    ? document.undo().value().value()
                                    : document.apply_edits(edits).value();
            map.apply(change, document.text());
            const std::size_t near =
                change.changes.empty() ? 0 : change.changes.back().range.start.line;
            SCOPED_TRACE("round " + std::to_string(round) + " step " + std::to_string(step));
            // A lookup before the full comparison leaves the sums made in part.
            if (below(2) == 0) {
                map.first_row(below(map.line_count()));
            }
            expect_rows(map, document.text(), near);
            ASSERT_FALSE(HasFatalFailure());
        }
    }
}

} // namespace
} // namespace anvil::core
