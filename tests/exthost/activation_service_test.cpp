#include "anvil/exthost/activation_service.hpp"
#include "fake_host.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace anvil::exthost;
using anvil::exthost::test::FakeHost;
using nlohmann::json;

namespace {

class ActivationServiceTest : public ::testing::Test {
protected:
    ActivationServiceTest() : session_("{}"), host_(session_), service_(session_) {
        host_.complete_handshake();
    }

    // The events that the session has called `$activateByEvent` with, as "event/kind", in order.
    std::vector<std::string> sent() {
        std::vector<std::string> events;
        for (const RpcMessage& message : host_.drain_rpc()) {
            const auto* request = std::get_if<Request>(&message);
            if (request && request->method == "$activateByEvent") {
                EXPECT_EQ(request->proxy_id,
                          static_cast<std::uint8_t>(ProxyId::ExtHostExtensionService));
                const json args = json::parse(std::get<JsonArgs>(request->args).json);
                events.push_back(args[0].get<std::string>() + "/" + args[1].dump());
                requests_.push_back(request->req);
            }
        }
        return events;
    }

    Session::ReplyHandler record(const std::string& name) {
        return [this, name](CallResult result) {
            results_.push_back(name + (result ? ":ok" : ":" + result.error()));
        };
    }

    Session session_;
    FakeHost host_;
    ActivationService service_;
    std::vector<std::uint32_t> requests_;
    std::vector<std::string> results_;
};

} // namespace

TEST_F(ActivationServiceTest, EventsWaitUntilTheServiceIsReadyAndThenGoInTheOrderTheyWereFired) {
    service_.fire("onCommand:a", ActivationKind::normal, record("a"));
    service_.fire("onFileSystem:memfs", ActivationKind::immediate, record("b"));
    EXPECT_TRUE(sent().empty());

    service_.set_ready();
    EXPECT_EQ(sent(), (std::vector<std::string>{"onCommand:a/0", "onFileSystem:memfs/1"}));
    EXPECT_TRUE(results_.empty());
}

TEST_F(ActivationServiceTest, AReadyServiceSendsAtOnce) {
    service_.set_ready();
    service_.fire("onCommand:a", ActivationKind::normal, record("a"));
    EXPECT_EQ(sent(), (std::vector<std::string>{"onCommand:a/0"}));
}

TEST_F(ActivationServiceTest, TheCallerHearsWhenTheExtensionHostHasReplied) {
    service_.set_ready();
    service_.fire("onCommand:a", ActivationKind::normal, record("a"));
    service_.fire("onCommand:b", ActivationKind::normal, record("b"));
    sent();
    ASSERT_EQ(requests_.size(), 2u);

    ASSERT_TRUE(host_.send_rpc(EmptyReply{requests_[1]}).has_value());
    EXPECT_EQ(results_, (std::vector<std::string>{"b:ok"}));
    ASSERT_TRUE(host_.send_rpc(EmptyReply{requests_[0]}).has_value());
    EXPECT_EQ(results_, (std::vector<std::string>{"b:ok", "a:ok"}));
}

TEST_F(ActivationServiceTest, AFailedActivationReachesTheCaller) {
    service_.set_ready();
    service_.fire("onCommand:a", ActivationKind::normal, record("a"));
    sent();

    ASSERT_TRUE(host_.send_rpc(ErrorReply{requests_[0], R"({"message":"boom"})"}).has_value());
    ASSERT_EQ(results_.size(), 1u);
    EXPECT_NE(results_[0].find("boom"), std::string::npos) << results_[0];
}

TEST_F(ActivationServiceTest, EventsThatWaitFailWhenTheHostStops) {
    service_.fire("onCommand:a", ActivationKind::normal, record("a"));
    service_.fail_queued("gone");
    EXPECT_EQ(results_, (std::vector<std::string>{"a:gone"}));

    service_.set_ready();
    EXPECT_TRUE(sent().empty()) << "what failed is not sent later";
}
