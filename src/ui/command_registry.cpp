#include "anvil/ui/command_registry.hpp"

#include <algorithm>

namespace anvil::ui {

void CommandRegistry::add(std::string id, std::string title, Handler handler) {
    if (auto existing = std::ranges::find(commands_, id, &Command::id);
        existing != commands_.end()) {
        existing->title = std::move(title);
        existing->handler = std::move(handler);
        return;
    }
    commands_.push_back({std::move(id), std::move(title), std::move(handler)});
}

const CommandRegistry::Command* CommandRegistry::find(std::string_view id) const {
    const auto found = std::ranges::find(commands_, id, &Command::id);
    return found == commands_.end() ? nullptr : &*found;
}

bool CommandRegistry::execute(std::string_view id, const nlohmann::json& args) const {
    const Command* command = find(id);
    if (!command) {
        return false;
    }
    // A handler may register commands, which can reallocate the list it was found in.
    const Handler handler = command->handler;
    handler(args);
    return true;
}

} // namespace anvil::ui
