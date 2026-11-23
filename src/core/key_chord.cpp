#include "anvil/core/key_chord.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <iterator>
#include <unordered_map>

namespace anvil::core {
namespace {

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

std::string lower(std::string_view text) {
    std::string result(text);
    for (char& c : result) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return result;
}

const std::unordered_map<std::string, Key>& names() {
    static const std::unordered_map<std::string, Key> map = [] {
        std::unordered_map<std::string, Key> result;
        for (const KeyInfo& info : key_table) {
            result.emplace(std::string(info.setting), info.key);
            if (!info.alias.empty()) {
                result.emplace(std::string(info.alias), info.key);
            }
        }
        return result;
    }();
    return map;
}

struct ModifierName {
    std::string_view name;
    bool Modifiers::*flag;
};

constexpr ModifierName modifier_names[] = {
    {"ctrl", &Modifiers::ctrl},  {"shift", &Modifiers::shift}, {"alt", &Modifiers::alt},
    {"meta", &Modifiers::meta},  {"win", &Modifiers::meta},    {"cmd", &Modifiers::meta},
    {"super", &Modifiers::meta},
};

std::unexpected<Error> fail(std::size_t position, const std::string& what) {
    return std::unexpected(
        Error{ErrorCode::invalid_argument,
              "invalid key binding at " + std::to_string(position) + ": " + what});
}

// What the key is when it is a modifier, which the same modifier held makes redundant.
bool Modifiers::*modifier_of(Key key) {
    switch (key) {
    case Key::ctrl:
        return &Modifiers::ctrl;
    case Key::shift:
        return &Modifiers::shift;
    case Key::alt:
        return &Modifiers::alt;
    case Key::meta:
        return &Modifiers::meta;
    default:
        return nullptr;
    }
}

} // namespace

const KeyInfo& key_info(Key key) {
    return key_table[static_cast<std::size_t>(key)];
}

std::optional<Key> key_from_name(std::string_view name) {
    const auto found = names().find(lower(name));
    if (found == names().end()) {
        return std::nullopt;
    }
    return found->second;
}

std::expected<KeyChord, Error> parse_key_chord(std::string_view text) {
    KeyChord chord;
    std::size_t position = 0;
    while (true) {
        while (position < text.size() && is_space(text[position])) {
            ++position;
        }
        if (position == text.size()) {
            break;
        }
        const std::size_t start = position;
        while (position < text.size() && !is_space(text[position])) {
            ++position;
        }
        const std::string token = lower(text.substr(start, position - start));

        if (chord.steps.size() == KeyChord::max_steps) {
            return fail(start, "a key binding has at most " + std::to_string(KeyChord::max_steps) +
                                   " steps");
        }

        KeyStep step;
        std::size_t at = 0;
        for (bool found = true; found;) {
            found = false;
            for (const ModifierName& modifier : modifier_names) {
                const std::string_view rest = std::string_view(token).substr(at);
                if (rest.size() > modifier.name.size() && rest.starts_with(modifier.name) &&
                    (rest[modifier.name.size()] == '+' || rest[modifier.name.size()] == '-')) {
                    step.modifiers.*modifier.flag = true;
                    at += modifier.name.size() + 1;
                    found = true;
                    break;
                }
            }
        }
        const std::string_view key_text = std::string_view(token).substr(at);
        if (key_text.empty()) {
            return fail(start + at, "a modifier is not followed by a key");
        }
        const auto key = key_from_name(key_text);
        if (!key) {
            const std::size_t separator = key_text.find_first_of("+-", 1);
            if (separator != std::string_view::npos) {
                return fail(start + at, "unknown modifier '" +
                                            std::string(key_text.substr(0, separator)) + "'");
            }
            return fail(start + at, "unknown key '" + std::string(key_text) + "'");
        }
        step.key = *key;
        if (bool Modifiers::*own = modifier_of(step.key)) {
            step.modifiers.*own = false;
        }
        chord.steps.push_back(step);
    }
    if (chord.steps.empty()) {
        return fail(0, "there is no key");
    }
    return chord;
}

namespace {

std::string_view meta_name(KeyPlatform platform) {
    switch (platform) {
    case KeyPlatform::macos:
        return "cmd";
    case KeyPlatform::windows:
        return "win";
    case KeyPlatform::linux:
        break;
    }
    return "meta";
}

} // namespace

std::string format_key_chord(const KeyChord& chord, KeyPlatform platform) {
    std::string result;
    for (const KeyStep& step : chord.steps) {
        if (!result.empty()) {
            result += ' ';
        }
        if (step.modifiers.ctrl) {
            result += "ctrl+";
        }
        if (step.modifiers.shift) {
            result += "shift+";
        }
        if (step.modifiers.alt) {
            result += "alt+";
        }
        if (step.modifiers.meta) {
            result += meta_name(platform);
            result += '+';
        }
        result += key_info(step.key).setting;
    }
    return result;
}

std::string key_chord_label(const KeyChord& chord, KeyPlatform platform) {
    const bool mac = platform == KeyPlatform::macos;
    std::string result;
    for (const KeyStep& step : chord.steps) {
        if (!result.empty()) {
            result += ' ';
        }
        std::string label;
        const auto add = [&](bool held, std::string_view mac_label, std::string_view name) {
            if (held) {
                label += mac ? mac_label : name;
                label += mac ? "" : "+";
            }
        };
        add(step.modifiers.ctrl, "⌃", "Ctrl");
        add(step.modifiers.shift, "⇧", "Shift");
        add(step.modifiers.alt, "⌥", "Alt");
        add(step.modifiers.meta, "⌘", platform == KeyPlatform::windows ? "Windows" : "Super");
        if (mac) {
            switch (step.key) {
            case Key::left_arrow:
                label += "←";
                break;
            case Key::up_arrow:
                label += "↑";
                break;
            case Key::right_arrow:
                label += "→";
                break;
            case Key::down_arrow:
                label += "↓";
                break;
            default:
                label += key_info(step.key).label;
                break;
            }
        } else {
            label += key_info(step.key).label;
        }
        result += label;
    }
    return result;
}

} // namespace anvil::core
