#include "anvil/core/whitespace.hpp"

#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace anvil::core {
namespace {

using Columns = std::vector<std::size_t>;

Columns marks(std::u16string_view line, RenderWhitespace mode,
              std::vector<ColumnRange> selections = {}) {
    return whitespace_columns(line, mode, selections);
}

TEST(Whitespace, NoneDrawsNothingAndAllDrawsEverySpaceAndTab) {
    EXPECT_EQ(marks(u" a \tb  ", RenderWhitespace::none), Columns{});
    EXPECT_EQ(marks(u" a \tb  ", RenderWhitespace::all), (Columns{0, 2, 3, 5, 6}));
    EXPECT_EQ(marks(u"", RenderWhitespace::all), Columns{});
}

TEST(Whitespace, BoundaryLeavesOutASingleSpaceBetweenWords) {
    EXPECT_EQ(marks(u"a b", RenderWhitespace::boundary), Columns{});
    EXPECT_EQ(marks(u"a  b", RenderWhitespace::boundary), (Columns{1, 2}));
    EXPECT_EQ(marks(u"a \tb", RenderWhitespace::boundary), (Columns{1, 2}));
    EXPECT_EQ(marks(u"a\tb", RenderWhitespace::boundary), (Columns{1}));
    EXPECT_EQ(marks(u"  a b  ", RenderWhitespace::boundary), (Columns{0, 1, 5, 6}));
    EXPECT_EQ(marks(u" ", RenderWhitespace::boundary), (Columns{0}));
}

TEST(Whitespace, TrailingDrawsTheEndOfTheLineAndAllOfABlankLine) {
    EXPECT_EQ(marks(u"a b  ", RenderWhitespace::trailing), (Columns{3, 4}));
    EXPECT_EQ(marks(u"  a b", RenderWhitespace::trailing), Columns{});
    EXPECT_EQ(marks(u" \t ", RenderWhitespace::trailing), (Columns{0, 1, 2}));
    EXPECT_EQ(marks(u"a\t", RenderWhitespace::trailing), (Columns{1}));
}

TEST(Whitespace, SelectionDrawsOnlyWhatTheSelectionsCover) {
    EXPECT_EQ(marks(u"a b c d", RenderWhitespace::selection), Columns{});
    EXPECT_EQ(marks(u"a b c d", RenderWhitespace::selection, {{0, 4}}), (Columns{1, 3}));
    EXPECT_EQ(marks(u"a b c d", RenderWhitespace::selection, {{1, 2}, {5, 6}}), (Columns{1, 5}));
    EXPECT_EQ(marks(u" a ", RenderWhitespace::selection, {{1, 2}}), Columns{})
        << "a selection that covers no whitespace";
}

TEST(Whitespace, OnlyTheWindowIsReturned) {
    EXPECT_EQ(whitespace_columns(u"a  b  c  d", RenderWhitespace::all, {}, 3, 7), (Columns{4, 5}));
    EXPECT_EQ(whitespace_columns(u"a  b", RenderWhitespace::all, {}, 0, 100), (Columns{1, 2}));
    EXPECT_EQ(whitespace_columns(u"a  b", RenderWhitespace::all, {}, 3, 1), Columns{});
}

// VS Code's loop (`_applyRenderWhitespace` of viewLineRenderer.ts) for one line, as its code reads.
Columns reference(std::u16string_view line, RenderWhitespace mode,
                  const std::vector<ColumnRange>& selections) {
    const auto is_blank = [](char16_t c) { return c == u' ' || c == u'\t'; };
    Columns result;
    if (mode == RenderWhitespace::none) {
        return result;
    }
    const long length = static_cast<long>(line.size());
    long first = 0;
    while (first < length && is_blank(line[static_cast<std::size_t>(first)])) {
        ++first;
    }
    const bool empty_or_whitespace = first == length;
    long last = length;
    if (!empty_or_whitespace) {
        last = length - 1;
        while (is_blank(line[static_cast<std::size_t>(last)])) {
            --last;
        }
    }
    bool was_in_whitespace = false;
    for (long index = 0; index < length; ++index) {
        const char16_t c = line[static_cast<std::size_t>(index)];
        bool in_whitespace = false;
        if (index < first || index > last) {
            in_whitespace = true;
        } else if (c == u'\t') {
            in_whitespace = true;
        } else if (c == u' ') {
            if (mode == RenderWhitespace::boundary) {
                if (was_in_whitespace) {
                    in_whitespace = true;
                } else {
                    const char16_t next =
                        index + 1 < length ? line[static_cast<std::size_t>(index + 1)] : u'\0';
                    in_whitespace = next == u' ' || next == u'\t';
                }
            } else {
                in_whitespace = true;
            }
        }
        if (in_whitespace && mode == RenderWhitespace::selection) {
            bool inside = false;
            for (const ColumnRange& range : selections) {
                inside = inside || (range.begin <= static_cast<std::size_t>(index) &&
                                    static_cast<std::size_t>(index) < range.end);
            }
            in_whitespace = inside;
        }
        if (in_whitespace && mode == RenderWhitespace::trailing) {
            in_whitespace = empty_or_whitespace || index > last;
        }
        was_in_whitespace = in_whitespace;
        if (in_whitespace) {
            result.push_back(static_cast<std::size_t>(index));
        }
    }
    return result;
}

TEST(Whitespace, EveryShortLineAgreesWithVSCodesLoop) {
    const char16_t alphabet[] = {u'a', u' ', u'\t'};
    const std::vector<std::vector<ColumnRange>> selection_sets = {{}, {{0, 3}}, {{1, 2}, {4, 7}}};
    for (std::size_t length = 0; length <= 7; ++length) {
        std::size_t combinations = 1;
        for (std::size_t i = 0; i < length; ++i) {
            combinations *= 3;
        }
        for (std::size_t n = 0; n < combinations; ++n) {
            std::u16string line;
            for (std::size_t i = 0, rest = n; i < length; ++i, rest /= 3) {
                line.push_back(alphabet[rest % 3]);
            }
            for (const RenderWhitespace mode :
                 {RenderWhitespace::none, RenderWhitespace::boundary, RenderWhitespace::selection,
                  RenderWhitespace::trailing, RenderWhitespace::all}) {
                for (const auto& selections : selection_sets) {
                    ASSERT_EQ(marks(line, mode, selections), reference(line, mode, selections))
                        << "line #" << n << " of length " << length << ", mode "
                        << static_cast<int>(mode);
                }
            }
        }
    }
}

} // namespace
} // namespace anvil::core
