#include "anvil/exthost/vscode_types.hpp"

#include <gtest/gtest.h>

using namespace anvil::exthost;
using nlohmann::json;
namespace core = anvil::core;

TEST(VsCodeTypes, UriRoundTripsThroughJson) {
    const core::Uri uri{"https", "example.com", "/a/b", "q=1", "frag"};
    const auto back = uri_from_json(uri_to_json(uri));
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(*back, uri);
}

TEST(VsCodeTypes, UriJsonOmitsEmptyOptionalComponents) {
    const json encoded = uri_to_json(core::Uri::file("/a"));
    EXPECT_EQ(encoded, json::parse(R"({"$mid":1,"scheme":"file","path":"/a"})"));
}

TEST(VsCodeTypes, UriFromJsonAcceptsExtraFieldsAndRejectsMissingSchemes) {
    const auto uri = uri_from_json(json::parse(
        R"({"$mid":1,"scheme":"file","path":"/a","fsPath":"/a","external":"file:///a"})"));
    ASSERT_TRUE(uri.has_value());
    EXPECT_EQ(uri->path, "/a");
    EXPECT_FALSE(uri_from_json(json::parse(R"({"path":"/a"})")).has_value());
    EXPECT_FALSE(uri_from_json(json::parse("[]")).has_value());
}

TEST(VsCodeTypes, RangesAreOneBased) {
    const core::Range range{{0, 0}, {2, 5}};
    const json encoded = range_to_json(range);
    EXPECT_EQ(
        encoded,
        json::parse(R"({"startLineNumber":1,"startColumn":1,"endLineNumber":3,"endColumn":6})"));
    const auto back = range_from_json(encoded);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(*back, range);
}

TEST(VsCodeTypes, RangeFromJsonRejectsMalformedAndZeroBasedValues) {
    EXPECT_FALSE(range_from_json(json::parse(R"({"startLineNumber":1})")).has_value());
    EXPECT_FALSE(
        range_from_json(
            json::parse(R"({"startLineNumber":0,"startColumn":1,"endLineNumber":1,"endColumn":1})"))
            .has_value());
    EXPECT_FALSE(
        range_from_json(
            json::parse(
                R"({"startLineNumber":"1","startColumn":1,"endLineNumber":1,"endColumn":1})"))
            .has_value());
}

TEST(VsCodeTypes, SelectionsConvertBetweenOffsetsAndPositions) {
    const core::Text text = *core::Text::from_utf8("ab\ncd");
    const core::Selection selection{4, 1}; // active end is before the anchor
    const json encoded = selection_to_json(selection, text);
    EXPECT_EQ(
        encoded,
        json::parse(
            R"({"selectionStartLineNumber":2,"selectionStartColumn":2,"positionLineNumber":1,"positionColumn":2})"));
    const auto back = selection_from_json(encoded, text);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(*back, selection);
}

TEST(VsCodeTypes, SelectionColumnsCountUtf16Units) {
    const core::Text text = *core::Text::from_utf8("a\xF0\x9F\x98\x80z");
    const json encoded = selection_to_json({5, 5}, text);
    EXPECT_EQ(encoded["positionColumn"], 4); // a=1, the pair=2, so 'z' is column 4
}
