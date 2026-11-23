#include "anvil/core/key_dispatcher.hpp"

namespace anvil::core {
namespace {

bool is_modifier_key(Key key) {
    return key == Key::ctrl || key == Key::shift || key == Key::alt || key == Key::meta;
}

} // namespace

KeyDispatch KeyDispatcher::leave() {
    pending_.clear();
    KeyDispatch result;
    result.status = KeyDispatch::Status::closed;
    return result;
}

KeyDispatch KeyDispatcher::dispatch(const KeyStep& press, const ContextKeys& context, Time now) {
    KeyDispatch result;
    if (is_modifier_key(press.key)) {
        return result;
    }

    const auto label = [&](const KeyStep& step) {
        return key_chord_label(KeyChord{{step}}, platform_);
    };
    const Resolution resolution = table_->resolve(pending_, press, context);

    const auto no_command = [&] {
        result.status = KeyDispatch::Status::no_command;
        result.key_label = label(press);
        for (const KeyStep& step : pending_) {
            result.pending.push_back(label(step));
        }
        pending_.clear();
        result.handled = true;
    };

    switch (resolution.kind) {
    case Resolution::Kind::none:
        if (in_chord()) {
            no_command();
        }
        return result;
    case Resolution::Kind::more_keys_needed:
        pending_.push_back(press);
        last_press_ = now;
        result.handled = true;
        result.status = KeyDispatch::Status::waiting;
        for (const KeyStep& step : pending_) {
            result.pending.push_back(label(step));
        }
        return result;
    case Resolution::Kind::found:
        if (resolution.command.empty()) {
            if (in_chord()) {
                no_command();
            }
            return result;
        }
        if (in_chord()) {
            pending_.clear();
            result.status = KeyDispatch::Status::closed;
        }
        result.handled = !resolution.bubble;
        result.command = resolution.command;
        result.args = resolution.args;
        return result;
    }
    return result;
}

KeyDispatch KeyDispatcher::check_timeout(Time now) {
    if (in_chord() && now - last_press_ > chord_timeout) {
        return leave();
    }
    return {};
}

KeyDispatch KeyDispatcher::cancel() {
    if (in_chord()) {
        return leave();
    }
    return {};
}

} // namespace anvil::core
