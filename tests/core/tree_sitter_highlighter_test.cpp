#include "anvil/core/tree_sitter_highlighter.hpp"
#include "anvil/core/utf.hpp"

#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <string>

#ifdef ANVIL_TREE_SITTER

using namespace anvil::core;
using anvil::ErrorCode;

namespace {

std::string read_file(const std::string& name) {
    std::ifstream in(std::string(ANVIL_CORE_TEST_DATA_DIR) + "/" + name, std::ios::binary);
    EXPECT_TRUE(in.good()) << name;
    std::stringstream out;
    out << in.rdbuf();
    return out.str();
}

Text text_of(std::string_view utf8) {
    auto text = Text::from_utf8(utf8);
    EXPECT_TRUE(text.has_value());
    return text ? *text : Text{};
}

std::unique_ptr<TreeSitterHighlighter> json_highlighter(const Text& text) {
    auto highlighter = TreeSitterHighlighter::create("json", text);
    EXPECT_TRUE(highlighter.has_value());
    return highlighter ? std::move(*highlighter) : nullptr;
}

// One line per span: `<line> <begin>-<end> <scope>`.
std::string render(const Highlighter& highlighter, const Text& text) {
    std::string out;
    for (std::size_t line = 0; line < text.line_count(); ++line) {
        for (const HighlightSpan& span : highlighter.spans(line)) {
            out += std::to_string(line) + " " + std::to_string(span.begin) + "-" +
                   std::to_string(span.end) + " " + std::string(span.scope) + "\n";
        }
    }
    return out;
}

void expect_well_formed(const Highlighter& highlighter, const Text& text) {
    for (std::size_t line = 0; line < text.line_count(); ++line) {
        const std::size_t length = *utf16_length(text.line(line));
        std::size_t previous_end = 0;
        for (const HighlightSpan& span : highlighter.spans(line)) {
            EXPECT_LT(span.begin, span.end) << "line " << line;
            EXPECT_GE(span.begin, previous_end) << "line " << line;
            EXPECT_LE(span.end, length) << "line " << line;
            EXPECT_FALSE(span.scope.empty());
            previous_end = span.end;
        }
    }
}

} // namespace

TEST(TreeSitterHighlighter, TheJsonSampleMatchesTheGoldenFile) {
    const Text text = text_of(read_file("highlight_json_sample.json"));
    const auto highlighter = json_highlighter(text);
    ASSERT_TRUE(highlighter);
    EXPECT_EQ(render(*highlighter, text), read_file("highlight_json_sample.spans"));
    expect_well_formed(*highlighter, text);
}

TEST(TreeSitterHighlighter, TheRootScopeIsTheLanguages) {
    const auto highlighter = json_highlighter(text_of("[]"));
    ASSERT_TRUE(highlighter);
    EXPECT_EQ(highlighter->root_scope(), "source.json");
}

TEST(TreeSitterHighlighter, AGrammarIsFoundByLanguageId) {
    EXPECT_TRUE(TreeSitterHighlighter::supports("json"));
    EXPECT_FALSE(TreeSitterHighlighter::supports("cpp"));
    EXPECT_FALSE(TreeSitterHighlighter::supports(""));
    const auto missing = TreeSitterHighlighter::create("cpp", Text{});
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code, ErrorCode::unsupported);
}

TEST(TreeSitterHighlighter, AnEmptyDocumentHasNoSpans) {
    const Text text;
    const auto highlighter = json_highlighter(text);
    ASSERT_TRUE(highlighter);
    EXPECT_TRUE(highlighter->spans(0).empty());
    EXPECT_TRUE(highlighter->spans(1).empty());
    EXPECT_TRUE(highlighter->spans(1000).empty());
}

TEST(TreeSitterHighlighter, ALinePastTheEndAndABlankLineHaveNoSpans) {
    const Text text = text_of("{\n\n  \"a\": 1\n}\n");
    const auto highlighter = json_highlighter(text);
    ASSERT_TRUE(highlighter);
    EXPECT_TRUE(highlighter->spans(1).empty());
    EXPECT_TRUE(highlighter->spans(4).empty());
    EXPECT_TRUE(highlighter->spans(5).empty());
    EXPECT_FALSE(highlighter->spans(2).empty());
}

TEST(TreeSitterHighlighter, AKeyIsAPropertyNameAndAValueIsAString) {
    const Text text = text_of(R"({"key": "value"})");
    const auto highlighter = json_highlighter(text);
    ASSERT_TRUE(highlighter);
    const auto spans = highlighter->spans(0);
    ASSERT_EQ(spans.size(), 2u);
    EXPECT_EQ(spans[0].begin, 1u);
    EXPECT_EQ(spans[0].end, 6u);
    EXPECT_EQ(spans[0].scope, "support.type.property-name.json");
    EXPECT_EQ(spans[1].begin, 8u);
    EXPECT_EQ(spans[1].end, 15u);
    EXPECT_EQ(spans[1].scope, "string.quoted.double.json");
}

TEST(TreeSitterHighlighter, AnEscapeSplitsTheStringItIsIn) {
    const Text text = text_of(R"(["a\nb"])");
    const auto highlighter = json_highlighter(text);
    ASSERT_TRUE(highlighter);
    const auto spans = highlighter->spans(0);
    ASSERT_EQ(spans.size(), 3u);
    EXPECT_EQ(spans[0].scope, "string.quoted.double.json");
    EXPECT_EQ(spans[0].end, 3u);
    EXPECT_EQ(spans[1].scope, "constant.character.escape.json");
    EXPECT_EQ(spans[1].begin, 3u);
    EXPECT_EQ(spans[1].end, 5u);
    EXPECT_EQ(spans[2].scope, "string.quoted.double.json");
    EXPECT_EQ(spans[2].begin, 5u);
    EXPECT_EQ(spans[2].end, 7u);
}

TEST(TreeSitterHighlighter, ABlockCommentIsClippedToEachOfItsLines) {
    const Text text = text_of("[1, /* a\n b\n c */ 2]");
    const auto highlighter = json_highlighter(text);
    ASSERT_TRUE(highlighter);
    const auto first = highlighter->spans(0);
    ASSERT_EQ(first.size(), 2u);
    EXPECT_EQ(first[1].begin, 4u);
    EXPECT_EQ(first[1].end, 8u);
    EXPECT_EQ(first[1].scope, "comment.json");
    const auto middle = highlighter->spans(1);
    ASSERT_EQ(middle.size(), 1u);
    EXPECT_EQ(middle[0].begin, 0u);
    EXPECT_EQ(middle[0].end, 2u);
    const auto last = highlighter->spans(2);
    ASSERT_EQ(last.size(), 2u);
    EXPECT_EQ(last[0].begin, 0u);
    EXPECT_EQ(last[0].end, 5u);
    EXPECT_EQ(last[1].scope, "constant.numeric.json");
}

TEST(TreeSitterHighlighter, ColumnsAreUtf16CodeUnitsNotBytes) {
    // "é" is two bytes and one unit, the emoji four bytes and two units.
    const Text text = text_of("[\"\xC3\xA9\xF0\x9F\x98\x80\", 12, \"z\"]");
    const auto highlighter = json_highlighter(text);
    ASSERT_TRUE(highlighter);
    const auto spans = highlighter->spans(0);
    ASSERT_EQ(spans.size(), 3u);
    EXPECT_EQ(spans[0].begin, 1u);
    EXPECT_EQ(spans[0].end, 6u);
    EXPECT_EQ(spans[1].begin, 8u);
    EXPECT_EQ(spans[1].end, 10u);
    EXPECT_EQ(spans[1].scope, "constant.numeric.json");
    EXPECT_EQ(spans[2].begin, 12u);
    EXPECT_EQ(spans[2].end, 15u);
}

TEST(TreeSitterHighlighter, ASyntaxErrorLeavesTheRestHighlighted) {
    const Text text = text_of("{\"a\": 1,, \"b\": [true,\n \"c\": ]\n}\n[null]");
    const auto highlighter = json_highlighter(text);
    ASSERT_TRUE(highlighter);
    expect_well_formed(*highlighter, text);

    const std::string rendered = render(*highlighter, text);
    EXPECT_NE(rendered.find("0 1-4 support.type.property-name.json"), std::string::npos)
        << rendered;
    EXPECT_NE(rendered.find("0 6-7 constant.numeric.json"), std::string::npos) << rendered;
    EXPECT_NE(rendered.find("constant.language.json"), std::string::npos) << rendered;
    EXPECT_NE(rendered.find("3 1-5 constant.language.json"), std::string::npos) << rendered;
}

TEST(TreeSitterHighlighter, ALongLineIsHighlightedWhole) {
    constexpr std::size_t count = 40000;
    std::string source = "[";
    for (std::size_t i = 0; i < count; ++i) {
        source += i == 0 ? "1" : ",1";
    }
    source += "]\n";
    const Text text = text_of(source);
    const auto highlighter = json_highlighter(text);
    ASSERT_TRUE(highlighter);
    const auto spans = highlighter->spans(0);
    ASSERT_EQ(spans.size(), count);
    EXPECT_EQ(spans.front().begin, 1u);
    EXPECT_EQ(spans.front().end, 2u);
    EXPECT_EQ(spans.back().begin, 2 * count - 1);
    EXPECT_EQ(spans.back().end, 2 * count);
    EXPECT_TRUE(highlighter->spans(1).empty());
}

TEST(TreeSitterHighlighter, ADocumentOfManyChunksIsReadWhole) {
    std::string source = "[\n";
    for (int i = 0; i < 6000; ++i) {
        source += "  \"row " + std::to_string(i) + "\",\n";
    }
    source += "  null\n]\n";
    const Text text = text_of(source);
    ASSERT_GT(text.chunk_count(), 3u);
    const auto highlighter = json_highlighter(text);
    ASSERT_TRUE(highlighter);
    for (const std::size_t line : {std::size_t{1}, std::size_t{3000}, std::size_t{6000}}) {
        const auto spans = highlighter->spans(line);
        ASSERT_EQ(spans.size(), 1u) << line;
        EXPECT_EQ(spans[0].scope, "string.quoted.double.json");
    }
    const auto last = highlighter->spans(6001);
    ASSERT_EQ(last.size(), 1u);
    EXPECT_EQ(last[0].scope, "constant.language.json");
}

TEST(TreeSitterHighlighter, TheHighlighterKeepsTheSnapshotItWasMadeFrom) {
    Text text = text_of("[1]");
    const auto highlighter = json_highlighter(text);
    ASSERT_TRUE(highlighter);
    text = *text.replaced(0, 3, "\"changed\"");
    const auto spans = highlighter->spans(0);
    ASSERT_EQ(spans.size(), 1u);
    EXPECT_EQ(spans[0].scope, "constant.numeric.json");
}

#else

TEST(TreeSitterHighlighter, NeedsTheTreeSitterSources) {
    GTEST_SKIP() << "built without .deps/tree-sitter; run scripts/fetch-tree-sitter.sh";
}

#endif
