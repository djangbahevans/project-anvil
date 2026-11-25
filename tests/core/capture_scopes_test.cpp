#include "anvil/core/capture_scopes.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <regex>
#include <sstream>

using namespace anvil::core;

namespace {

constexpr std::array rows = {
    CaptureScope{"string", "string.quoted"},
    CaptureScope{"string.special", "string.special.quoted"},
    CaptureScope{"string.special.key", "support.key"},
    CaptureScope{"number", "constant.numeric"},
};

} // namespace

TEST(CaptureScopes, AnExactNameFindsItsRow) {
    EXPECT_EQ(scope_for_capture(rows, "number"), "constant.numeric");
    EXPECT_EQ(scope_for_capture(rows, "string.special.key"), "support.key");
}

TEST(CaptureScopes, TheLongestRowThatNamesTheCaptureWins) {
    EXPECT_EQ(scope_for_capture(rows, "string"), "string.quoted");
    EXPECT_EQ(scope_for_capture(rows, "string.special"), "string.special.quoted");
    EXPECT_EQ(scope_for_capture(rows, "string.special.regex"), "string.special.quoted");
    EXPECT_EQ(scope_for_capture(rows, "string.escape"), "string.quoted");
}

TEST(CaptureScopes, ARowNamesOnlyWholeSegments) {
    EXPECT_EQ(scope_for_capture(rows, "stringy"), "");
    EXPECT_EQ(scope_for_capture(rows, "number2"), "");
    EXPECT_EQ(scope_for_capture(rows, "str"), "");
}

TEST(CaptureScopes, ACaptureWithoutARowHasNoScope) {
    EXPECT_EQ(scope_for_capture(rows, "comment"), "");
    EXPECT_EQ(scope_for_capture(rows, ""), "");
    EXPECT_EQ(scope_for_capture({}, "string"), "");
}

TEST(CaptureScopes, TheRowsOfAGrammarHaveScopesWithoutTheLanguageSuffix) {
    EXPECT_EQ(scope_for_capture(json_capture_scopes(), "string.special.key"),
              "support.type.property-name");
    EXPECT_EQ(scope_for_capture(json_capture_scopes(), "string"), "string.quoted.double");
    EXPECT_EQ(scope_for_capture(json_capture_scopes(), "constant.builtin"), "constant.language");
    for (const auto table : {json_capture_scopes(), c_capture_scopes(), cpp_capture_scopes(),
                             javascript_capture_scopes(), typescript_capture_scopes()}) {
        for (const CaptureScope& row : table) {
            EXPECT_FALSE(row.scope.empty()) << row.capture;
            for (const char* suffix : {".json", ".c", ".cpp", ".js", ".ts", ".tsx"}) {
                EXPECT_FALSE(row.scope.ends_with(suffix)) << row.scope;
            }
        }
    }
}

TEST(CaptureScopes, ARowCanBeWeak) {
    constexpr std::array rows = {CaptureScope{"variable", "variable.other", true},
                                 CaptureScope{"function", "entity.name.function"}};
    EXPECT_TRUE(find_capture_scope(rows, "variable")->weak);
    EXPECT_FALSE(find_capture_scope(rows, "function")->weak);
    EXPECT_EQ(find_capture_scope(rows, "constant"), nullptr);
}

TEST(CaptureScopes, TheTablesOfTheLanguagesThatBuildOnAnotherHaveItsRows) {
    EXPECT_EQ(scope_for_capture(cpp_capture_scopes(), "keyword.control"), "keyword.control");
    EXPECT_EQ(scope_for_capture(cpp_capture_scopes(), "keyword.operator.new"),
              "keyword.operator.new");
    EXPECT_EQ(scope_for_capture(c_capture_scopes(), "keyword.operator.new"), "keyword.other");
    EXPECT_EQ(scope_for_capture(typescript_capture_scopes(), "string.template"), "string.template");
    EXPECT_EQ(scope_for_capture(typescript_capture_scopes(), "support.type.primitive"),
              "support.type.primitive");
    EXPECT_EQ(scope_for_capture(javascript_capture_scopes(), "support.type.primitive"), "");
}

namespace {

std::string read_source_file(const char* relative) {
    std::ifstream in(std::filesystem::path(ANVIL_SOURCE_DIR) / relative, std::ios::binary);
    EXPECT_TRUE(in.good()) << relative;
    std::stringstream out;
    out << in.rdbuf();
    return out.str();
}

} // namespace

TEST(ThirdPartyNotices, NameEveryReleaseThatTheTreeSitterFetchScriptPins) {
    const std::string script = read_source_file("scripts/fetch-tree-sitter.sh");
    const std::string notices = read_source_file("THIRD_PARTY_NOTICES.md");
    const std::regex pin(R"(github\.com/([\w-]+/[\w-]+)/archive/refs/tags/(v[\d.]+)\.tar\.gz)");
    int pins = 0;
    for (std::sregex_iterator it(script.begin(), script.end(), pin), end; it != end; ++it) {
        ++pins;
        EXPECT_NE(
            notices.find("https://github.com/" + (*it)[1].str() + ", release " + (*it)[2].str()),
            std::string::npos)
            << (*it)[0];
    }
    EXPECT_GE(pins, 2);
}
