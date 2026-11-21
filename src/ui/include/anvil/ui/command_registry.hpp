#pragma once

#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace anvil::ui {

// The editor's own commands, keyed by VS Code's command ids. Menus, key bindings, the command
// palette and extensions (through CommandsService) all run a command by its id, so an operation
// has one entry point however it is reached.
class CommandRegistry {
public:
    // Receives the JSON array of arguments the caller supplied; most commands ignore it.
    using Handler = std::function<void(const nlohmann::json& args)>;

    struct Command {
        std::string id;
        // The label shown in the command palette.
        std::string title;
        Handler handler;
    };

    // Adds a command, or replaces the title and handler of the one with the same id while keeping
    // its position.
    void add(std::string id, std::string title, Handler handler);

    const Command* find(std::string_view id) const;

    // Runs the command; false when no command has the id.
    bool execute(std::string_view id, const nlohmann::json& args = nlohmann::json::array()) const;

    // In registration order.
    const std::vector<Command>& commands() const {
        return commands_;
    }

private:
    std::vector<Command> commands_;
};

} // namespace anvil::ui
