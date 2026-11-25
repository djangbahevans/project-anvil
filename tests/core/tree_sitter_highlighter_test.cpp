#include "anvil/core/tree_sitter_highlighter.hpp"
#include "anvil/core/utf.hpp"

#include <atomic>
#include <fstream>
#include <gtest/gtest.h>
#include <random>
#include <span>
#include <sstream>
#include <stop_token>
#include <string>
#include <thread>

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

std::unique_ptr<TreeSitterHighlighter> highlighter_for(std::string_view language,
                                                       const Text& text) {
    auto highlighter = TreeSitterHighlighter::create(language, text);
    EXPECT_TRUE(highlighter.has_value()) << (highlighter ? "" : highlighter.error().message);
    return highlighter ? std::move(*highlighter) : nullptr;
}

std::unique_ptr<TreeSitterHighlighter> json_highlighter(const Text& text) {
    return highlighter_for("json", text);
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

// What a fresh highlighter says about the text, which an edited one must agree with.
std::string fresh_render(const Text& text, std::string_view language = "json") {
    const auto highlighter = highlighter_for(language, text);
    return highlighter ? render(*highlighter, text) : std::string();
}

// A document with enough variety that random edits break it in many ways: nested containers,
// strings with escapes, multibyte text, comments and a line comment that ends the line.
std::string json_document(std::mt19937& random, int items) {
    static const char* const values[] = {"1",
                                         "-2.5e3",
                                         "true",
                                         "null",
                                         "\"plain\"",
                                         "\"caf\xC3\xA9 \xF0\x9F\x98\x80\"",
                                         "\"a\\n\\\"b\"",
                                         "[1, [2, {\"k\": 3}]]",
                                         "{}"};
    std::string out = "{\n";
    for (int i = 0; i < items; ++i) {
        out += "  \"key" + std::to_string(i) + "\": ";
        out += values[random() % std::size(values)];
        if (random() % 5 == 0) {
            out += " // note \xC3\xA9";
        }
        if (random() % 7 == 0) {
            out += "\n  /* block\n     comment */";
        }
        out += i + 1 < items ? ",\n" : "\n";
    }
    return out + "}\n";
}

const char* const fragments[] = {
    "",   "{",  "}",  "[",    "]",         ",", ":",    "\"",    "\\",       "/*",
    "*/", "//", "\n", "\n\n", " ",         "1", "null", "\"x\"", "\xC3\xA9", "\xF0\x9F\x98\x80",
    "tr", "ue", "-",  "e5",   "\"k\": 1, "};

// A random edit: a range on character boundaries and a replacement made of fragments.
struct RandomEdit {
    std::size_t start = 0;
    std::size_t end = 0;
    std::string text;
};

RandomEdit random_edit(std::mt19937& random, const Text& text,
                       std::span<const char* const> pieces = fragments) {
    const std::size_t size = text.size();
    auto boundary = [&](std::size_t offset) {
        offset = std::min(offset, size);
        while (!text.is_char_boundary(offset)) {
            ++offset;
        }
        return offset;
    };
    RandomEdit edit;
    edit.start = boundary(random() % (size + 1));
    const std::size_t length = random() % 3 == 0 ? random() % 40 : random() % 4;
    edit.end = boundary(edit.start + length);
    const int parts = static_cast<int>(random() % 4);
    for (int i = 0; i < parts; ++i) {
        edit.text += pieces[random() % pieces.size()];
    }
    return edit;
}

} // namespace

TEST(TreeSitterHighlighter, TheJsonSampleMatchesTheGoldenFile) {
    const Text text = text_of(read_file("highlight_json_sample.json"));
    const auto highlighter = json_highlighter(text);
    ASSERT_TRUE(highlighter);
    EXPECT_EQ(render(*highlighter, text), read_file("highlight_json_sample.spans"));
    expect_well_formed(*highlighter, text);
}

TEST(TreeSitterHighlighter, TheCSampleMatchesTheGoldenFile) {
    const Text text = text_of(read_file("highlight_c_sample.c"));
    const auto highlighter = highlighter_for("c", text);
    ASSERT_TRUE(highlighter);
    EXPECT_EQ(render(*highlighter, text), read_file("highlight_c_sample.spans"));
    expect_well_formed(*highlighter, text);
}

TEST(TreeSitterHighlighter, TheCppSampleMatchesTheGoldenFile) {
    const Text text = text_of(read_file("highlight_cpp_sample.cpp"));
    const auto highlighter = highlighter_for("cpp", text);
    ASSERT_TRUE(highlighter);
    EXPECT_EQ(render(*highlighter, text), read_file("highlight_cpp_sample.spans"));
    expect_well_formed(*highlighter, text);
}

TEST(TreeSitterHighlighter, TheCapturesWithoutAScopeAreTheOnesTheTablesLeaveOut) {
    EXPECT_TRUE(TreeSitterHighlighter::unmapped_captures("json").empty());
    const std::vector<std::string> c = {"variable", "delimiter"};
    EXPECT_EQ(TreeSitterHighlighter::unmapped_captures("c"), c);
    EXPECT_EQ(TreeSitterHighlighter::unmapped_captures("cpp"), c);
    EXPECT_TRUE(TreeSitterHighlighter::unmapped_captures("python").empty());
}

TEST(TreeSitterHighlighter, AnIdentifierIsAConstantOnlyWhenItsTextMatchesThePredicate) {
    const Text text = text_of("int a = MAX_SIZE + x + Y2 + v1 + _A;\n");
    const auto highlighter = highlighter_for("c", text);
    ASSERT_TRUE(highlighter);
    std::string constants;
    for (const HighlightSpan& span : highlighter->spans(0)) {
        if (span.scope == "variable.other.constant.c") {
            constants += text.line(0).substr(span.begin, span.end - span.begin) + " ";
        }
    }
    EXPECT_EQ(constants, "MAX_SIZE Y2 ");
}

namespace {

std::vector<std::string> strings_matching(std::string_view predicate) {
    const Text text = text_of(R"(["apple", "banana", "avocado", 7])");
    const std::string query = "((string) @string " + std::string(predicate) + ")";
    const auto highlighter = TreeSitterHighlighter::create_with_query("json", query, text);
    EXPECT_TRUE(highlighter.has_value()) << (highlighter ? "" : highlighter.error().message);
    std::vector<std::string> found;
    if (highlighter) {
        for (const HighlightSpan& span : (*highlighter)->spans(0)) {
            found.push_back(text.line(0).substr(span.begin, span.end - span.begin));
        }
    }
    return found;
}

} // namespace

TEST(TreeSitterHighlighter, AMatchPredicateKeepsTheNodesWhoseTextMatches) {
    EXPECT_EQ(strings_matching(R"q((#match? @string "^\"a"))q"),
              (std::vector<std::string>{"\"apple\"", "\"avocado\""}));
    EXPECT_TRUE(strings_matching(R"q((#match? @string "zzz"))q").empty());
}

TEST(TreeSitterHighlighter, ANotMatchPredicateKeepsTheNodesWhoseTextDoesNotMatch) {
    EXPECT_EQ(strings_matching(R"q((#not-match? @string "^\"a"))q"),
              (std::vector<std::string>{"\"banana\""}));
}

TEST(TreeSitterHighlighter, AQueryWithAPredicateThatIsNotUnderstoodIsRefused) {
    const auto refused = TreeSitterHighlighter::create_with_query(
        "json", R"q(((string) @s (#eq? @s "\"a\"")))q", Text{});
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error().code, ErrorCode::environment_unavailable);
    EXPECT_NE(refused.error().message.find("#eq?"), std::string::npos) << refused.error().message;
}

TEST(TreeSitterHighlighter, AQueryThatDoesNotCompileSaysWhatIsWrongAndWhere) {
    const auto refused =
        TreeSitterHighlighter::create_with_query("json", "(string) @s\n(nonsense) @t", Text{});
    ASSERT_FALSE(refused.has_value());
    EXPECT_NE(refused.error().message.find("node type"), std::string::npos)
        << refused.error().message;
    EXPECT_NE(refused.error().message.find("nonsense"), std::string::npos)
        << refused.error().message;
}

TEST(TreeSitterHighlighter, TheRootScopeIsTheLanguages) {
    const auto highlighter = json_highlighter(text_of("[]"));
    ASSERT_TRUE(highlighter);
    EXPECT_EQ(highlighter->root_scope(), "source.json");
}

TEST(TreeSitterHighlighter, AGrammarIsFoundByLanguageId) {
    EXPECT_TRUE(TreeSitterHighlighter::supports("json"));
    EXPECT_TRUE(TreeSitterHighlighter::supports("c"));
    EXPECT_TRUE(TreeSitterHighlighter::supports("cpp"));
    EXPECT_FALSE(TreeSitterHighlighter::supports("python"));
    EXPECT_FALSE(TreeSitterHighlighter::supports(""));
    const auto missing = TreeSitterHighlighter::create("python", Text{});
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

TEST(TreeSitterHighlighter, EditedHighlightingEqualsAFreshParseOfTheNewText) {
    std::mt19937 random(2026);
    for (int document = 0; document < 10; ++document) {
        TextBuffer buffer(text_of(json_document(random, 4 + document * 3)));
        const auto highlighter = json_highlighter(buffer.snapshot());
        ASSERT_TRUE(highlighter);
        for (int step = 0; step < 120; ++step) {
            // Fill the cache, so that a span kept past an edit that should have dropped it shows.
            (void)render(*highlighter, buffer.snapshot());

            const RandomEdit edit = random_edit(random, buffer.snapshot());
            const auto change = buffer.replace(edit.start, edit.end, edit.text);
            ASSERT_TRUE(change.has_value());
            ASSERT_TRUE(highlighter->apply(*change, buffer.snapshot()).has_value());
            ASSERT_EQ(highlighter->describe_tree(),
                      json_highlighter(buffer.snapshot())->describe_tree())
                << "document " << document << " step " << step;
            ASSERT_EQ(render(*highlighter, buffer.snapshot()), fresh_render(buffer.snapshot()))
                << "document " << document << " step " << step << " edit [" << edit.start << ", "
                << edit.end << ") -> \"" << edit.text << "\"\n"
                << buffer.snapshot().to_string();
        }
    }
}

namespace {

const char* const c_fragments[] = {"",     "{",         "}",           "(",
                                   ")",    ";",         "\n",          "\n\n",
                                   "\"",   "'",         "/*",          "*/",
                                   "//",   "#include ", "#if",         "#define X ",
                                   "int ", "const ",    "if (",        "return ",
                                   "x",    "_t",        "1",           "0x",
                                   "->",   "struct ",   "caf\xC3\xA9", "\xF0\x9F\x98\x80",
                                   "\\",   "<",         ">",           "NULL",
                                   "FOO",  "::",        "class ",      "template <typename T> ",
                                   "new ", "nullptr",   "R\"("};

// Random edits to a sample document, after each of which an edited highlighter must agree with a
// fresh one on every line and on the tree.
void expect_edits_to_agree_with_a_fresh_parse(std::string_view language, const char* sample,
                                              unsigned seed) {
    std::mt19937 random(seed);
    for (int round = 0; round < 3; ++round) {
        TextBuffer buffer(text_of(read_file(sample)));
        const auto highlighter = highlighter_for(language, buffer.snapshot());
        ASSERT_TRUE(highlighter);
        for (int step = 0; step < 100; ++step) {
            (void)render(*highlighter, buffer.snapshot());
            const RandomEdit edit = random_edit(random, buffer.snapshot(), c_fragments);
            const auto change = buffer.replace(edit.start, edit.end, edit.text);
            ASSERT_TRUE(change.has_value());
            ASSERT_TRUE(highlighter->apply(*change, buffer.snapshot()).has_value());
            ASSERT_EQ(highlighter->describe_tree(),
                      highlighter_for(language, buffer.snapshot())->describe_tree())
                << "round " << round << " step " << step;
            ASSERT_EQ(render(*highlighter, buffer.snapshot()),
                      fresh_render(buffer.snapshot(), language))
                << "round " << round << " step " << step << " edit [" << edit.start << ", "
                << edit.end << ") -> \"" << edit.text << "\"\n"
                << buffer.snapshot().to_string();
        }
    }
}

} // namespace

TEST(TreeSitterHighlighter, EditedCHighlightingEqualsAFreshParseOfTheNewText) {
    expect_edits_to_agree_with_a_fresh_parse("c", "highlight_c_sample.c", 11);
}

TEST(TreeSitterHighlighter, EditedCppHighlightingEqualsAFreshParseOfTheNewText) {
    expect_edits_to_agree_with_a_fresh_parse("cpp", "highlight_cpp_sample.cpp", 12);
}

TEST(TreeSitterHighlighter, ADocumentChangeOfSeveralEditsIsFollowed) {
    std::mt19937 random(7);
    Document document(text_of(json_document(random, 30)));
    const auto highlighter = json_highlighter(document.text());
    ASSERT_TRUE(highlighter);
    for (int step = 0; step < 120; ++step) {
        (void)render(*highlighter, document.text());
        std::vector<Edit> edits;
        std::size_t floor = 0;
        for (int i = 0, count = 1 + static_cast<int>(random() % 4); i < count; ++i) {
            const Text rest = document.text();
            RandomEdit edit = random_edit(random, rest);
            if (edit.start < floor) {
                continue;
            }
            floor = edit.end;
            edits.push_back(Edit{edit.start, edit.end, edit.text});
        }
        const auto change = document.apply_edits(edits);
        ASSERT_TRUE(change.has_value());
        ASSERT_TRUE(highlighter->apply(*change, document.text()).has_value());
        ASSERT_EQ(highlighter->describe_tree(), json_highlighter(document.text())->describe_tree())
            << "step " << step;
        ASSERT_EQ(render(*highlighter, document.text()), fresh_render(document.text()))
            << "step " << step;
    }
}

TEST(TreeSitterHighlighter, AnEditThatDoesNotLeadToTheNewTextIsRefusedAndChangesNothing) {
    TextBuffer buffer(text_of("[1, 2]\n[3]"));
    const auto highlighter = json_highlighter(buffer.snapshot());
    ASSERT_TRUE(highlighter);
    const std::string before = render(*highlighter, buffer.snapshot());
    const auto change = buffer.replace(1, 2, "\"long\"");
    ASSERT_TRUE(change.has_value());

    const auto wrong_text = highlighter->apply(*change, text_of("[1, 2]\n[3]"));
    ASSERT_FALSE(wrong_text.has_value());
    EXPECT_EQ(wrong_text.error().code, ErrorCode::invalid_argument);

    TextChange past_the_end = *change;
    past_the_end.offset = 1000;
    EXPECT_FALSE(highlighter->apply(past_the_end, buffer.snapshot()).has_value());
    EXPECT_EQ(render(*highlighter, text_of("[1, 2]\n[3]")), before);

    ASSERT_TRUE(highlighter->apply(*change, buffer.snapshot()).has_value());
    EXPECT_EQ(render(*highlighter, buffer.snapshot()), fresh_render(buffer.snapshot()));
}

TEST(TreeSitterHighlighter, TheVersionChangesWithEachEdit) {
    TextBuffer buffer(text_of("[1]"));
    const auto highlighter = json_highlighter(buffer.snapshot());
    ASSERT_TRUE(highlighter);
    const std::uint64_t first = highlighter->version();
    const auto change = buffer.insert(2, "2");
    ASSERT_TRUE(highlighter->apply(*change, buffer.snapshot()).has_value());
    EXPECT_NE(highlighter->version(), first);
}

namespace {

// Objects within objects, so that no node has more than a few dozen children.
std::string nested_json(int groups) {
    std::string out = "{\n";
    for (int a = 0; a < groups; ++a) {
        out += "  \"group" + std::to_string(a) + "\": {\n";
        for (int b = 0; b < 30; ++b) {
            out += "    \"item" + std::to_string(b) +
                   "\": [1, 2.5, \"value\", true, null, {\"a\": \"b\"}]";
            out += b + 1 < 30 ? ",\n" : "\n";
        }
        out += a + 1 < groups ? "  },\n" : "  }\n";
    }
    return out + "}\n";
}

std::string big_json(std::size_t lines) {
    std::string out = "{\n";
    for (std::size_t i = 0; i < lines; ++i) {
        out +=
            "  \"key" + std::to_string(i) + "\": [1, 2.5, \"value\", true, null, {\"a\": \"b\"}]";
        out += i + 1 < lines ? ",\n" : "\n";
    }
    return out + "}\n";
}

} // namespace

TEST(TreeSitterHighlighter, AnEditAsksTheQueryForTheLinesItReachedOnly) {
    TextBuffer buffer(text_of(big_json(2000)));
    const auto highlighter = json_highlighter(buffer.snapshot());
    ASSERT_TRUE(highlighter);
    (void)render(*highlighter, buffer.snapshot());
    ASSERT_EQ(highlighter->line_queries(), buffer.snapshot().line_count());

    // A character inside a number on line 1000 changes that line and nothing else.
    const std::size_t at = buffer.snapshot().line_start(1000) + 20;
    const auto change = buffer.replace(at, at + 1, "7");
    ASSERT_TRUE(change.has_value());
    ASSERT_TRUE(highlighter->apply(*change, buffer.snapshot()).has_value());
    const std::size_t before = highlighter->line_queries();
    (void)render(*highlighter, buffer.snapshot());
    EXPECT_LE(highlighter->line_queries() - before, 3u);
    EXPECT_GE(highlighter->line_queries() - before, 1u);
}

TEST(TreeSitterHighlighter, AnEditReparsesAFractionOfTheBytesOfTheFirstParse) {
    TextBuffer buffer(text_of(nested_json(700)));
    ASSERT_GT(buffer.snapshot().size(), 1000u * 1000);
    const auto highlighter = json_highlighter(buffer.snapshot());
    ASSERT_TRUE(highlighter);
    const std::size_t first_parse = highlighter->bytes_read();
    ASSERT_GE(first_parse, buffer.snapshot().size());

    const std::size_t at = buffer.snapshot().size() / 2;
    const auto change = buffer.replace(at, at, " ");
    ASSERT_TRUE(change.has_value());
    ASSERT_TRUE(highlighter->apply(*change, buffer.snapshot()).has_value());
    const std::size_t reparse = highlighter->bytes_read() - first_parse;
    EXPECT_GT(reparse, 0u);
    EXPECT_LT(reparse * 50, first_parse) << reparse << " of " << first_parse;
}

TEST(TreeSitterHighlighter, AStopEndsTheParseOfALargeText) {
    const Text text = text_of(nested_json(700));
    std::stop_source source;
    source.request_stop();
    const auto cancelled = TreeSitterHighlighter::create("json", text, source.get_token());
    ASSERT_FALSE(cancelled.has_value());
    EXPECT_EQ(cancelled.error().message, "cancelled");
    EXPECT_TRUE(TreeSitterHighlighter::create("json", text, std::stop_token{}).has_value());
}

TEST(TreeSitterHighlighter, AStopEndsTheReparseAndLeavesNoTree) {
    TextBuffer buffer(text_of(nested_json(700)));
    const auto highlighter = json_highlighter(buffer.snapshot());
    ASSERT_TRUE(highlighter);
    const auto change = buffer.insert(buffer.snapshot().size() / 2, "[");
    std::stop_source source;
    source.request_stop();
    // A reparse of one edit may finish before the parser looks at the flag; a failure leaves no
    // tree, and a success is a correct one.
    const auto result = highlighter->apply(std::span<const TextChange>(&*change, 1),
                                           buffer.snapshot(), source.get_token());
    if (!result) {
        EXPECT_TRUE(highlighter->spans(1).empty());
        EXPECT_FALSE(highlighter->apply(*change, buffer.snapshot()).has_value());
    } else {
        EXPECT_EQ(render(*highlighter, buffer.snapshot()), fresh_render(buffer.snapshot()));
    }
}

TEST(TreeSitterHighlighter, ASnapshotKeepsTheTextItWasTakenAt) {
    TextBuffer buffer(text_of("[1, true]\n[2]"));
    const auto highlighter = json_highlighter(buffer.snapshot());
    ASSERT_TRUE(highlighter);
    const auto snapshot = highlighter->snapshot();
    const std::string before = render(*snapshot, buffer.snapshot());
    EXPECT_EQ(before, render(*highlighter, buffer.snapshot()));
    EXPECT_EQ(snapshot->root_scope(), "source.json");

    const auto change = buffer.replace(1, 2, "\"changed\"");
    ASSERT_TRUE(highlighter->apply(*change, buffer.snapshot()).has_value());
    EXPECT_EQ(render(*snapshot, text_of("[1, true]\n[2]")), before);
    EXPECT_NE(render(*highlighter, buffer.snapshot()), before);
}

TEST(TreeSitterHighlighter, ASnapshotHasNoSpansOnALineOfMoreThanTheLimit) {
    std::string long_line = "[1";
    for (std::size_t i = 0; i < max_highlighted_line_units; ++i) {
        long_line += ",1";
    }
    const Text text = text_of(long_line + "]\n[2]\n");
    const auto highlighter = json_highlighter(text);
    ASSERT_TRUE(highlighter);
    EXPECT_FALSE(highlighter->spans(0).empty());
    const auto snapshot = highlighter->snapshot();
    EXPECT_TRUE(snapshot->spans(0).empty());
    EXPECT_EQ(snapshot->spans(1).size(), 1u);
}

TEST(TreeSitterHighlighter, ASnapshotCanBeReadWhileTheHighlighterGoesOnEditing) {
    TextBuffer buffer(text_of(nested_json(100)));
    const auto highlighter = json_highlighter(buffer.snapshot());
    ASSERT_TRUE(highlighter);
    const auto snapshot = highlighter->snapshot();
    const Text original = buffer.snapshot();
    const std::string expected = render(*snapshot, original);

    std::atomic<bool> done = false;
    // A second snapshot of the same tree, read on this thread's behalf while the first is.
    std::thread reader([&] {
        while (!done) {
            EXPECT_EQ(render(*snapshot, original), expected);
        }
    });
    for (int i = 0; i < 200; ++i) {
        const auto change = buffer.insert(buffer.snapshot().line_start(3) + 3, "x");
        ASSERT_TRUE(highlighter->apply(*change, buffer.snapshot()).has_value());
    }
    done = true;
    reader.join();
    EXPECT_EQ(render(*snapshot, original), expected);
}

#else

TEST(TreeSitterHighlighter, NeedsTheTreeSitterSources) {
    GTEST_SKIP() << "built without .deps/tree-sitter; run scripts/fetch-tree-sitter.sh";
}

#endif
