#include "anvil/exthost/request_router.hpp"

#include <gtest/gtest.h>

using namespace anvil::exthost;
using nlohmann::json;

namespace {

Request json_request(ProxyId service, std::string method, std::string args) {
    return Request{1, static_cast<std::uint8_t>(service), std::move(method),
                   JsonArgs{std::move(args)}};
}

} // namespace

TEST(ParseArguments, ParsesJsonArgumentArrays) {
    const auto args =
        parse_arguments(json_request(ProxyId::MainThreadCommands, "$x", R"(["a",1,{"k":null}])"));
    ASSERT_TRUE(args.has_value());
    EXPECT_EQ(*args, json::parse(R"(["a",1,{"k":null}])"));
}

TEST(ParseArguments, ParsesMixedArgumentsAndTurnsUndefinedIntoNull) {
    Request request{
        1, 1, "$x",
        MixedArgs{JsonArg{"\"text\""}, UndefinedArg{}, ObjectWithBuffersArg{"{\"a\":1}", {}}}};
    const auto args = parse_arguments(request);
    ASSERT_TRUE(args.has_value());
    EXPECT_EQ(*args, json::parse(R"(["text",null,{"a":1}])"));
}

TEST(ParseArguments, ABufferArgumentIsNullAndAnObjectWithBuffersKeepsItsPlaceholders) {
    Request request{
        1, 1, "$x",
        MixedArgs{JsonArg{"\"path\""}, BufferArg{anvil::to_bytes("bytes")},
                  ObjectWithBuffersArg{R"({"a":{"$$ref$$":1},"b":{"$$ref$$":0}})",
                                       {anvil::to_bytes("first"), anvil::to_bytes("second")}},
                  UndefinedArg{}}};
    const auto args = parse_arguments(request);
    ASSERT_TRUE(args.has_value());
    EXPECT_EQ(*args, json::parse(R"(["path",null,{"a":{"$$ref$$":1},"b":{"$$ref$$":0}},null])"));
}

TEST(BufferArgument, GivesTheBytesOfABufferAndNothingForAnythingElse) {
    Request request{1, 1, "$x",
                    MixedArgs{JsonArg{"1"}, BufferArg{anvil::to_bytes("bytes")},
                              ObjectWithBuffersArg{"{}", {anvil::to_bytes("o")}}, UndefinedArg{},
                              BufferArg{{}}}};
    EXPECT_EQ(buffer_argument(request, 0), nullptr);
    ASSERT_NE(buffer_argument(request, 1), nullptr);
    EXPECT_EQ(anvil::as_text(*buffer_argument(request, 1)), "bytes");
    EXPECT_EQ(buffer_argument(request, 2), nullptr);
    EXPECT_EQ(buffer_argument(request, 3), nullptr);
    ASSERT_NE(buffer_argument(request, 4), nullptr);
    EXPECT_TRUE(buffer_argument(request, 4)->empty());
    EXPECT_EQ(buffer_argument(request, 5), nullptr);

    const Request json_only = json_request(ProxyId::MainThreadCommands, "$x", "[1]");
    EXPECT_EQ(buffer_argument(json_only, 0), nullptr);
}

TEST(ObjectBuffers, GivesTheBuffersOfAnObjectInTheOrderOfTheirPlaceholders) {
    Request request{
        1, 1, "$x",
        MixedArgs{BufferArg{anvil::to_bytes("lone")},
                  ObjectWithBuffersArg{"{}", {anvil::to_bytes("a"), anvil::to_bytes("bc")}},
                  ObjectWithBuffersArg{"{}", {}}}};
    EXPECT_TRUE(object_buffers(request, 0).empty());
    const auto buffers = object_buffers(request, 1);
    ASSERT_EQ(buffers.size(), 2U);
    EXPECT_EQ(anvil::as_text(buffers[0]), "a");
    EXPECT_EQ(anvil::as_text(buffers[1]), "bc");
    EXPECT_TRUE(object_buffers(request, 2).empty());
    EXPECT_TRUE(object_buffers(request, 3).empty());
}

TEST(ParseArguments, RejectsInvalidJson) {
    EXPECT_FALSE(
        parse_arguments(json_request(ProxyId::MainThreadCommands, "$x", "[1,")).has_value());
}

TEST(ErrorJson, HasTheShapeTheExtensionHostRevives) {
    EXPECT_EQ(json::parse(error_json("boom")),
              json::parse(R"({"$isError":true,"name":"Error","message":"boom"})"));
}

TEST(RequestRouter, DispatchesToTheHandlerForServiceAndMethod) {
    RequestRouter router;
    json seen;
    router.on(ProxyId::MainThreadCommands, "$registerCommand",
              [&](const Request&, const json& args) -> std::optional<HandlerResult> {
                  seen = args;
                  return OkJson{"true"};
              });
    router.on(
        ProxyId::MainThreadWindow, "$registerCommand",
        [](const Request&, const json&) -> std::optional<HandlerResult> { return Failed{"{}"}; });

    const auto result =
        router.dispatch(json_request(ProxyId::MainThreadCommands, "$registerCommand", R"(["id"])"));
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(std::holds_alternative<OkJson>(*result));
    EXPECT_EQ(std::get<OkJson>(*result).json, "true");
    EXPECT_EQ(seen, json::parse(R"(["id"])"));
}

TEST(RequestRouter, AnswersUnhandledRequestsEmptyAndRemembersThem) {
    RequestRouter router;
    const auto result =
        router.dispatch(json_request(ProxyId::MainThreadTask, "$registerTaskSystem", "[]"));
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(std::holds_alternative<Ok>(*result));
    EXPECT_EQ(router.unhandled(), (std::set<std::string>{"MainThreadTask.$registerTaskSystem"}));
}

TEST(RequestRouter, FailsRequestsWithUnparsableArguments) {
    RequestRouter router;
    router.on(ProxyId::MainThreadCommands, "$x",
              [](const Request&, const json&) -> std::optional<HandlerResult> { return Ok{}; });
    const auto result = router.dispatch(json_request(ProxyId::MainThreadCommands, "$x", "["));
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(std::holds_alternative<Failed>(*result));
}

TEST(RequestRouter, TurnsMalformedArgumentShapesIntoErrorReplies) {
    RequestRouter router;
    router.on(ProxyId::MainThreadCommands, "$x",
              [](const Request&, const json& args) -> std::optional<HandlerResult> {
                  return OkJson{args.at(0).get<std::string>()};
              });
    const auto result = router.dispatch(json_request(ProxyId::MainThreadCommands, "$x", "[42]"));
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(std::holds_alternative<Failed>(*result));
    EXPECT_NE(std::get<Failed>(*result).error_json.find("malformed arguments"), std::string::npos);
}
