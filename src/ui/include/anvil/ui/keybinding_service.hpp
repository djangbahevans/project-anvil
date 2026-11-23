#pragma once

#include "anvil/core/key_dispatcher.hpp"
#include "anvil/core/keybindings.hpp"

#include <QKeyEvent>
#include <QObject>
#include <QString>
#include <QTimer>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace anvil::ui {

// Turns key presses into commands: it watches the key events of the whole application before the
// widgets see them, translates each into a step of a key binding, and lets the dispatcher decide
// what it does. A press that is bound is swallowed, and its command runs; a press that is not goes
// on to the widget as before, so typing is untouched. A chord in progress is shown to the user
// through `chord_message`.
//
// A press is left alone while a modal dialog is open, and while an input method is composing text
// in the editor, which owns the keys it is using then.
class KeybindingService : public QObject {
    Q_OBJECT

public:
    struct Hooks {
        // Sets the context keys that the `when` of a binding reads, for a press that has arrived.
        std::function<void(core::ContextKeys&)> fill_context;
        // Runs a command with its arguments.
        std::function<void(const std::string& command, const nlohmann::json* args)> run;
        // Whether an input method is composing text in the widget that has the focus.
        std::function<bool()> composing;
    };

    KeybindingService(Hooks hooks, core::KeyPlatform platform, QObject* parent = nullptr);

    // Replaces the bindings. A chord in progress is dropped.
    void set_bindings(std::vector<core::Keybinding> bindings);
    const core::KeybindingTable& table() const {
        return table_;
    }
    core::KeyPlatform platform() const {
        return platform_;
    }

    // The keys that an extension sets with the `setContext` command, which the context of every
    // press has. Setting a value of `null` removes the key.
    void set_extension_context(const std::string& key, const nlohmann::json& value);

    // The context keys as a press sees them now.
    core::ContextKeys context() const;

    // How a menu shows the primary key binding of a command, such as `Ctrl+S`, or nothing.
    QString shortcut_label(const std::string& command) const;

    // Dispatches a press as the event filter does, for the tests of what the user sees.
    bool handle_press(const QKeyEvent& event);

    // Ends a chord that was last extended more than five seconds before `now`.
    void check_timeout(core::KeyDispatcher::Time now = std::chrono::steady_clock::now());

    bool eventFilter(QObject* watched, QEvent* event) override;

signals:
    // What to tell the user about a chord in progress: the text, and for how long in milliseconds
    // (zero until the next message). An empty text takes the message away.
    void chord_message(const QString& text, int duration_ms);

private:
    void apply(const core::KeyDispatch& result);

    Hooks hooks_;
    core::KeyPlatform platform_;
    core::KeybindingTable table_;
    core::KeyDispatcher dispatcher_;
    std::map<std::string, nlohmann::json> extension_context_;
    QTimer timeout_;
    // The message of the chord in progress, so that it is taken away only if it is still shown.
    QString shown_message_;
};

} // namespace anvil::ui
