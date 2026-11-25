#include "anvil/core/language_comments.hpp"
#include "anvil/core/language_comments_table.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <map>
#include <nlohmann/json.hpp>
#include <sstream>

using namespace anvil::core;

namespace {

std::string read_text(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

LanguageComments only_line(std::string token, bool no_indent = false) {
    LanguageComments comments;
    comments.line = std::move(token);
    comments.line_no_indent = no_indent;
    return comments;
}

LanguageComments only_block(std::string open, std::string close) {
    LanguageComments comments;
    comments.block = BlockComment{std::move(open), std::move(close)};
    return comments;
}

} // namespace

TEST(ReadLanguageComments, ALineToken) {
    const ReadComments read = read_language_comments(R"j({"comments": {"lineComment": "//"}})j");
    EXPECT_EQ(read.comments, only_line("//"));
    EXPECT_TRUE(read.problems.empty());
}

TEST(ReadLanguageComments, ABlockToken) {
    const ReadComments read =
        read_language_comments(R"j({"comments": {"blockComment": ["/*", "*/"]}})j");
    EXPECT_EQ(read.comments, only_block("/*", "*/"));
    EXPECT_TRUE(read.problems.empty());
}

TEST(ReadLanguageComments, BothTokens) {
    const ReadComments read = read_language_comments(
        R"j({"comments": {"lineComment": "#", "blockComment": ["<#", "#>"]}})j");
    LanguageComments expected = only_line("#");
    expected.block = BlockComment{"<#", "#>"};
    EXPECT_EQ(read.comments, expected);
    EXPECT_TRUE(read.problems.empty());
}

TEST(ReadLanguageComments, ALineTokenGivenAsAnObjectHasItsNoIndentFlag) {
    EXPECT_EQ(read_language_comments(
                  R"j({"comments": {"lineComment": {"comment": "#", "noIndent": true}}})j")
                  .comments,
              only_line("#", true));
    EXPECT_EQ(read_language_comments(
                  R"j({"comments": {"lineComment": {"comment": "#", "noIndent": false}}})j")
                  .comments,
              only_line("#", false));
    EXPECT_EQ(
        read_language_comments(R"j({"comments": {"lineComment": {"comment": "#"}}})j").comments,
        only_line("#", false));
    EXPECT_EQ(read_language_comments(
                  R"j({"comments": {"lineComment": {"comment": "#", "noIndent": "yes"}}})j")
                  .comments,
              only_line("#", false));
}

TEST(ReadLanguageComments, JsonWithCommentsAndTrailingCommasIsRead) {
    const ReadComments read = read_language_comments(R"j(
        // a language configuration
        {
            "brackets": [["{", "}"],],
            "comments": {
                /* the tokens */
                "lineComment": "//",
                "blockComment": ["/*", "*/",],
            },
        }
    )j");
    LanguageComments expected = only_line("//");
    expected.block = BlockComment{"/*", "*/"};
    EXPECT_EQ(read.comments, expected);
    EXPECT_TRUE(read.problems.empty());
}

TEST(ReadLanguageComments, AConfigurationWithoutCommentsHasNoTokensAndNoProblem) {
    for (const char* text : {"{}", R"j({"brackets": [["(", ")"]]})j", R"j({"comments": {}})j"}) {
        const ReadComments read = read_language_comments(text);
        EXPECT_TRUE(read.comments.empty()) << text;
        EXPECT_TRUE(read.problems.empty()) << text;
    }
}

TEST(ReadLanguageComments, WrongShapesGiveNoTokenAndAProblemThatNamesTheKey) {
    struct Case {
        const char* text;
        const char* key;
    };
    const Case cases[] = {
        {R"j({"comments": {"lineComment": 3}})j", "lineComment"},
        {R"j({"comments": {"lineComment": ["#"]}})j", "lineComment"},
        {R"j({"comments": {"lineComment": {"comment": 3}}})j", "lineComment"},
        {R"j({"comments": {"lineComment": {"noIndent": true}}})j", "lineComment"},
        {R"j({"comments": {"blockComment": "/* */"}})j", "blockComment"},
        {R"j({"comments": {"blockComment": ["/*"]}})j", "blockComment"},
        {R"j({"comments": {"blockComment": ["/*", "*/", "x"]}})j", "blockComment"},
        {R"j({"comments": {"blockComment": ["/*", 3]}})j", "blockComment"},
        {R"j({"comments": {"blockComment": ["", "*/"]}})j", "blockComment"},
        {R"j({"comments": {"blockComment": ["/*", ""]}})j", "blockComment"},
        {R"j({"comments": "none"})j", "comments"},
        {R"j({"comments": ["//"]})j", "comments"},
    };
    for (const Case& test : cases) {
        const ReadComments read = read_language_comments(test.text);
        EXPECT_TRUE(read.comments.empty()) << test.text;
        ASSERT_EQ(read.problems.size(), 1U) << test.text;
        EXPECT_NE(read.problems[0].find(test.key), std::string::npos) << read.problems[0];
    }
}

TEST(ReadLanguageComments, AnEmptyLineTokenSaysThereIsNoneAndIsNotAProblem) {
    for (const char* text : {R"j({"comments": {"lineComment": ""}})j",
                             R"j({"comments": {"lineComment": {"comment": ""}}})j"}) {
        const ReadComments read = read_language_comments(text);
        EXPECT_TRUE(read.comments.empty()) << text;
        EXPECT_TRUE(read.problems.empty()) << text;
    }
    const ReadComments with_block = read_language_comments(
        R"j({"comments": {"lineComment": "", "blockComment": ["<!--", "-->"]}})j");
    EXPECT_EQ(with_block.comments, only_block("<!--", "-->"));
    EXPECT_TRUE(with_block.problems.empty());
}

TEST(ReadLanguageComments, ABadKindDoesNotTakeTheGoodOneAway) {
    const ReadComments bad_line = read_language_comments(
        R"j({"comments": {"lineComment": 3, "blockComment": ["/*", "*/"]}})j");
    EXPECT_EQ(bad_line.comments, only_block("/*", "*/"));
    EXPECT_EQ(bad_line.problems.size(), 1U);

    const ReadComments bad_block =
        read_language_comments(R"j({"comments": {"lineComment": "#", "blockComment": ["/*"]}})j");
    EXPECT_EQ(bad_block.comments, only_line("#"));
    EXPECT_EQ(bad_block.problems.size(), 1U);
}

TEST(ReadLanguageComments, TextThatIsNotAnObjectOrNotJsonGivesNoTokensAndAProblem) {
    for (const char* text : {"", "[1, 2]", "42", R"j("comments")j", "{", R"j({"comments": })j",
                             R"j({"comments": {"lineComment": "#"}} extra)j"}) {
        const ReadComments read = read_language_comments(text);
        EXPECT_TRUE(read.comments.empty()) << text;
        EXPECT_EQ(read.problems.size(), 1U) << text;
    }
    const ReadComments positioned = read_language_comments("{\n  \"comments\": ,\n}");
    ASSERT_EQ(positioned.problems.size(), 1U);
    EXPECT_NE(positioned.problems[0].find("line 2"), std::string::npos);
}

TEST(LanguageCommentsOverlay, ReplacesTheKindsTheOverlaySetsAndKeepsTheRest) {
    LanguageComments base = only_line("//");
    base.block = BlockComment{"/*", "*/"};

    EXPECT_EQ(base.overlaid_with(LanguageComments{}), base);
    EXPECT_EQ(LanguageComments{}.overlaid_with(base), base);

    LanguageComments expected = only_line("#", true);
    expected.block = BlockComment{"/*", "*/"};
    EXPECT_EQ(base.overlaid_with(only_line("#", true)), expected);

    LanguageComments with_block = only_line("//");
    with_block.block = BlockComment{"{-", "-}"};
    EXPECT_EQ(base.overlaid_with(only_block("{-", "-}")), with_block);

    EXPECT_EQ(base.overlaid_with(base), base);
}

TEST(BuiltinLanguageComments, TheLanguagesKnownToDifferHaveTheirOwnTokens) {
    const LanguageComments* python = builtin_language_comments("python");
    ASSERT_NE(python, nullptr);
    EXPECT_EQ(python->line, "#");
    EXPECT_EQ(python->block, (BlockComment{"\"\"\"", "\"\"\""}));

    const LanguageComments* html = builtin_language_comments("html");
    ASSERT_NE(html, nullptr);
    EXPECT_FALSE(html->line);
    EXPECT_EQ(html->block, (BlockComment{"<!--", "-->"}));

    const LanguageComments* cpp = builtin_language_comments("cpp");
    ASSERT_NE(cpp, nullptr);
    EXPECT_EQ(cpp->line, "//");
    EXPECT_EQ(cpp->block, (BlockComment{"/*", "*/"}));

    const LanguageComments* makefile = builtin_language_comments("makefile");
    ASSERT_NE(makefile, nullptr);
    EXPECT_EQ(makefile->line, "#");
    EXPECT_TRUE(makefile->line_no_indent);
    EXPECT_FALSE(makefile->block);

    const LanguageComments* c = builtin_language_comments("c");
    ASSERT_NE(c, nullptr);
    EXPECT_FALSE(c->line_no_indent);
}

TEST(BuiltinLanguageComments, ALanguageWithoutTokensAndAnUnknownOneHaveNone) {
    EXPECT_EQ(builtin_language_comments("plaintext"), nullptr);
    EXPECT_EQ(builtin_language_comments("no-such-language"), nullptr);
    EXPECT_EQ(builtin_language_comments(""), nullptr);
}

TEST(BuiltinLanguageComments, EveryRowOfTheTableHasATokenAndAnUniqueId) {
    std::map<std::string, int> seen;
    for (const LanguageCommentsRow& row : language_comments_table) {
        EXPECT_TRUE(!row.line.empty() || !row.block_open.empty()) << row.id;
        EXPECT_EQ(row.block_open.empty(), row.block_close.empty()) << row.id;
        EXPECT_FALSE(row.line_no_indent && row.line.empty()) << row.id;
        EXPECT_EQ(++seen[std::string(row.id)], 1) << row.id;
        ASSERT_NE(builtin_language_comments(row.id), nullptr) << row.id;
    }
}

// The table is what the reader makes of the language configuration files of the pinned checkout,
// merged for a language that several contributions name, in the order of the extension folders.
TEST(BuiltinLanguageComments, TheTableIsWhatTheReaderMakesOfTheConfigurationsOfTheCheckout) {
    const std::filesystem::path extensions = std::filesystem::path(ANVIL_VSCODE_DIR) / "extensions";
    if (!std::filesystem::exists(extensions)) {
        GTEST_SKIP() << "no VS Code checkout";
    }
    std::vector<std::filesystem::path> folders;
    for (const auto& entry : std::filesystem::directory_iterator(extensions)) {
        if (entry.is_directory() && std::filesystem::exists(entry.path() / "package.json")) {
            folders.push_back(entry.path());
        }
    }
    std::ranges::sort(folders);

    std::map<std::string, LanguageComments> expected;
    int configurations = 0;
    for (const std::filesystem::path& folder : folders) {
        const auto manifest = nlohmann::json::parse(read_text(folder / "package.json"));
        for (const nlohmann::json& language :
             manifest.value("contributes", nlohmann::json::object())
                 .value("languages", nlohmann::json::array())) {
            if (!language.contains("configuration")) {
                continue;
            }
            ++configurations;
            const std::string id = language.at("id").get<std::string>();
            const std::filesystem::path file =
                folder / language.at("configuration").get<std::string>();
            const ReadComments read = read_language_comments(read_text(file));
            EXPECT_TRUE(read.problems.empty()) << file;
            expected[id] = expected[id].overlaid_with(read.comments);
        }
    }
    EXPECT_GT(configurations, 50);
    std::size_t with_tokens = 0;
    for (const auto& [id, comments] : expected) {
        const LanguageComments* built = builtin_language_comments(id);
        if (comments.empty()) {
            EXPECT_EQ(built, nullptr) << id;
            continue;
        }
        ++with_tokens;
        ASSERT_NE(built, nullptr) << id;
        EXPECT_EQ(*built, comments) << id;
    }
    EXPECT_EQ(with_tokens, std::size(language_comments_table));
}
