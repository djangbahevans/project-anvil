#include "anvil/exthost/activation_service.hpp"

#include <nlohmann/json.hpp>

namespace anvil::exthost {

void ActivationService::fire(std::string event, ActivationKind kind, Session::ReplyHandler done) {
    Queued queued{std::move(event), kind, std::move(done)};
    if (ready_) {
        send(std::move(queued));
    } else {
        queue_.push_back(std::move(queued));
    }
}

void ActivationService::set_ready() {
    ready_ = true;
    std::deque<Queued> waiting = std::move(queue_);
    queue_.clear();
    for (Queued& queued : waiting) {
        send(std::move(queued));
    }
}

void ActivationService::fail_queued(const std::string& reason) {
    std::deque<Queued> waiting = std::move(queue_);
    queue_.clear();
    for (Queued& queued : waiting) {
        queued.done(std::unexpected(reason));
    }
}

void ActivationService::send(Queued queued) {
    const nlohmann::json args =
        nlohmann::json::array({queued.event, static_cast<int>(queued.kind)});
    session_.call(ProxyId::ExtHostExtensionService, "$activateByEvent", args.dump(),
                  std::move(queued.done));
}

} // namespace anvil::exthost
