#include "anvil/core/search.hpp"

#include <atomic>
#include <gtest/gtest.h>
#include <random>
#include <regex>
#include <thread>

using namespace anvil::core;

namespace {

Text make_text(std::string_view utf8) {
    return *Text::from_utf8(utf8);
}

Search make_search(Query query, std::size_t window_bytes = Search::default_window_bytes) {
    auto search = Search::create(std::move(query), window_bytes);
    EXPECT_TRUE(search.has_value()) << (search ? "" : search.error().message);
    return *search;
}

std::vector<Match> all_matches(const Search& search, const Text& text) {
    auto list = search.find_all(text, 0, text.size(), 1'000'000);
    EXPECT_TRUE(list.has_value());
    EXPECT_FALSE(list->truncated);
    return list->matches;
}

std::vector<std::string> texts_of(const Text& text, const std::vector<Match>& matches) {
    std::vector<std::string> result;
    for (const Match& match : matches) {
        result.push_back(text.slice(match.begin, match.end - match.begin));
    }
    return result;
}

std::size_t char_length(unsigned char lead) {
    return lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
}

// The obviously correct reference for literal search over bytes.
struct Reference {
    std::string text;
    std::string pattern;
    bool whole_word = false;

    static bool is_word(unsigned char c) {
        return c >= 0x80 || c == '_' || std::isalnum(c) != 0;
    }

    bool valid(std::size_t begin, std::size_t end) const {
        const bool left = begin == 0 || !is_word(static_cast<unsigned char>(text[begin - 1])) ||
                          !is_word(static_cast<unsigned char>(text[begin]));
        const bool right = end == text.size() || !is_word(static_cast<unsigned char>(text[end])) ||
                           !is_word(static_cast<unsigned char>(text[end - 1]));
        return left && right;
    }

    // The candidate search that continues one character after a rejected candidate.
    std::optional<Match> first_from(std::size_t from) const {
        std::size_t position = from;
        while (true) {
            const std::size_t found = text.find(pattern, position);
            if (found == std::string::npos) {
                return std::nullopt;
            }
            if (!whole_word || valid(found, found + pattern.size())) {
                return Match{found, found + pattern.size()};
            }
            position = found + char_length(static_cast<unsigned char>(text[found]));
        }
    }

    std::vector<Match> all() const {
        std::vector<Match> result;
        std::size_t position = 0;
        while (const auto match = first_from(position)) {
            result.push_back(*match);
            position = match->end;
        }
        return result;
    }
};

std::string random_text(std::mt19937& random, std::size_t bytes) {
    static const char* const pieces[] = {"a", "b", "a", "b", "_", " ", "\n", "é", "€", "😀", "ab"};
    std::string text;
    while (text.size() < bytes) {
        text += pieces[random() % std::size(pieces)];
    }
    return text;
}

// A random run of whole characters of `text`.
std::string random_substring(std::mt19937& random, const std::string& text, std::size_t max_chars) {
    std::size_t begin = random() % text.size();
    while (begin > 0 && (static_cast<unsigned char>(text[begin]) & 0xC0u) == 0x80u) {
        --begin;
    }
    std::size_t end = begin;
    const std::size_t chars = 1 + random() % max_chars;
    for (std::size_t i = 0; i < chars && end < text.size(); ++i) {
        end += char_length(static_cast<unsigned char>(text[end]));
    }
    return text.substr(begin, end - begin);
}

} // namespace

TEST(SearchModelTest, LiteralMatchesAgreeWithTheReference) {
    std::mt19937 random(7);
    for (const std::size_t window : {1u, 3u, 17u, 500u, 2048u, 65536u}) {
        for (int round = 0; round < 25; ++round) {
            const std::string raw = random_text(random, 5000 + random() % 4000);
            const Text text = make_text(raw);
            ASSERT_GT(text.chunk_count(), 2u);
            const bool whole_word = random() % 3 == 0;
            Reference reference{raw, random_substring(random, raw, 6), whole_word};
            const Search search = make_search(
                {.pattern = reference.pattern, .match_case = true, .whole_word = whole_word},
                window);

            ASSERT_EQ(all_matches(search, text), reference.all())
                << "window " << window << " pattern '" << reference.pattern << "'";

            for (int probe = 0; probe < 10; ++probe) {
                const std::size_t from = random() % (raw.size() + 1);
                const auto next = search.find_next(text, from);
                ASSERT_TRUE(next.has_value());
                const auto expected = reference.first_from(from);
                ASSERT_EQ(next->has_value(), expected.has_value());
                if (expected) {
                    EXPECT_EQ((*next)->match, *expected);
                    EXPECT_FALSE((*next)->wrapped);
                }
            }
        }
    }
}

TEST(SearchModelTest, PreviousMatchIsTheLastOneBeforeThePosition) {
    std::mt19937 random(11);
    for (const std::size_t window : {1u, 40u, 3000u, 65536u}) {
        for (int round = 0; round < 20; ++round) {
            const std::string raw = random_text(random, 4000 + random() % 3000);
            const Text text = make_text(raw);
            Reference reference{raw, random_substring(random, raw, 5)};
            // A pattern that contains a line break can overlap itself across lines, where the
            // alignment of the matches depends on where scanning started.
            if (reference.pattern.find('\n') != std::string::npos) {
                continue;
            }
            const Search search =
                make_search({.pattern = reference.pattern, .match_case = true}, window);
            const std::vector<Match> matches = reference.all();
            for (int probe = 0; probe < 10; ++probe) {
                const std::size_t before = random() % (raw.size() + 2);
                std::optional<Match> expected;
                for (const Match& match : matches) {
                    if (match.begin < before) {
                        expected = match;
                    }
                }
                const auto previous = search.find_previous(text, before);
                ASSERT_TRUE(previous.has_value());
                ASSERT_EQ(previous->has_value(), expected.has_value())
                    << "before " << before << " window " << window << " pattern '"
                    << reference.pattern << "'";
                if (expected) {
                    EXPECT_EQ((*previous)->match, *expected);
                }
            }
        }
    }
}

TEST(SearchModelTest, MatchesAcrossEveryChunkBoundaryAreFound) {
    const std::string pattern = "needle€needle";
    for (std::size_t window : {1u, 64u, 65536u}) {
        const Search search = make_search({.pattern = pattern, .match_case = true}, window);
        for (std::size_t start = 1960; start < 2100; ++start) {
            const std::string raw =
                std::string(start, 'x') + pattern + std::string(4100 - start - pattern.size(), 'y');
            const Text text = make_text(raw);
            ASSERT_GT(text.chunk_count(), 1u);
            const auto matches = all_matches(search, text);
            ASSERT_EQ(matches.size(), 1u) << "start " << start << " window " << window;
            EXPECT_EQ(matches[0], (Match{start, start + pattern.size()}));
        }
    }
}

TEST(SearchModelTest, ASCIICaseInsensitiveMatchesAgreeWithLowercasedReference) {
    std::mt19937 random(5);
    for (int round = 0; round < 40; ++round) {
        std::string raw = random_text(random, 4500);
        for (std::size_t i = 0; i < raw.size(); ++i) {
            if (random() % 3 == 0 && raw[i] >= 'a' && raw[i] <= 'z') {
                raw[i] = static_cast<char>(raw[i] - 'a' + 'A');
            }
        }
        std::string lowered = raw;
        for (char& c : lowered) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        std::string pattern = random_substring(random, raw, 5);
        std::string lowered_pattern = pattern;
        for (char& c : lowered_pattern) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        const Text text = make_text(raw);
        const Search search = make_search({.pattern = pattern}, 100);
        EXPECT_EQ(all_matches(search, text), (Reference{lowered, lowered_pattern}.all()));
    }
}

TEST(SearchModelTest, RegexMatchesAgreeWithStdRegexPerLine) {
    std::mt19937 random(3);
    const std::vector<std::string> patterns = {"a+", "ab|b", "[ab]c",   "a.b",
                                               "^a", "b$",   "\\bab\\b"};
    for (const std::string& pattern : patterns) {
        std::string raw;
        while (raw.size() < 6000) {
            static const char* const pieces[] = {"a", "b", "c", " ", "\n", "ab"};
            raw += pieces[random() % std::size(pieces)];
        }
        const Text text = make_text(raw);
        std::vector<Match> expected;
        const std::regex regex(pattern);
        std::size_t line_begin = 0;
        while (line_begin <= raw.size()) {
            std::size_t line_end = raw.find('\n', line_begin);
            if (line_end == std::string::npos) {
                line_end = raw.size();
            }
            const std::string line = raw.substr(line_begin, line_end - line_begin);
            for (std::sregex_iterator it(line.begin(), line.end(), regex), done; it != done; ++it) {
                expected.push_back({line_begin + static_cast<std::size_t>(it->position()),
                                    line_begin + static_cast<std::size_t>(it->position()) +
                                        static_cast<std::size_t>(it->length())});
            }
            line_begin = line_end + 1;
        }
        for (const std::size_t window : {1u, 70u, 65536u}) {
            const Search search =
                make_search({.pattern = pattern, .match_case = true, .regex = true}, window);
            EXPECT_EQ(all_matches(search, text), expected)
                << "pattern " << pattern << " window " << window;
        }
    }
}

TEST(SearchTest, AnEmptyPatternMatchesNothing) {
    const Text text = make_text("abc");
    for (const bool regex : {false, true}) {
        const Search search = make_search({.regex = regex});
        EXPECT_TRUE(all_matches(search, text).empty());
        EXPECT_FALSE(search.find_next(text, 0, true)->has_value());
        EXPECT_FALSE(search.find_previous(text, 3, true)->has_value());
    }
}

TEST(SearchTest, MatchesDoNotOverlap) {
    const Search search = make_search({.pattern = "aa", .match_case = true});
    EXPECT_EQ(all_matches(search, make_text("aaa")), (std::vector<Match>{{0, 2}}));
    EXPECT_EQ(all_matches(search, make_text("aaaa")), (std::vector<Match>{{0, 2}, {2, 4}}));
}

TEST(SearchTest, AMatchAtTheVeryEndIsFound) {
    const Text text = make_text("xx\nend");
    const Search search = make_search({.pattern = "end", .match_case = true});
    EXPECT_EQ(search.find_next(text, 0)->value().match, (Match{3, 6}));
    EXPECT_EQ(search.find_next(text, 3)->value().match, (Match{3, 6}));
    EXPECT_FALSE(search.find_next(text, 4)->has_value());
    EXPECT_FALSE(search.find_next(text, 100)->has_value());
    EXPECT_EQ(search.find_previous(text, 100)->value().match, (Match{3, 6}));
}

TEST(SearchTest, OffsetsAreBytesOfMultiByteText) {
    const Text text = make_text("é€😀 é€😀");
    const Search search = make_search({.pattern = "€😀", .match_case = true});
    EXPECT_EQ(all_matches(search, text), (std::vector<Match>{{2, 9}, {12, 19}}));
    EXPECT_EQ(text.position_at(12), (Position{0, 6}));
}

TEST(SearchTest, WrappingNextAndPreviousReportTheWrap) {
    const Text text = make_text("a b a b");
    const Search search = make_search({.pattern = "b", .match_case = true});
    EXPECT_FALSE(search.find_next(text, 7)->has_value());

    const auto next = search.find_next(text, 7, true);
    EXPECT_EQ(next->value().match, (Match{2, 3}));
    EXPECT_TRUE(next->value().wrapped);

    const auto previous = search.find_previous(text, 2, true);
    EXPECT_EQ(previous->value().match, (Match{6, 7}));
    EXPECT_TRUE(previous->value().wrapped);

    EXPECT_FALSE(search.find_next(text, 3, true)->value().wrapped);
    EXPECT_FALSE(search.find_previous(text, 7, true)->value().wrapped);
}

TEST(SearchTest, WrappingFindsTheOnlyMatchWhenTheSearchStartsAfterIt) {
    const Text text = make_text("a b a");
    const Search search = make_search({.pattern = "b", .match_case = true});
    const auto next = search.find_next(text, 4, true);
    ASSERT_TRUE(next->has_value());
    EXPECT_TRUE(next->value().wrapped);
    EXPECT_EQ(next->value().match, (Match{2, 3}));
}

TEST(SearchTest, CaseInsensitiveSearchFoldsUnicode) {
    const Text text = make_text("Éé ÉÉ straße STRASSE");
    const Search folded = make_search({.pattern = "éé"});
    EXPECT_EQ(texts_of(text, all_matches(folded, text)), (std::vector<std::string>{"Éé", "ÉÉ"}));
    const Search exact = make_search({.pattern = "éé", .match_case = true});
    EXPECT_TRUE(all_matches(exact, text).empty());
    // Simple case folding does not turn one character into two.
    const Search sharp = make_search({.pattern = "STRASSE"});
    EXPECT_EQ(all_matches(sharp, text).size(), 1u);
}

TEST(SearchTest, CaseInsensitiveMatchesCanBeLongerThanThePattern) {
    // The Kelvin sign is three bytes and folds to "k".
    std::string raw(3000, 'x');
    raw += "\xE2\x84\xAA\xE2\x84\xAA";
    raw += std::string(3000, 'x');
    const Text text = make_text(raw);
    for (const std::size_t window : {1u, 2999u, 65536u}) {
        const Search search = make_search({.pattern = "kk"}, window);
        EXPECT_EQ(all_matches(search, text), (std::vector<Match>{{3000, 3006}}))
            << "window " << window;
    }
}

TEST(SearchTest, WholeWordUsesTheClassesOfWordMotion) {
    // Rejected: the word inside "concat", "cat_" and "cats", and the ones next to the non-ASCII
    // letters in "écat" and "catè", which count as word characters.
    const std::string raw = "cat concat cat_ cats cat. écat catè (cat) cat";
    const Text text = make_text(raw);
    const Search search = make_search({.pattern = "cat", .match_case = true, .whole_word = true});
    std::vector<std::size_t> found;
    for (const Match& match : all_matches(search, text)) {
        found.push_back(match.begin);
    }
    EXPECT_EQ(found, (std::vector<std::size_t>{0, raw.find("cat."), raw.find("(cat)") + 1,
                                               raw.rfind("cat")}));
}

TEST(SearchTest, WholeWordIsNotCheckedOnASideThatStartsWithPunctuation) {
    const Text text = make_text("a.b .b a.b.c");
    const Search search = make_search({.pattern = ".b", .match_case = true, .whole_word = true});
    EXPECT_EQ(all_matches(search, text), (std::vector<Match>{{1, 3}, {4, 6}, {8, 10}}));
}

TEST(SearchTest, WholeWordRegexRejectsInnerMatches) {
    const Text text = make_text("ab abc bab ab");
    const Search search =
        make_search({.pattern = "ab", .match_case = true, .whole_word = true, .regex = true});
    EXPECT_EQ(all_matches(search, text), (std::vector<Match>{{0, 2}, {11, 13}}));
}

TEST(SearchRegexTest, SupportsLookAroundAndBackReferences) {
    const Text text = make_text("foobar foobaz xx yy abab");
    const Search lookahead =
        make_search({.pattern = "foo(?=baz)", .match_case = true, .regex = true});
    EXPECT_EQ(all_matches(lookahead, text), (std::vector<Match>{{7, 10}}));
    const Search backreference =
        make_search({.pattern = "(ab)\\1", .match_case = true, .regex = true});
    EXPECT_EQ(all_matches(backreference, text), (std::vector<Match>{{20, 24}}));
    const Search lookbehind =
        make_search({.pattern = "(?<=foo)ba.", .match_case = true, .regex = true});
    EXPECT_EQ(all_matches(lookbehind, text), (std::vector<Match>{{3, 6}, {10, 13}}));
}

TEST(SearchRegexTest, CaseOptionAppliesToPatterns) {
    const Text text = make_text("Hello hello HELLO");
    EXPECT_EQ(all_matches(make_search({.pattern = "h\\w+", .regex = true}), text).size(), 3u);
    EXPECT_EQ(
        all_matches(make_search({.pattern = "h\\w+", .match_case = true, .regex = true}), text)
            .size(),
        1u);
}

TEST(SearchRegexTest, AnchorsMatchAtTheEndsOfLines) {
    const Text text = make_text("ab\nab\n\nab");
    EXPECT_EQ(
        all_matches(make_search({.pattern = "^ab$", .match_case = true, .regex = true}), text),
        (std::vector<Match>{{0, 2}, {3, 5}, {7, 9}}));
}

TEST(SearchRegexTest, MatchesNeverContainALineBreak) {
    const Text text = make_text("a\nb");
    EXPECT_TRUE(
        all_matches(make_search({.pattern = "a\\nb", .match_case = true, .regex = true}), text)
            .empty());
    EXPECT_EQ(all_matches(make_search({.pattern = "a.*", .match_case = true, .regex = true}), text),
              (std::vector<Match>{{0, 1}}));
}

TEST(SearchRegexTest, EmptyMatchesAreReportedOncePerPosition) {
    const Text text = make_text("a\nbé");
    const Search search = make_search({.pattern = "^", .regex = true});
    EXPECT_EQ(all_matches(search, text), (std::vector<Match>{{0, 0}, {2, 2}}));
    const Search star = make_search({.pattern = "x*", .match_case = true, .regex = true});
    EXPECT_EQ(all_matches(star, make_text("é")), (std::vector<Match>{{0, 0}, {2, 2}}));
}

TEST(SearchRegexTest, AnInvalidPatternIsReportedAtCreation) {
    const auto search = Search::create({.pattern = "(ab", .regex = true});
    ASSERT_FALSE(search.has_value());
    EXPECT_EQ(search.error().code, anvil::ErrorCode::invalid_argument);
    EXPECT_NE(search.error().message.find("offset"), std::string::npos);
    for (const char* pattern : {"[a-", "a\\Cb", "*a"}) {
        const auto invalid = Search::create({.pattern = pattern, .regex = true});
        ASSERT_FALSE(invalid.has_value()) << pattern;
        EXPECT_EQ(invalid.error().code, anvil::ErrorCode::invalid_argument) << pattern;
    }
    // The same text is fine as a literal.
    EXPECT_TRUE(Search::create({.pattern = "(ab"}).has_value());
}

TEST(SearchRegexTest, RunawayBacktrackingFailsInsteadOfHanging) {
    const Text text = make_text(std::string(40, 'a') + "!");
    const Search search = make_search({.pattern = "(a+)+$", .match_case = true, .regex = true});
    const auto result = search.find_all(text, 0, text.size(), 10);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, anvil::ErrorCode::limit_exceeded);
    EXPECT_NE(result.error().message.find("too complex"), std::string::npos);
}

TEST(SearchRegexTest, OverflowingTheEngineStackIsALimitFailureToo) {
    std::string raw;
    for (int i = 0; i < 400000; ++i) {
        raw += "ab";
    }
    const Text text = make_text(raw);
    const Search search = make_search({.pattern = "(?:a|b)*c", .match_case = true, .regex = true});
    const auto result = search.find_all(text, 0, text.size(), 10);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, anvil::ErrorCode::limit_exceeded);
    EXPECT_NE(result.error().message.find("too complex"), std::string::npos);
}

TEST(SearchRegexTest, EveryOperationReportsTheEngineLimitWithTheSameCode) {
    const Text text = make_text(std::string(40, 'a') + "!");
    const Search search = make_search({.pattern = "(a+)+$", .match_case = true, .regex = true});
    const auto code = [](const auto& result) {
        return result.has_value() ? std::nullopt : std::optional(result.error().code);
    };
    const std::optional limit = anvil::ErrorCode::limit_exceeded;
    EXPECT_EQ(code(search.find_next(text, 0)), limit);
    EXPECT_EQ(code(search.find_next(text, 0, true)), limit);
    EXPECT_EQ(code(search.find_previous(text, text.size())), limit);
    EXPECT_EQ(code(search.find_all(text, 0, text.size(), 10)), limit);
    EXPECT_EQ(code(search.replacement_for(text, {0, 40}, "x")), limit);
    EXPECT_EQ(code(search.replace_all(text, "x")), limit);
}

TEST(SearchTest, ALimitTruncatesTheList) {
    const Text text = make_text("a a a a a");
    const Search search = make_search({.pattern = "a", .match_case = true});
    auto three = search.find_all(text, 0, text.size(), 3);
    EXPECT_EQ(three->matches.size(), 3u);
    EXPECT_TRUE(three->truncated);
    auto five = search.find_all(text, 0, text.size(), 5);
    EXPECT_EQ(five->matches.size(), 5u);
    EXPECT_FALSE(five->truncated);
}

TEST(SearchTest, FindAllStaysInsideTheRange) {
    const Text text = make_text("ab ab ab ab");
    const Search search = make_search({.pattern = "ab", .match_case = true});
    EXPECT_EQ(search.find_all(text, 3, 8, 10)->matches, (std::vector<Match>{{3, 5}, {6, 8}}));
    EXPECT_EQ(search.find_all(text, 4, 7, 10)->matches, (std::vector<Match>{}));
    EXPECT_EQ(search.find_all(text, 3, 100, 10)->matches.size(), 3u);
    EXPECT_TRUE(search.find_all(text, 50, 100, 10)->matches.empty());
}

TEST(SearchTest, AStoppedSearchReturnsWhatItFound) {
    const Text text = make_text(std::string(100000, 'a'));
    const Search search = make_search({.pattern = "a", .match_case = true}, 1024);
    std::stop_source source;
    source.request_stop();
    const auto result = search.find_all(text, 0, text.size(), 1'000'000, source.get_token());
    ASSERT_TRUE(result.has_value());
    EXPECT_LT(result->matches.size(), 100000u);
}

TEST(SearchTest, ACompiledSearchCanBeUsedFromSeveralThreads) {
    std::mt19937 random(9);
    const std::string raw = random_text(random, 20000);
    const Text text = make_text(raw);
    const Search search = make_search({.pattern = "ab", .match_case = true});
    const std::vector<Match> expected = Reference{raw, "ab"}.all();
    std::vector<std::thread> threads;
    std::atomic<int> failures = 0;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&] {
            for (int round = 0; round < 20; ++round) {
                if (all_matches(search, text) != expected) {
                    ++failures;
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(failures, 0);
}

TEST(SearchTest, ALongSingleLineIsSearchedWithoutCuttingMatches) {
    const std::string raw = std::string(300000, 'x') + "y" + std::string(300000, 'x');
    const Text text = make_text(raw);
    const Search literal = make_search({.pattern = "xyx", .match_case = true});
    EXPECT_EQ(all_matches(literal, text), (std::vector<Match>{{299999, 300002}}));
    const Search regex = make_search({.pattern = "x+y", .match_case = true, .regex = true}, 4096);
    EXPECT_EQ(all_matches(regex, text), (std::vector<Match>{{0, 300001}}));
}

TEST(SearchReplaceTest, PlainReplacementsAreUsedAsWritten) {
    const Text text = make_text("a-a");
    const Search search = make_search({.pattern = "a", .match_case = true});
    const auto edits = search.replace_all(text, "$1\\n$&");
    ASSERT_TRUE(edits.has_value());
    ASSERT_EQ(edits->size(), 2u);
    EXPECT_EQ((*edits)[0].text, "$1\\n$&");
    EXPECT_EQ(*search.replacement_for(text, {2, 3}, "$1"), "$1");
}

TEST(SearchReplaceTest, RegexReplacementsExpandGroups) {
    const Text text = make_text("2024-05-17");
    const Search search =
        make_search({.pattern = "(\\d+)-(\\d+)-(\\d+)", .match_case = true, .regex = true});
    const Match whole{0, 10};
    EXPECT_EQ(*search.replacement_for(text, whole, "$3/$2/$1"), "17/05/2024");
    EXPECT_EQ(*search.replacement_for(text, whole, "[$&|$0]"), "[2024-05-17|2024-05-17]");
    EXPECT_EQ(*search.replacement_for(text, whole, "$$1 \\\\ \\t|\\n|"), "$1 \\ \t|\n|");
    // A group the pattern lacks is left as written, and a second digit that does not make a
    // group is text.
    EXPECT_EQ(*search.replacement_for(text, whole, "$4 $10 $31"), "$4 20240 171");
    EXPECT_EQ(*search.replacement_for(text, whole, "\\q$"), "\\q$");
}

TEST(SearchReplaceTest, AGroupThatDidNotTakePartExpandsToNothing) {
    const Text text = make_text("b");
    const Search search = make_search({.pattern = "(a)?b(c)?", .match_case = true, .regex = true});
    EXPECT_EQ(*search.replacement_for(text, {0, 1}, "<$1|$2>"), "<|>");
}

TEST(SearchReplaceTest, ReplacementForRejectsRangesThatAreNotMatches) {
    const Text text = make_text("foo bar foo");
    const Search search = make_search({.pattern = "foo", .match_case = true});
    EXPECT_TRUE(search.replacement_for(text, {0, 3}, "x")->has_value());
    EXPECT_FALSE(search.replacement_for(text, {0, 2}, "x")->has_value());
    EXPECT_FALSE(search.replacement_for(text, {1, 4}, "x")->has_value());
    EXPECT_FALSE(search.replacement_for(text, {4, 7}, "x")->has_value());
    EXPECT_FALSE(search.replacement_for(text, {50, 53}, "x")->has_value());
}

TEST(SearchReplaceTest, ReplacementForSeesTheTextAfterTheMatch) {
    const Text text = make_text("foobar");
    const Search search = make_search({.pattern = "foo(?=bar)", .match_case = true, .regex = true});
    EXPECT_TRUE(search.replacement_for(text, {0, 3}, "x")->has_value());
}

TEST(SearchReplaceTest, ReplaceAllIsOneUndoStepAndTracksTheDirtyState) {
    Document document(make_text("one two one two one"));
    EXPECT_FALSE(document.is_dirty());
    const Search search = make_search({.pattern = "one", .match_case = true});
    const auto edits = search.replace_all(document.text(), "three");
    ASSERT_TRUE(edits.has_value());
    ASSERT_EQ(edits->size(), 3u);
    ASSERT_TRUE(document.apply_edits(*edits).has_value());
    EXPECT_EQ(document.text().to_string(), "three two three two three");
    EXPECT_TRUE(document.is_dirty());

    ASSERT_TRUE(document.undo().has_value());
    EXPECT_EQ(document.text().to_string(), "one two one two one");
    EXPECT_FALSE(document.is_dirty());
    EXPECT_FALSE(document.can_undo());
}

TEST(SearchReplaceTest, EmptyMatchesBecomeInsertions) {
    Document document(make_text("a\nb"));
    const Search search = make_search({.pattern = "^", .regex = true});
    const auto edits = search.replace_all(document.text(), "> ");
    ASSERT_TRUE(edits.has_value());
    ASSERT_TRUE(document.apply_edits(*edits).has_value());
    EXPECT_EQ(document.text().to_string(), "> a\n> b");
}

TEST(SearchTest, RangeOffsetsInsideACharacterMoveToCharacterBoundaries) {
    // é is bytes 0-1, € is 2-4, é is 5-6.
    const Text text = make_text("é€é");
    for (const bool regex : {false, true}) {
        const Search any =
            make_search({.pattern = regex ? "." : "€", .match_case = true, .regex = regex});
        // An end inside the first character leaves no room for a match.
        EXPECT_TRUE(any.find_all(text, 0, 1, 10)->matches.empty()) << regex;
        // A begin inside the first character starts at the next one, an end inside the last
        // character stops before it.
        EXPECT_EQ(any.find_all(text, 1, 6, 10)->matches, (std::vector<Match>{{2, 5}})) << regex;
    }
    const Search first = make_search({.pattern = "é", .match_case = true});
    EXPECT_EQ(first.find_all(text, 0, 6, 10)->matches, (std::vector<Match>{{0, 2}}));
    EXPECT_EQ(first.find_all(text, 0, 7, 10)->matches, (std::vector<Match>{{0, 2}, {5, 7}}));
}

TEST(SearchReplaceTest, ReplacementForAcceptsAnOverlappedRangeThatScanningWouldSkip) {
    const Text text = make_text("aaa");
    const Search search = make_search({.pattern = "aa", .match_case = true});
    EXPECT_EQ(all_matches(search, text), (std::vector<Match>{{0, 2}}));
    EXPECT_TRUE(search.replacement_for(text, {1, 3}, "x")->has_value());
}
