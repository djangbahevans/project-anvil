#include "anvil/core/file_name_order.hpp"

#include <algorithm>
#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <random>
#include <string>
#include <vector>

using anvil::core::compare_file_names;
using anvil::core::FileNameKey;

namespace {

nlohmann::json recorded_cases() {
    std::ifstream file(std::string(ANVIL_CORE_TEST_DATA_DIR) + "/file_name_order_cases.json");
    return nlohmann::json::parse(file);
}

std::vector<std::string> names_of(const nlohmann::json& sorted) {
    return sorted.get<std::vector<std::string>>();
}

} // namespace

TEST(FileNameOrder, MatchesTheOrderThatVsCodeRecorded) {
    const nlohmann::json cases = recorded_cases();
    ASSERT_FALSE(cases["cases"].empty());
    for (const nlohmann::json& one : cases["cases"]) {
        const std::vector<std::string> expected = names_of(one["sorted"]);
        for (std::size_t i = 0; i < expected.size(); ++i) {
            for (std::size_t j = i + 1; j < expected.size(); ++j) {
                EXPECT_TRUE(compare_file_names(expected[i], expected[j]) < 0)
                    << one["name"] << ": \"" << expected[i] << "\" should sort before \""
                    << expected[j] << "\"";
            }
        }
    }
}

TEST(FileNameOrder, SortingAShuffledCorpusGivesTheRecordedOrder) {
    const nlohmann::json cases = recorded_cases();
    std::mt19937 random(7);
    for (const nlohmann::json& one : cases["cases"]) {
        const std::vector<std::string> expected = names_of(one["sorted"]);
        for (int round = 0; round < 5; ++round) {
            std::vector<std::string> names = expected;
            std::ranges::shuffle(names, random);
            std::ranges::sort(names, [](const std::string& a, const std::string& b) {
                return compare_file_names(a, b) < 0;
            });
            EXPECT_EQ(names, expected) << one["name"];
        }
    }
}

TEST(FileNameOrder, AKeyOrdersLikeTheFunction) {
    const std::vector<std::string> names = {"a", "A", "a1", "a01", "é", "e", "ø", "", "10", "9"};
    for (const std::string& a : names) {
        for (const std::string& b : names) {
            EXPECT_EQ(FileNameKey(a) <=> FileNameKey(b), compare_file_names(a, b));
            EXPECT_EQ(FileNameKey(a) == FileNameKey(b), a == b);
        }
    }
}

TEST(FileNameOrder, DigitRunsCompareAsNumbers) {
    EXPECT_TRUE(compare_file_names("file9", "file10") < 0);
    EXPECT_TRUE(compare_file_names("a2b", "a10b") < 0);
    EXPECT_TRUE(compare_file_names("a1b2", "a1b10") < 0);
}

TEST(FileNameOrder, NamesThatAreEqualToTheCollatorSortTheShorterFirst) {
    EXPECT_TRUE(compare_file_names("a1", "a01") < 0);
    EXPECT_TRUE(compare_file_names("a01", "a001") < 0);
    EXPECT_TRUE(compare_file_names("a001", "a1") > 0);
}

TEST(FileNameOrder, OnlyIdenticalNamesAreEqual) {
    EXPECT_TRUE(compare_file_names("same.txt", "same.txt") == 0);
    EXPECT_TRUE(compare_file_names("a\x01", "a") != 0);
    EXPECT_TRUE(compare_file_names("x\x01y", "xy") != 0);
}

TEST(FileNameOrder, InvalidUtf8SortsAfterEveryCharacterAndKeepsAnOrder) {
    EXPECT_TRUE(compare_file_names("a\xFF", "a\xEF\xBF\xBD") > 0);
    EXPECT_TRUE(compare_file_names("a\x80", "a\xFF") < 0);
    EXPECT_TRUE(compare_file_names("a\xC3", "a") != 0);
    EXPECT_TRUE(compare_file_names("\xE3\x81", "\xE3") != 0);
    // An overlong encoding of "/" and an encoded surrogate are bytes, not characters.
    EXPECT_TRUE(compare_file_names("\xC0\xAF", "/") != 0);
    EXPECT_TRUE(compare_file_names("\xED\xA0\x80", "a") != 0);
}

TEST(FileNameOrder, TheLengthTieBreakCountsUtf16UnitsNotBytes) {
    // The collation ignores U+0080, so the first name is "a1" to it and the second "a1" as well.
    // The first has 3 UTF-16 units and the second 4; both have 4 bytes, and the bytes of the first
    // sort after those of the second.
    EXPECT_TRUE(compare_file_names("a1\xC2\x80", "a001") < 0);
}

TEST(FileNameOrder, TheEmptyNameSortsFirst) {
    EXPECT_TRUE(compare_file_names("", "a") < 0);
    EXPECT_TRUE(compare_file_names("", "") == 0);
}
