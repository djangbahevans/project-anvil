#include "anvil/core/live_highlighter.hpp"

#include <gtest/gtest.h>
#include <map>

using namespace anvil::core;

namespace {

// Spans that name the line they came from: line `n` has one span of `n` to `n + 1` and the scope
// `line<n>`, so that a test can see which line of the result a line was taken from.
class NumberedLines final : public Highlighter {
public:
    explicit NumberedLines(std::size_t lines) : lines_(lines) {
        for (std::size_t i = 0; i < lines; ++i) {
            scopes_.push_back("line" + std::to_string(i));
        }
    }
    std::vector<HighlightSpan> spans(std::size_t line) const override {
        if (line >= lines_) {
            return {};
        }
        return {{line, line + 1, scopes_[line]}};
    }
    std::string_view root_scope() const override {
        return "source.test";
    }
    std::uint64_t version() const override {
        return 1;
    }

private:
    std::size_t lines_;
    std::vector<std::string> scopes_;
};

TextChange replacing(std::size_t first_line, std::size_t last_line, std::string text) {
    TextChange change;
    change.range = Range{{first_line, 0}, {last_line, 0}};
    change.text = std::move(text);
    return change;
}

DocumentChange version_with(std::uint64_t version, std::vector<TextChange> changes = {}) {
    return DocumentChange{version, ChangeCause::edit, std::move(changes)};
}

std::string scope_of(const LiveHighlighter& highlighter, std::size_t line) {
    const auto spans = highlighter.spans(line);
    return spans.empty() ? std::string("-") : std::string(spans[0].scope);
}

} // namespace

TEST(LiveHighlighter, HasNoSpansBeforeTheFirstResult) {
    LiveHighlighter highlighter("source.test", 1);
    EXPECT_TRUE(highlighter.spans(0).empty());
    EXPECT_EQ(highlighter.root_scope(), "source.test");
    EXPECT_EQ(highlighter.parsed_version(), 0u);
}

TEST(LiveHighlighter, AResultForTheCurrentVersionIsUsed) {
    LiveHighlighter highlighter("source.test", 1);
    ASSERT_TRUE(highlighter.deliver(1, std::make_shared<NumberedLines>(5)));
    EXPECT_EQ(scope_of(highlighter, 3), "line3");
    EXPECT_EQ(highlighter.parsed_version(), 1u);
}

TEST(LiveHighlighter, AResultForAnOlderVersionIsDropped) {
    LiveHighlighter highlighter("source.test", 1);
    ASSERT_TRUE(highlighter.deliver(1, std::make_shared<NumberedLines>(5)));
    highlighter.edited(version_with(2, {replacing(1, 1, "x")}));
    highlighter.edited(version_with(3, {replacing(1, 1, "y")}));

    EXPECT_FALSE(highlighter.deliver(2, std::make_shared<NumberedLines>(50)));
    EXPECT_EQ(highlighter.parsed_version(), 1u);
    EXPECT_TRUE(highlighter.spans(40).empty());

    EXPECT_TRUE(highlighter.deliver(3, std::make_shared<NumberedLines>(50)));
    EXPECT_EQ(highlighter.parsed_version(), 3u);
    EXPECT_EQ(scope_of(highlighter, 40), "line40");
}

TEST(LiveHighlighter, AResultForAVersionThatHasNotComeYetIsDropped) {
    LiveHighlighter highlighter("source.test", 1);
    EXPECT_FALSE(highlighter.deliver(2, std::make_shared<NumberedLines>(5)));
    EXPECT_TRUE(highlighter.spans(0).empty());
}

TEST(LiveHighlighter, LinesBelowAnInsertedLineKeepTheirColors) {
    LiveHighlighter highlighter("source.test", 1);
    ASSERT_TRUE(highlighter.deliver(1, std::make_shared<NumberedLines>(10)));
    // A line break typed at the start of line 4: line 4 became lines 4 and 5.
    highlighter.edited(version_with(2, {replacing(4, 4, "\n")}));
    EXPECT_EQ(scope_of(highlighter, 3), "line3");
    EXPECT_EQ(scope_of(highlighter, 4), "line4");
    EXPECT_EQ(scope_of(highlighter, 5), "line4");
    EXPECT_EQ(scope_of(highlighter, 6), "line5");
    EXPECT_EQ(scope_of(highlighter, 10), "line9");
}

TEST(LiveHighlighter, LinesBelowARemovedLineKeepTheirColors) {
    LiveHighlighter highlighter("source.test", 1);
    ASSERT_TRUE(highlighter.deliver(1, std::make_shared<NumberedLines>(10)));
    // Lines 2 to 4 joined into one.
    highlighter.edited(version_with(2, {replacing(2, 4, "")}));
    EXPECT_EQ(scope_of(highlighter, 1), "line1");
    EXPECT_EQ(scope_of(highlighter, 2), "line2");
    EXPECT_EQ(scope_of(highlighter, 3), "line5");
    EXPECT_EQ(scope_of(highlighter, 7), "line9");
}

TEST(LiveHighlighter, AnEditedLineKeepsTheColorsItHad) {
    LiveHighlighter highlighter("source.test", 1);
    ASSERT_TRUE(highlighter.deliver(1, std::make_shared<NumberedLines>(5)));
    highlighter.edited(version_with(2, {replacing(2, 2, "a")}));
    EXPECT_EQ(scope_of(highlighter, 2), "line2");
    EXPECT_EQ(scope_of(highlighter, 3), "line3");
}

TEST(LiveHighlighter, SeveralEditsAreFollowedInTheOrderTheyWereMade) {
    LiveHighlighter highlighter("source.test", 1);
    ASSERT_TRUE(highlighter.deliver(1, std::make_shared<NumberedLines>(10)));
    highlighter.edited(version_with(2, {replacing(1, 1, "\n\n")}));
    highlighter.edited(version_with(3, {replacing(0, 0, "\n")}));
    // Line 1 became lines 1-3, then a line was added above: old line 5 is now line 8.
    EXPECT_EQ(scope_of(highlighter, 0), "line0");
    EXPECT_EQ(scope_of(highlighter, 1), "line0");
    EXPECT_EQ(scope_of(highlighter, 2), "line1");
    EXPECT_EQ(scope_of(highlighter, 8), "line5");
}

TEST(LiveHighlighter, TheVersionChangesWithEveryEditAndResult) {
    LiveHighlighter highlighter("source.test", 1);
    std::map<std::uint64_t, int> seen;
    seen[highlighter.version()]++;
    highlighter.edited(version_with(2, {replacing(0, 0, "a")}));
    seen[highlighter.version()]++;
    ASSERT_TRUE(highlighter.deliver(2, std::make_shared<NumberedLines>(1)));
    seen[highlighter.version()]++;
    highlighter.edited(version_with(3, {replacing(0, 0, "b")}));
    seen[highlighter.version()]++;
    EXPECT_EQ(seen.size(), 4u);
}

TEST(LiveHighlighter, ForgetsTheOlderResultWhenTooManyEditsPileUp) {
    LiveHighlighter highlighter("source.test", 1);
    ASSERT_TRUE(highlighter.deliver(1, std::make_shared<NumberedLines>(5)));
    std::uint64_t version = 1;
    for (std::size_t i = 0; i <= LiveHighlighter::max_pending_edits; ++i) {
        highlighter.edited(version_with(++version, {replacing(0, 0, "a")}));
    }
    EXPECT_TRUE(highlighter.spans(2).empty());
    ASSERT_TRUE(highlighter.deliver(version, std::make_shared<NumberedLines>(5)));
    EXPECT_EQ(scope_of(highlighter, 2), "line2");
}

TEST(LiveHighlighter, AnEditWithNoChangesStillMovesTheVersion) {
    LiveHighlighter highlighter("source.test", 1);
    highlighter.edited(version_with(2));
    EXPECT_TRUE(highlighter.deliver(2, std::make_shared<NumberedLines>(2)));
}
