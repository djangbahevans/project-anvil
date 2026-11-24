#include "anvil/core/brackets.hpp"

#include <algorithm>
#include <gtest/gtest.h>
#include <random>
#include <string>

using namespace anvil::core;

namespace {

// A transcription of the recursive descent of VS Code's bracket pair parser (parser.ts): a list
// ends at a closing bracket of any kind that is open, an open bracket takes the closing bracket
// that follows its list only when it is of its kind, and a closing bracket that no list is waiting
// for is skipped.
class ReferenceParser {
public:
    explicit ReferenceParser(std::string_view text) : text_(text) {}

    std::vector<BracketPair> parse() {
        parse_list(0);
        std::ranges::sort(pairs_, {}, &BracketPair::open);
        return pairs_;
    }

private:
    static int opener(char c) {
        const std::string_view all = "([{";
        const auto at = all.find(c);
        return at == std::string_view::npos ? -1 : static_cast<int>(at);
    }
    static int closer(char c) {
        const std::string_view all = ")]}";
        const auto at = all.find(c);
        return at == std::string_view::npos ? -1 : static_cast<int>(at);
    }

    void parse_list(unsigned opened) {
        while (position_ < text_.size()) {
            const int closing = closer(text_[position_]);
            if (closing >= 0 && (opened >> closing & 1U)) {
                return;
            }
            parse_child(opened);
        }
    }

    void parse_child(unsigned opened) {
        const char c = text_[position_++];
        const int kind = opener(c);
        if (kind < 0) {
            return;
        }
        const std::size_t open = position_ - 1;
        parse_list(opened | (1U << kind));
        if (position_ < text_.size() && closer(text_[position_]) == kind) {
            pairs_.push_back({open, position_});
            ++position_;
        }
    }

    std::string_view text_;
    std::size_t position_ = 0;
    std::vector<BracketPair> pairs_;
};

bool contains(std::size_t bracket, std::size_t position) {
    return bracket <= position && position <= bracket + 1;
}

// VS Code's Go to Bracket on the pairs of the reference parser: matchBracket, then
// findEnclosingBrackets, then findNextBracket.
std::size_t reference_jump(std::string_view text, std::size_t position) {
    const std::vector<BracketPair> pairs = ReferenceParser(text).parse();

    std::optional<BracketPair> matched;
    std::size_t matched_start = 0;
    for (const BracketPair& pair : pairs) {
        if (!contains(pair.open, position) && !contains(pair.close, position)) {
            continue;
        }
        const std::size_t start = contains(pair.open, position) ? pair.open : pair.close;
        if (!matched || start >= matched_start) {
            matched = pair;
            matched_start = start;
        }
    }
    if (matched) {
        if (contains(matched->open, position) && !contains(matched->close, position)) {
            return matched->close;
        }
        return matched->open;
    }

    std::optional<BracketPair> enclosing;
    for (const BracketPair& pair : pairs) {
        if (pair.open < position && position < pair.close + 1) {
            enclosing = pair;
        }
    }
    if (enclosing) {
        return enclosing->close;
    }
    for (std::size_t i = position; i < text.size(); ++i) {
        if (std::string_view("()[]{}").find(text[i]) != std::string_view::npos) {
            return i;
        }
    }
    return position;
}

} // namespace

TEST(Brackets, MatchesNestedAndSiblingPairsInTheOrderOfTheirOpeningBrackets) {
    const auto pairs = bracket_pairs("a(b[c]{d})(e)");
    EXPECT_EQ(pairs, (std::vector<BracketPair>{{1, 9}, {3, 5}, {6, 8}, {10, 12}}));
}

TEST(Brackets, AnOpenBracketThatIsNeverClosedAndAStrayClosingBracketAreLeftOut) {
    EXPECT_EQ(bracket_pairs("(a"), std::vector<BracketPair>{});
    EXPECT_EQ(bracket_pairs("a)"), std::vector<BracketPair>{});
    EXPECT_EQ(bracket_pairs("(a))"), (std::vector<BracketPair>{{0, 2}}));
}

TEST(Brackets, AClosingBracketClosesTheNearestOpenBracketOfItsKindAndLeavesTheOnesInsideOpen) {
    EXPECT_EQ(bracket_pairs("([)]"), (std::vector<BracketPair>{{0, 2}}));
    EXPECT_EQ(bracket_pairs("{(]}"), (std::vector<BracketPair>{{0, 3}}));
    EXPECT_EQ(bracket_pairs("([{)}"), (std::vector<BracketPair>{{0, 3}}));
}

TEST(Brackets, TheCaretBeforeOrAfterABracketJumpsToItsPartner) {
    const std::string text = "f(a, [b])";
    EXPECT_EQ(jump_to_bracket(text, 1), 8U);
    EXPECT_EQ(jump_to_bracket(text, 2), 8U);
    EXPECT_EQ(jump_to_bracket(text, 8), 1U);
    EXPECT_EQ(jump_to_bracket(text, 9), 1U);
    EXPECT_EQ(jump_to_bracket(text, 5), 7U);
    EXPECT_EQ(jump_to_bracket(text, 6), 7U);
    EXPECT_EQ(jump_to_bracket(text, 7), 5U);
}

TEST(Brackets, BetweenTwoBracketsTheLaterOneWins) {
    EXPECT_EQ(jump_to_bracket("(a)(b)", 3), 5U);
    EXPECT_EQ(jump_to_bracket("(a)(b)", 6), 3U);
}

TEST(Brackets, BetweenTheBracketsOfAnEmptyPairTheCaretGoesToTheOpeningBracket) {
    EXPECT_EQ(jump_to_bracket("a()b", 2), 1U);
}

TEST(Brackets, WithNoBracketAtTheCaretItGoesToTheClosingBracketOfTheInnermostPairAround) {
    const std::string text = "{ a (b c) d }";
    EXPECT_EQ(jump_to_bracket(text, 7), 8U);
    EXPECT_EQ(jump_to_bracket(text, 3), 12U);
    EXPECT_EQ(jump_to_bracket(text, 11), 12U);
}

TEST(Brackets, WithNoPairAroundItGoesToTheNextBracketOfAnyKind) {
    EXPECT_EQ(jump_to_bracket("ab (c)", 0), 3U);
    EXPECT_EQ(jump_to_bracket("ab )c(", 0), 3U);
    EXPECT_EQ(jump_to_bracket("ab (c", 0), 3U);
}

TEST(Brackets, WithNoBracketAfterTheCaretItStaysWhereItIs) {
    EXPECT_EQ(jump_to_bracket("no brackets", 4), 4U);
    EXPECT_EQ(jump_to_bracket("(a) tail", 6), 6U);
    EXPECT_EQ(jump_to_bracket("", 0), 0U);
}

TEST(Brackets, PairsSpanLines) {
    const std::string text = "if (x) {\n  y;\n}\n";
    EXPECT_EQ(jump_to_bracket(text, 7), 14U);
    EXPECT_EQ(jump_to_bracket(text, 14), 7U);
    EXPECT_EQ(jump_to_bracket(text, 10), 14U);
}

TEST(BracketsModel, TheMatchingAndTheJumpAgreeWithTheTranscriptionOfVsCodesParser) {
    std::mt19937 random(11);
    const std::string alphabet = "([{)]}ab \n";
    for (int round = 0; round < 3000; ++round) {
        std::string text;
        const std::size_t length = random() % 16;
        for (std::size_t i = 0; i < length; ++i) {
            text.push_back(alphabet[random() % alphabet.size()]);
        }
        ASSERT_EQ(bracket_pairs(text), ReferenceParser(text).parse()) << text;
        for (std::size_t position = 0; position <= text.size(); ++position) {
            ASSERT_EQ(jump_to_bracket(text, position), reference_jump(text, position))
                << text << " at " << position;
        }
    }
}
