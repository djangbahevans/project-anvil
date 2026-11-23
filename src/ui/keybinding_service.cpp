#include "anvil/ui/keybinding_service.hpp"

#include "anvil/ui/key_translation.hpp"

#include <QApplication>
#include <QKeyEvent>
#include <QWidget>

namespace anvil::ui {
namespace {

core::ContextValue context_value(const nlohmann::json& json) {
    if (json.is_boolean()) {
        return json.get<bool>();
    }
    if (json.is_number()) {
        return json.get<double>();
    }
    if (json.is_string()) {
        return json.get<std::string>();
    }
    std::vector<std::string> items;
    if (json.is_array()) {
        for (const nlohmann::json& item : json) {
            items.push_back(item.is_string() ? item.get<std::string>() : item.dump());
        }
    }
    return items;
}

} // namespace

KeybindingService::KeybindingService(Hooks hooks, core::KeyPlatform platform, QObject* parent)
    : QObject(parent), hooks_(std::move(hooks)), platform_(platform),
      dispatcher_(table_, platform) {
    timeout_.setInterval(500);
    connect(&timeout_, &QTimer::timeout, this, [this] { check_timeout(); });
    connect(qApp, &QGuiApplication::applicationStateChanged, this,
            [this](Qt::ApplicationState state) {
                if (state != Qt::ApplicationActive) {
                    apply(dispatcher_.cancel());
                }
            });
}

void KeybindingService::set_bindings(std::vector<core::Keybinding> bindings) {
    apply(dispatcher_.cancel());
    table_ = core::KeybindingTable(std::move(bindings));
    dispatcher_.set_table(table_);
}

void KeybindingService::set_extension_context(const std::string& key, const nlohmann::json& value) {
    if (value.is_null()) {
        extension_context_.erase(key);
    } else {
        extension_context_[key] = value;
    }
}

core::ContextKeys KeybindingService::context() const {
    core::ContextKeys keys;
    for (const auto& [key, value] : extension_context_) {
        keys.set(key, context_value(value));
    }
    if (hooks_.fill_context) {
        hooks_.fill_context(keys);
    }
    return keys;
}

QString KeybindingService::shortcut_label(const std::string& command) const {
    const core::Keybinding* binding = table_.lookup_primary(command, context());
    if (!binding) {
        return {};
    }
    return QString::fromStdString(core::key_chord_label(binding->chord, platform_));
}

void KeybindingService::apply(const core::KeyDispatch& result) {
    using Status = core::KeyDispatch::Status;
    const auto join = [](const std::vector<std::string>& labels) {
        QStringList parts;
        for (const std::string& label : labels) {
            parts << QString::fromStdString(label);
        }
        return parts.join(QStringLiteral(", "));
    };
    const auto close_message = [&] {
        if (!shown_message_.isEmpty()) {
            emit chord_message({}, 0);
            shown_message_.clear();
        }
    };

    switch (result.status) {
    case Status::unchanged:
        break;
    case Status::waiting:
        shown_message_ = result.pending.size() == 1
                             ? tr("(%1) was pressed. Waiting for second key of chord...")
                                   .arg(join(result.pending))
                             : tr("(%1) was pressed. Waiting for next key of chord...")
                                   .arg(join(result.pending));
        emit chord_message(shown_message_, 0);
        timeout_.start();
        break;
    case Status::closed:
        timeout_.stop();
        close_message();
        break;
    case Status::no_command:
        timeout_.stop();
        close_message();
        emit chord_message(tr("The key combination (%1, %2) is not a command.")
                               .arg(join(result.pending), QString::fromStdString(result.key_label)),
                           10000);
        break;
    }

    if (result.command && hooks_.run) {
        hooks_.run(*result.command, result.args ? &*result.args : nullptr);
    }
}

void KeybindingService::check_timeout(core::KeyDispatcher::Time now) {
    apply(dispatcher_.check_timeout(now));
    if (!dispatcher_.in_chord()) {
        timeout_.stop();
    }
}

bool KeybindingService::handle_press(const QKeyEvent& event) {
    const auto step = key_step_from_event(event, platform_);
    if (!step) {
        return false;
    }
    const core::KeyDispatch result =
        dispatcher_.dispatch(*step, context(), std::chrono::steady_clock::now());
    apply(result);
    return result.handled;
}

bool KeybindingService::eventFilter(QObject* watched, QEvent* event) {
    const QEvent::Type type = event->type();
    if (type != QEvent::KeyPress && type != QEvent::ShortcutOverride) {
        return false;
    }
    if (!qobject_cast<QWidget*>(watched) || QApplication::activeModalWidget() ||
        (hooks_.composing && hooks_.composing())) {
        return false;
    }
    const auto& key_event = static_cast<const QKeyEvent&>(*event);
    if (type == QEvent::ShortcutOverride) {
        // A key that is bound must not trigger the shortcuts of the toolkit; the press that
        // follows is the one that runs the command.
        const auto step = key_step_from_event(key_event, platform_);
        if (step && dispatcher_.would_handle(*step, context())) {
            event->accept();
        }
        return false;
    }
    return handle_press(key_event);
}

} // namespace anvil::ui
