#pragma once

#include "anvil/core/key_chord.hpp"
#include "anvil/core/keybindings.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace anvil::core {

// What the dispatcher decides about a key press, and what the interface should show for a chord.
struct KeyDispatch {
    // The press belongs to the bindings: it must not reach the widgets.
    bool handled = false;
    // The command to run, with its arguments. Not set when the press only starts or continues a
    // chord, or is bound to nothing.
    std::optional<std::string> command;
    std::optional<nlohmann::json> args;

    enum class Status {
        // The message about a chord stays as it is.
        unchanged,
        // A chord is waiting for its next key: show "(`pending`) was pressed. Waiting for ... key
        // of
        // chord..." until it ends.
        waiting,
        // The chord ended and its message goes: the command ran, or the time was up.
        closed,
        // The chord ended with a key that continues no binding: show "The key combination
        // (`pending`, `key_label`) is not a command." for ten seconds.
        no_command,
    };
    Status status = Status::unchanged;
    // The labels of the presses of the chord, such as `Ctrl+K`, in order. For `no_command` they
    // are the presses before the key that continues nothing.
    std::vector<std::string> pending;
    std::string key_label;
};

// The state of the key press dispatch: the presses of a chord that is in progress, and when they
// were last extended. It decides what a press does, as `abstractKeybindingService.ts` does,
// and leaves the commands, the messages and the clock to the caller, so that it runs on values.
//
// A press that is a modifier alone is ignored. A press that waits for more keys, and one that is
// bound, are handled; one that is bound with `^` runs its command and goes on to the widgets as
// well. Without a chord in progress a press that is bound to nothing is not handled, and in the
// middle of a chord it ends the chord and is handled, so that a key such as Escape cancels a chord
// without reaching the editor. A chord that is not continued for five seconds ends by itself.
class KeyDispatcher {
public:
    using Time = std::chrono::steady_clock::time_point;
    static constexpr std::chrono::milliseconds chord_timeout{5000};

    // The table must outlive the dispatcher, or be replaced with `set_table` first.
    KeyDispatcher(const KeybindingTable& table, KeyPlatform platform)
        : table_(&table), platform_(platform) {}

    // A chord in progress continues with the bindings of the new table.
    void set_table(const KeybindingTable& table) {
        table_ = &table;
    }

    KeyDispatch dispatch(const KeyStep& press, const ContextKeys& context, Time now);

    // Ends a chord whose last press is more than `chord_timeout` old.
    KeyDispatch check_timeout(Time now);
    // Ends a chord, as when the window loses the focus.
    KeyDispatch cancel();

    bool in_chord() const {
        return !pending_.empty();
    }

private:
    KeyDispatch leave();

    const KeybindingTable* table_;
    KeyPlatform platform_;
    std::vector<KeyStep> pending_;
    Time last_press_;
};

} // namespace anvil::core
