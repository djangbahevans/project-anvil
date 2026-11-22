#include "anvil/core/fuzzy.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <numeric>
#include <string>
#include <vector>

using namespace anvil::core;
using nlohmann::json;
using Ranges = std::vector<MatchRange>;

namespace {

std::string to_utf8(std::u16string_view text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char32_t c = text[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < text.size()) {
            c = 0x10000 + ((c - 0xD800) << 10) + (text[++i] - 0xDC00);
        }
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (c >> 18)));
            out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

Ranges ranges_of(const json& pairs) {
    Ranges ranges;
    for (const json& pair : pairs) {
        ranges.push_back({pair[0].get<std::size_t>(), pair[1].get<std::size_t>()});
    }
    return ranges;
}

json load_cases() {
    std::ifstream file(std::string(ANVIL_CORE_TEST_DATA_DIR) + "/fuzzy_cases.json");
    return json::parse(file);
}

// A file as the explorer's quick open shows it: the name, the directory and the full path.
struct FileItem {
    std::string label;
    std::string description;
    std::string path;

    explicit FileItem(const std::string& full) : path(full) {
        const std::size_t slash = full.rfind('/');
        label = full.substr(slash + 1);
        description = slash == std::string::npos ? "" : full.substr(0, slash);
    }
    FuzzyItem item() const {
        return {label, description, path};
    }
};

ItemScore score(const FileItem& file, const std::string& query, bool allow = true) {
    return score_item(file.item(), prepare_query(query), allow);
}

// The paths of the items from the best match to the worst.
std::vector<std::string> ranked(const std::vector<FileItem>& files, const std::string& query) {
    const PreparedQuery prepared = prepare_query(query);
    std::vector<std::size_t> order(files.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::vector<ItemScore> scores;
    for (const FileItem& file : files) {
        scores.push_back(score_item(file.item(), prepared));
    }
    std::ranges::stable_sort(order, [&](std::size_t a, std::size_t b) {
        return compare_items(files[a].item(), scores[a], files[b].item(), scores[b], prepared) < 0;
    });
    std::vector<std::string> paths;
    for (const std::size_t i : order) {
        paths.push_back(files[i].path);
    }
    return paths;
}

int fuzzy_score(const std::string& target, const std::string& query, bool allow) {
    return score_fuzzy(target, prepare_query(query), allow).score;
}

} // namespace

// The cases below were recorded from VS Code's own scorer by scripts/generate-fuzzy-cases.mjs.

TEST(FuzzyRecordedTest, ScoresAndPositionsOfStringsMatchVsCode) {
    const json cases = load_cases();
    ASSERT_FALSE(cases["scoreFuzzy"].empty());
    for (const json& c : cases["scoreFuzzy"]) {
        const std::string target = c[0];
        const std::string query = c[1];
        const FuzzyMatch match = score_fuzzy(target, prepare_query(query), c[2].get<bool>());
        EXPECT_EQ(match.score, c[3].get<int>()) << target << " / " << query;
        EXPECT_EQ(match.positions, c[4].get<std::vector<std::size_t>>())
            << target << " / " << query;
    }
}

TEST(FuzzyRecordedTest, ItemScoresAndRangesMatchVsCode) {
    const json cases = load_cases();
    ASSERT_FALSE(cases["scoreItem"].empty());
    for (const json& c : cases["scoreItem"]) {
        const std::string label = c[0];
        const std::string description = c[1];
        const std::string path = c[2];
        const std::string query = c[3];
        const ItemScore result =
            score_item({label, description, path}, prepare_query(query), c[4].get<bool>());
        const std::string name = label + " | " + description + " | " + path + " / " + query;
        EXPECT_EQ(result.score, c[5].get<int>()) << name;
        EXPECT_EQ(result.label_match, ranges_of(c[6])) << name;
        EXPECT_EQ(result.description_match, ranges_of(c[7])) << name;
    }
}

TEST(FuzzyRecordedTest, ThePairwiseOrderOfItemsMatchesVsCode) {
    const json cases = load_cases();
    ASSERT_FALSE(cases["compare"].empty());
    for (const json& group : cases["compare"]) {
        const std::string query = group["query"];
        const PreparedQuery prepared = prepare_query(query);
        std::vector<std::array<std::string, 3>> texts;
        for (const json& item : group["items"]) {
            texts.push_back({item[0], item[1], item[2]});
        }
        std::vector<FuzzyItem> items;
        std::vector<ItemScore> scores;
        for (const auto& text : texts) {
            items.push_back({text[0], text[1], text[2]});
            scores.push_back(score_item(items.back(), prepared));
        }
        std::size_t index = 0;
        for (std::size_t a = 0; a < items.size(); ++a) {
            for (std::size_t b = a + 1; b < items.size(); ++b, ++index) {
                const int result =
                    compare_items(items[a], scores[a], items[b], scores[b], prepared);
                const int sign = (result > 0) - (result < 0);
                EXPECT_EQ(sign, group["signs"][index].get<int>())
                    << "query '" << query << "': '" << texts[a][0] << "' against '" << texts[b][0]
                    << "'";
            }
        }
    }
}

TEST(FuzzyRecordedTest, PreparedQueriesMatchVsCode) {
    const json cases = load_cases();
    ASSERT_FALSE(cases["prepareQuery"].empty());
    for (const json& c : cases["prepareQuery"]) {
        const std::string query = c["query"];
        const PreparedQuery prepared = prepare_query(query);
        EXPECT_EQ(to_utf8(prepared.normalized), c["normalized"].get<std::string>()) << query;
        EXPECT_EQ(to_utf8(prepared.normalized_lowercase),
                  c["normalizedLowercase"].get<std::string>())
            << query;
        EXPECT_EQ(to_utf8(prepared.path_normalized), c["pathNormalized"].get<std::string>())
            << query;
        EXPECT_EQ(prepared.expect_contiguous, c["expectContiguousMatch"].get<bool>()) << query;
        EXPECT_EQ(prepared.contains_path_separator, c["containsPathSeparator"].get<bool>())
            << query;
        if (c["pieces"].is_null()) {
            EXPECT_TRUE(prepared.pieces.empty()) << query;
            continue;
        }
        ASSERT_EQ(prepared.pieces.size(), c["pieces"].size()) << query;
        for (std::size_t i = 0; i < prepared.pieces.size(); ++i) {
            EXPECT_EQ(to_utf8(prepared.pieces[i].normalized),
                      c["pieces"][i]["normalized"].get<std::string>())
                << query;
            EXPECT_EQ(prepared.pieces[i].expect_contiguous,
                      c["pieces"][i]["expectContiguousMatch"].get<bool>())
                << query;
        }
    }
}

// The cases below state the behavior in words; they follow the tests of fuzzyScorer.test.ts.

TEST(FuzzyTest, BetterMatchesOfAWordScoreHigher) {
    const std::string target = "HelLo-World";
    const std::vector<std::string> queries = {
        "HelLo-World", // direct case match
        "hello-world", // direct mixed-case match
        "HW",          // case prefix of several words
        "hw",          // mixed-case prefix of several words
        "H",           // case prefix
        "h",           // mixed-case prefix
        "W",           // case word prefix
        "Ld",          // case match inside the string, in two parts
        "ld",          // consecutive match, avoids the scattered one
        "w",           // mixed-case word prefix
        "L",           // case match inside the string
        "l",           // mixed-case match inside the string
        "4",           // no match
    };
    int previous = fuzzy_score(target, queries.front(), true);
    for (const std::string& query : queries) {
        const int current = fuzzy_score(target, query, true);
        EXPECT_LE(current, previous) << query;
        previous = current;
    }
    EXPECT_EQ(fuzzy_score(target, "4", true), 0);
}

TEST(FuzzyTest, WithoutNonContiguousMatchesTheQueryMustOccurAsAWhole) {
    const std::string target = "HelLo-World";
    EXPECT_GT(fuzzy_score(target, "HelLo-World", false), 0);
    EXPECT_EQ(score_fuzzy(target, prepare_query("HelLo-World"), false).positions.size(),
              std::string("HelLo-World").size());
    EXPECT_GT(fuzzy_score(target, "hello-world", false), 0);
    EXPECT_EQ(fuzzy_score(target, "HW", false), 0);
    EXPECT_GT(fuzzy_score(target, "ello", false), 0);
    EXPECT_EQ(fuzzy_score(target, "eo", false), 0);
}

TEST(FuzzyTest, AQuotedQueryNeedsAnExactOccurrence) {
    const FileItem file("/xyz/some/path/someFile123.txt");
    EXPECT_GT(score(file, "sF").score, 0);
    EXPECT_EQ(score(file, "\"sF\"").score, 0);
    EXPECT_GT(score(file, "\"File\"").score, 0);
}

TEST(FuzzyTest, QueryCharactersMustAppearInTheTargetInOrder) {
    EXPECT_EQ(score(FileItem("etem"), "teem").score, 0);
    EXPECT_EQ(score(FileItem("abcde"), "edcda").score, 0);
    EXPECT_EQ(score(FileItem("ede"), "de").label_match, (Ranges{{1, 3}}));
}

TEST(FuzzyTest, ReportsWhereTheLabelAndTheDescriptionMatched) {
    const FileItem file("/xyz/some/path/someFile123.txt");

    const ItemScore identity = score(file, file.path);
    EXPECT_EQ(identity.score, 1 << 18);
    EXPECT_EQ(identity.label_match, (Ranges{{0, file.label.size()}}));
    EXPECT_EQ(identity.description_match, (Ranges{{0, file.description.size()}}));

    const ItemScore prefix = score(file, "som");
    EXPECT_EQ(prefix.label_match, (Ranges{{0, 3}}));
    EXPECT_TRUE(prefix.description_match.empty());

    EXPECT_EQ(score(file, "sF").label_match, (Ranges{{0, 1}, {4, 5}}));
    EXPECT_EQ(score(file, "of").label_match, (Ranges{{1, 2}, {4, 5}}));

    const ItemScore path = score(file, "xyz123");
    EXPECT_EQ(path.label_match, (Ranges{{8, 11}}));
    EXPECT_EQ(path.description_match, (Ranges{{1, 4}}));

    EXPECT_EQ(score(file, "987").score, 0);
    EXPECT_GT(identity.score, prefix.score);
    EXPECT_GT(prefix.score, score(file, "of").score);
    EXPECT_GT(score(file, "of").score, path.score);
}

TEST(FuzzyTest, AnEllipsisStandsForTheStartOfAPath) {
    const FileItem file("/xyz/some/path/someFile123.txt");
    EXPECT_GT(score(file, "…me/path/someFile123.txt").score, 0);
}

TEST(FuzzyTest, EveryWordOfAQueryMustMatch) {
    const FileItem file("/xyz/some/path/someFile123.txt");

    const ItemScore first = score(file, "xyz some");
    EXPECT_EQ(first.label_match, (Ranges{{0, 4}}));
    EXPECT_EQ(first.description_match, (Ranges{{1, 4}}));
    EXPECT_EQ(score(file, "some xyz").score, first.score);

    EXPECT_GT(score(file, "some xyz file file123").score, first.score);
    EXPECT_EQ(score(file, "some xyz file file123").label_match, (Ranges{{0, 11}}));

    const ItemScore descriptionOnly = score(file, "path z y");
    EXPECT_LT(descriptionOnly.score, first.score);
    EXPECT_TRUE(descriptionOnly.label_match.empty());
    EXPECT_EQ(descriptionOnly.description_match, (Ranges{{2, 4}, {10, 14}}));

    EXPECT_EQ(score(file, "some nomatch").score, 0);
}

TEST(FuzzyTest, PrefersTheMoreRelevantMatchAtTheEndOfAPath) {
    const FileItem file("/xyz/others/spath/some/xsp/file123.txt");
    const ItemScore result = score(file, "xspfile123");
    EXPECT_EQ(result.label_match, (Ranges{{0, 7}}));
    EXPECT_EQ(result.description_match, (Ranges{{23, 26}}));
}

TEST(FuzzyTest, AvoidsScatteringAMatchAcrossTheDescription) {
    EXPECT_EQ(score(FileItem("projects/ui/cula/ats/target.mk"), "tcltarget.mk").label_match,
              (Ranges{{0, 9}}));
    EXPECT_EQ(score(FileItem("/1a111d1/11a1d1/something.txt"), "ad").description_match,
              (Ranges{{11, 12}, {13, 14}}));
}

TEST(FuzzyTest, SlashAndBackslashMatchEachOther) {
    const FileItem file("abcde/super/duper");
    EXPECT_GT(score(file, "abcde\\super\\duper").score, 0);
    EXPECT_GT(score(file, "abcde/super/duper").score, 0);
    EXPECT_GT(score_item(file.item(), prepare_query("abcde\\super\\duper", {u'\\', false})).score,
              0);
}

TEST(FuzzyTest, ACapitalInsideAWordOnlyEarnsItsBonusOutsideARun) {
    EXPECT_GT(fuzzy_score("asdfasdfasdf", "asdf", true), fuzzy_score("ASDFasdfasdf", "asdf", true));
}

TEST(FuzzyTest, AnExactPathBeatsEveryOtherMatch) {
    const std::vector<FileItem> files = {FileItem("/some/path/fileA.txt"),
                                         FileItem("/some/path/other/fileB.txt"),
                                         FileItem("/unrelated/some/path/other/fileC.txt")};
    EXPECT_EQ(ranked(files, files[1].path).front(), files[1].path);
    EXPECT_EQ(ranked({files[2], files[1], files[0]}, files[0].path).front(), files[0].path);
}

TEST(FuzzyTest, ALabelPrefixRanksFirstAndShorterLabelsRankAheadOfLongerOnes) {
    const std::vector<FileItem> files = {FileItem("/some/path/fileA.txt"),
                                         FileItem("/some/path/other/fileBLonger.txt"),
                                         FileItem("/unrelated/the/path/other/fileC.txt")};
    EXPECT_EQ(ranked(files, "file"),
              (std::vector<std::string>{files[0].path, files[2].path, files[1].path}));
    EXPECT_EQ(ranked({files[2], files[1], files[0]}, "file"),
              (std::vector<std::string>{files[0].path, files[2].path, files[1].path}));
}

TEST(FuzzyTest, ACamelCaseMatchRanksAboveAScatteredOne) {
    const std::vector<FileItem> files = {FileItem("/some/path/fileA.txt"),
                                         FileItem("/some/path/other/fileB.txt")};
    EXPECT_EQ(ranked(files, "fB").front(), files[1].path);
    EXPECT_EQ(ranked({files[1], files[0]}, "fA").front(), files[0].path);
}

TEST(FuzzyTest, AMatchInTheLabelBeatsOneThatNeededTheDescription) {
    const std::vector<FileItem> files = {FileItem("parts/quick/arrow-left-dark.svg"),
                                         FileItem("parts/quickopen/quickopen.ts")};
    EXPECT_EQ(ranked(files, "partsquick").front(), files[1].path);
}

TEST(FuzzyTest, ShorterPathsRankAheadWhenScoresAreEqual) {
    const std::vector<FileItem> files = {FileItem("config/test/t1.js"), FileItem("config/test.js"),
                                         FileItem("config/test/t2.js")};
    EXPECT_EQ(ranked(files, "co/te").front(), files[1].path);
}

TEST(FuzzyTest, WhenEverythingElseTiesAPrefixOfTheQueryRanksFirst) {
    const ItemScore none;
    const PreparedQuery query = prepare_query("b");
    EXPECT_LT(compare_items({"bx", "", ""}, none, {"ab", "", ""}, none, query), 0);
    EXPECT_GT(compare_items({"ab", "", ""}, none, {"bx", "", ""}, none, query), 0);
    EXPECT_EQ(compare_items({"Bb", "", ""}, none, {"bB", "", ""}, none, prepare_query("zz")), 0);
}

TEST(FuzzyTest, ReportsRangesInUtf16CodeUnits) {
    // The magnifier takes two code units.
    const ItemScore result =
        score_item({"\xF0\x9F\x94\x8D Find Symbol", "", ""}, prepare_query("find"));
    EXPECT_EQ(result.label_match, (Ranges{{3, 7}}));

    const FuzzyMatch match = score_fuzzy("caf\xC3\xA9 menu", prepare_query("menu"), true);
    EXPECT_EQ(match.positions, (std::vector<std::size_t>{5, 6, 7, 8}));
}

TEST(FuzzyTest, MalformedUtf8ReadsAsOneReplacementCharacterPerBadByte) {
    const ItemScore result = score_item({"a\xFF"
                                         "b",
                                         "", ""},
                                        prepare_query("b"));
    EXPECT_EQ(result.label_match, (Ranges{{2, 3}}));
}

TEST(FuzzyTest, CaseIsFoldedForAsciiLettersOnly) {
    EXPECT_GT(score_item({"Open File", "", ""}, prepare_query("OPEN")).score, 0);
    EXPECT_EQ(score_item({"\xC3\x9C"
                          "ber",
                          "", ""},
                         prepare_query("\xC3\xBC"
                                       "ber"))
                  .score,
              0);
    EXPECT_GT(score_item({"\xC3\x9C"
                          "ber",
                          "", ""},
                         prepare_query("\xC3\x9C"))
                  .score,
              0);
}

TEST(FuzzyTest, PathsAreComparedWithThePlatformsConventions) {
    const FuzzyItem file{"Main.cpp", "C:\\src", "C:\\src\\Main.cpp"};
    EXPECT_EQ(score_item(file, prepare_query("c:\\src\\main.cpp", {u'\\', false})).score, 1 << 18);
    EXPECT_NE(score_item(file, prepare_query("c:\\src\\main.cpp", {u'\\', true})).score, 1 << 18);
    EXPECT_EQ(score_item(file, prepare_query("C:/src/Main.cpp", {u'\\', true})).score, 1 << 18);
}

TEST(FuzzyTest, InvalidInputScoresZero) {
    EXPECT_EQ(score_item({"", "", ""}, prepare_query("x")).score, 0);
    EXPECT_EQ(score_item({"x", "", ""}, prepare_query("")).score, 0);
    EXPECT_EQ(score_item({"x", "", ""}, prepare_query("  ")).score, 0);
    EXPECT_EQ(fuzzy_score("", "x", true), 0);
    EXPECT_EQ(fuzzy_score("x", "xy", true), 0);
}
