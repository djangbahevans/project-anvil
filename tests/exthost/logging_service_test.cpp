#include "anvil/exthost/logging_service.hpp"
#include "anvil/exthost/vscode_types.hpp"
#include "fake_host.hpp"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <unistd.h>

using namespace anvil::exthost;
using anvil::exthost::test::FakeHost;
using nlohmann::json;
namespace core = anvil::core;
using core::LogLevel;

namespace {

class LoggingServiceTest : public ::testing::Test {
protected:
    LoggingServiceTest()
        : session_("{}"), host_(session_), logger_(std::make_shared<core::RecordingLogger>()),
          service_(logger_, router_) {
        session_.set_request_handler([this](const Request& r) { return router_.dispatch(r); });
        host_.complete_handshake();
    }

    // Sends a request and returns the reply as "<empty>", the JSON, or "<error>".
    std::string request(ProxyId service, std::string method, json args) {
        EXPECT_TRUE(host_
                        .send_rpc(Request{1, static_cast<std::uint8_t>(service), std::move(method),
                                          JsonArgs{args.dump()}})
                        .has_value());
        for (const auto& message : host_.drain_rpc()) {
            if (std::holds_alternative<EmptyReply>(message)) {
                return "<empty>";
            }
            if (const auto* reply = std::get_if<JsonReply>(&message)) {
                return reply->json;
            }
            if (std::holds_alternative<ErrorReply>(message)) {
                return "<error>";
            }
        }
        return "<none>";
    }

    json console(const std::string& severity, const json& arguments) {
        return {{"type", "__$console"}, {"severity", severity}, {"arguments", arguments.dump()}};
    }

    Session session_;
    FakeHost host_;
    RequestRouter router_;
    std::shared_ptr<core::RecordingLogger> logger_;
    LoggingService service_;
};

} // namespace

TEST_F(LoggingServiceTest, ConsoleOutputIsLoggedWithTheMatchingLevel) {
    request(ProxyId::MainThreadConsole, "$logExtensionHostMessage",
            json::array({console("log", json::array({"a"}))}));
    request(ProxyId::MainThreadConsole, "$logExtensionHostMessage",
            json::array({console("info", json::array({"b"}))}));
    request(ProxyId::MainThreadConsole, "$logExtensionHostMessage",
            json::array({console("warn", json::array({"c"}))}));
    request(ProxyId::MainThreadConsole, "$logExtensionHostMessage",
            json::array({console("error", json::array({"d"}))}));
    request(ProxyId::MainThreadConsole, "$logExtensionHostMessage",
            json::array({console("debug", json::array({"e"}))}));

    EXPECT_TRUE(logger_->contains(LogLevel::info, "a", "extension host"));
    EXPECT_TRUE(logger_->contains(LogLevel::info, "b"));
    EXPECT_TRUE(logger_->contains(LogLevel::warning, "c"));
    EXPECT_TRUE(logger_->contains(LogLevel::error, "d"));
    EXPECT_TRUE(logger_->contains(LogLevel::debug, "e"));
}

TEST_F(LoggingServiceTest, ConsoleArgumentsAreJoinedAndNonStringsAreSerialized) {
    request(ProxyId::MainThreadConsole, "$logExtensionHostMessage",
            json::array({console("log", json::array({"hello", json{{"a", 1}}, 42, true}))}));
    ASSERT_EQ(logger_->entries().size(), 1u);
    EXPECT_EQ(logger_->entries()[0].message, R"(hello {"a":1} 42 true)");
}

TEST_F(LoggingServiceTest, ConsoleStackTracesAreAppendedNotJoinedAsArguments) {
    request(ProxyId::MainThreadConsole, "$logExtensionHostMessage",
            json::array({console(
                "error", json::array({"failed", json{{"__$stack", "at line 1\nat line 2"}}}))}));
    EXPECT_EQ(logger_->entries()[0].message, "failed\nat line 1\nat line 2");
}

TEST_F(LoggingServiceTest, MalformedConsoleArgumentsAreLoggedAsTheyCame) {
    request(ProxyId::MainThreadConsole, "$logExtensionHostMessage",
            json::array({json{{"severity", "log"}, {"arguments", "not json"}}}));
    EXPECT_TRUE(logger_->contains(LogLevel::info, "not json"));
}

TEST_F(LoggingServiceTest, UnexpectedErrorsAreLoggedWithTheirStack) {
    request(ProxyId::MainThreadErrors, "$onUnexpectedError",
            json::array({json{{"$isError", true},
                              {"name", "TypeError"},
                              {"message", "x is not a function"},
                              {"stack", "TypeError: x is not a function\n    at f (a.js:1:1)"}}}));
    EXPECT_TRUE(logger_->contains(
        LogLevel::error, "TypeError: x is not a function\n    at f (a.js:1:1)", "extension host"));

    request(ProxyId::MainThreadErrors, "$onUnexpectedError",
            json::array({json{{"name", "Error"}, {"message", "no stack"}}}));
    EXPECT_TRUE(logger_->contains(LogLevel::error, "Error: no stack"));
    request(ProxyId::MainThreadErrors, "$onUnexpectedError", json::array({"plain string"}));
    EXPECT_TRUE(logger_->contains(LogLevel::error, "plain string"));
}

TEST_F(LoggingServiceTest, ExtensionErrorsAreAttributedToTheExtension) {
    const json id = {{"value", "pub.ext"}, {"_lower", "pub.ext"}};
    request(ProxyId::MainThreadExtensionService, "$onExtensionRuntimeError",
            json::array({id, json{{"name", "Error"}, {"message", "runtime"}}}));
    request(ProxyId::MainThreadExtensionService, "$onExtensionActivationError",
            json::array({id, json{{"name", "Error"}, {"message", "no start"}}, nullptr}));
    EXPECT_TRUE(logger_->contains(LogLevel::error, "runtime", "pub.ext"));
    EXPECT_TRUE(
        logger_->contains(LogLevel::error, "activation failed: Error: no start", "pub.ext"));
}

TEST_F(LoggingServiceTest, ActivationTimingIsLoggedAtDebugLevel) {
    const json id = {{"value", "pub.ext"}, {"_lower", "pub.ext"}};
    request(ProxyId::MainThreadExtensionService, "$onDidActivateExtension",
            json::array({id, 1.5, 2.5, 3.5, json::object()}));
    EXPECT_TRUE(logger_->contains(LogLevel::debug, "activated", "pub.ext"));
}

// --- output channels ---

class OutputChannelTest : public LoggingServiceTest {
protected:
    OutputChannelTest() {
        directory_ = std::filesystem::temp_directory_path() /
                     ("anvil-channel-test-" + std::to_string(::getpid()));
        std::filesystem::create_directories(directory_);
    }
    ~OutputChannelTest() override {
        std::filesystem::remove_all(directory_);
    }

    // Registers a channel backed by a file in the scratch directory; returns its id.
    std::string register_channel(const std::string& label) {
        const json file = uri_to_json(core::Uri::file(directory_ / (label + ".log")));
        return json::parse(request(ProxyId::MainThreadOutputService, "$register",
                                   json::array({label, file, nullptr, "pub.ext"})))
            .get<std::string>();
    }

    void append(const std::string& label, std::string_view text) {
        std::ofstream(directory_ / (label + ".log"), std::ios::app | std::ios::binary) << text;
    }

    std::string update(const std::string& id, int mode,
                       std::optional<std::uint64_t> till = std::nullopt) {
        json args = json::array({id, mode});
        if (till) {
            args.push_back(*till);
        }
        return request(ProxyId::MainThreadOutputService, "$update", args);
    }

    std::filesystem::path directory_;
};

TEST_F(OutputChannelTest, AppendedLinesAreLoggedUnderTheChannelLabel) {
    const std::string id = register_channel("My Channel");
    append("My Channel", "first line\nsecond line\n");
    EXPECT_EQ(update(id, 1), "<empty>");

    ASSERT_EQ(logger_->entries().size(), 2u);
    EXPECT_TRUE(logger_->contains(LogLevel::info, "first line", "My Channel"));
    EXPECT_TRUE(logger_->contains(LogLevel::info, "second line", "My Channel"));
}

TEST_F(OutputChannelTest, PollingLogsWhatWasWrittenWithoutAnyNotification) {
    register_channel("a");
    register_channel("b");
    append("a", "from a\n");
    append("b", "from b\n");
    service_.poll();
    EXPECT_TRUE(logger_->contains(LogLevel::info, "from a", "a"));
    EXPECT_TRUE(logger_->contains(LogLevel::info, "from b", "b"));

    service_.poll();
    EXPECT_EQ(logger_->entries().size(), 2u);
    append("a", "more\n");
    service_.poll();
    EXPECT_EQ(logger_->entries().size(), 3u);
}

TEST_F(OutputChannelTest, VisibleLoggersAreFollowedUnderTheirName) {
    const json file = uri_to_json(core::Uri::file(directory_ / "host.log"));
    request(ProxyId::MainThreadLogger, "$registerLogger",
            json::array({json{{"resource", file}, {"id", "exthost"}, {"name", "Extension Host"}}}));
    append("host", "2026-11-17 10:00:00.000 [info] Extension host started\n");
    service_.poll();
    EXPECT_TRUE(logger_->contains(LogLevel::info, "Extension host started", "Extension Host"));
}

TEST_F(OutputChannelTest, HiddenLoggersAreNotFollowedUntilAChannelRegistersTheirFile) {
    const json file = uri_to_json(core::Uri::file(directory_ / "plain.log"));
    request(ProxyId::MainThreadLogger, "$registerLogger",
            json::array({json{{"resource", file}, {"id", "backing"}, {"hidden", true}}}));
    append("plain", "text\n");
    service_.poll();
    EXPECT_TRUE(logger_->entries().empty());

    request(ProxyId::MainThreadOutputService, "$register",
            json::array({"Plain Channel", file, nullptr, "e"}));
    service_.poll();
    ASSERT_EQ(logger_->entries().size(), 1u);
    EXPECT_EQ(logger_->entries()[0].source, "Plain Channel");
}

TEST_F(OutputChannelTest, ALoggerAndItsChannelShareOneFileWithoutLoggingTwice) {
    const json file = uri_to_json(core::Uri::file(directory_ / "both.log"));
    request(ProxyId::MainThreadLogger, "$registerLogger",
            json::array({json{{"resource", file}, {"name", "Visible"}}}));
    request(ProxyId::MainThreadOutputService, "$register",
            json::array({"Renamed", file, nullptr, "e"}));
    append("both", "once\n");
    service_.poll();
    ASSERT_EQ(logger_->entries().size(), 1u);
    EXPECT_EQ(logger_->entries()[0].source, "Renamed");
}

TEST_F(OutputChannelTest, DeregisteredLoggersAreNoLongerFollowed) {
    const json file = uri_to_json(core::Uri::file(directory_ / "gone.log"));
    request(ProxyId::MainThreadLogger, "$registerLogger",
            json::array({json{{"resource", file}, {"name", "Gone"}}}));
    request(ProxyId::MainThreadLogger, "$deregisterLogger", json::array({file}));
    append("gone", "ignored\n");
    service_.poll();
    EXPECT_TRUE(logger_->entries().empty());
}

TEST_F(OutputChannelTest, PollingToleratesChannelFilesThatDoNotExistYet) {
    register_channel("late");
    service_.poll();
    EXPECT_TRUE(logger_->entries().empty());
    append("late", "now it exists\n");
    service_.poll();
    EXPECT_EQ(logger_->entries().size(), 1u);
}

TEST_F(OutputChannelTest, EachUpdateLogsOnlyWhatWasAppendedSinceTheLastOne) {
    const std::string id = register_channel("c");
    append("c", "one\n");
    update(id, 1);
    append("c", "two\n");
    update(id, 1);
    update(id, 1);
    ASSERT_EQ(logger_->entries().size(), 2u);
    EXPECT_EQ(logger_->entries()[1].message, "two");
}

TEST_F(OutputChannelTest, AnIncompleteLineWaitsForItsEnd) {
    const std::string id = register_channel("c");
    append("c", "half a li");
    update(id, 1);
    EXPECT_TRUE(logger_->entries().empty());
    append("c", "ne\r\nnext\n");
    update(id, 1);
    ASSERT_EQ(logger_->entries().size(), 2u);
    EXPECT_EQ(logger_->entries()[0].message, "half a line");
    EXPECT_EQ(logger_->entries()[1].message, "next");
}

TEST_F(OutputChannelTest, LogChannelLinesKeepTheirLevelAndLoseTheirTimestamp) {
    const std::string id = register_channel("Log Channel");
    append("Log Channel", "2026-11-17 10:12:13.456 [info] started\n"
                          "2026-11-17 10:12:14.001 [warning] careful\n"
                          "2026-11-17 10:12:15 [error] broke\n"
                          "2026-11-17 10:12:16.5 [trace] tiny\n"
                          "not a log line\n");
    update(id, 1);
    EXPECT_TRUE(logger_->contains(LogLevel::info, "started", "Log Channel"));
    EXPECT_TRUE(logger_->contains(LogLevel::warning, "careful", "Log Channel"));
    EXPECT_TRUE(logger_->contains(LogLevel::error, "broke", "Log Channel"));
    EXPECT_TRUE(logger_->contains(LogLevel::trace, "tiny", "Log Channel"));
    EXPECT_TRUE(logger_->contains(LogLevel::info, "not a log line", "Log Channel"));
    EXPECT_EQ(logger_->entries()[0].message, "started");
}

TEST_F(OutputChannelTest, ClearingOrReplacingSkipsTheTextBeforeThePoint) {
    const std::string id = register_channel("c");
    append("c", "old text\n");
    const std::uint64_t cleared_at = std::filesystem::file_size(directory_ / "c.log");
    update(id, 3, cleared_at); // OutputChannelUpdateMode.Clear
    append("c", "new text\n");
    update(id, 1);
    ASSERT_EQ(logger_->entries().size(), 1u);
    EXPECT_EQ(logger_->entries()[0].message, "new text");
}

TEST_F(OutputChannelTest, UnknownAndDisposedChannelsAreIgnored) {
    const std::string id = register_channel("c");
    append("c", "text\n");
    request(ProxyId::MainThreadOutputService, "$dispose", json::array({id}));
    EXPECT_EQ(update(id, 1), "<empty>");
    EXPECT_EQ(update("no-such-channel", 1), "<empty>");
    EXPECT_TRUE(logger_->entries().empty());
}

TEST_F(OutputChannelTest, ChannelsWithoutAFileAreRejected) {
    const json not_a_file = {{"$mid", 1}, {"scheme", "inmemory"}, {"path", "/x"}};
    EXPECT_EQ(request(ProxyId::MainThreadOutputService, "$register",
                      json::array({"c", not_a_file, nullptr, "e"})),
              "<error>");
}

TEST_F(LoggingServiceTest, BookkeepingAndTelemetryCallsAreAcceptedSilently) {
    EXPECT_EQ(request(ProxyId::MainThreadTelemetry, "$publicLog2",
                      json::array({"event", json::object()})),
              "<empty>");
    EXPECT_EQ(request(ProxyId::MainThreadExtensionService, "$setPerformanceMarks",
                      json::array({json::array()})),
              "<empty>");
    EXPECT_EQ(request(ProxyId::MainThreadLogger, "$flush", json::array({json::object()})),
              "<empty>");
    EXPECT_TRUE(logger_->entries().empty());
}

TEST(Logger, StreamLoggerFormatsAndFiltersByLevel) {
    std::ostringstream out;
    core::StreamLogger logger(out, LogLevel::warning);
    logger.log(LogLevel::info, "src", "quiet");
    logger.log(LogLevel::warning, "src", "loud");
    logger.log(LogLevel::error, "ext.id", "bad");
    EXPECT_EQ(out.str(), "[warning] src: loud\n[error] ext.id: bad\n");
}
