#include "anvil/core/keybindings.hpp"

#include <algorithm>

namespace anvil::core {
namespace {

bool when_holds(const Keybinding& binding, const ContextKeys& context) {
    return !binding.when || binding.when->evaluate(context);
}

bool is_always_true(const std::optional<WhenExpression>& when) {
    return !when || when->canonical() == "true";
}

// Whether `a` provably implies `b`, with a missing expression meaning always.
bool when_is_included(const std::optional<WhenExpression>& a,
                      const std::optional<WhenExpression>& b) {
    if (is_always_true(b)) {
        return true;
    }
    if (is_always_true(a)) {
        return false;
    }
    return a->implies(*b);
}

// The command without the mark that lets the key press go on.
std::string_view command_of(const Keybinding& binding) {
    return std::string_view(binding.command).substr(binding.command.starts_with('^') ? 1 : 0);
}

bool is_removal(const Keybinding& binding) {
    return binding.command.starts_with('-');
}

// Whether the removal takes away the binding: its key is the start of the binding's key, and the
// `when` of the binding implies that of the removal.
bool removes(const Keybinding& removal, const Keybinding& binding) {
    const auto& removed = removal.chord.steps;
    const auto& steps = binding.chord.steps;
    if (removed.size() > steps.size() ||
        !std::ranges::equal(removed, std::span(steps).first(removed.size()))) {
        return false;
    }
    if (!is_always_true(removal.when)) {
        // A removal with a `when` that is not always true takes away only bindings that have one.
        return binding.when && when_is_included(binding.when, removal.when);
    }
    return true;
}

} // namespace

KeybindingTable::KeybindingTable(std::vector<Keybinding> bindings) {
    std::ranges::stable_sort(bindings, {}, &Keybinding::source);

    std::vector<Keybinding> kept;
    std::vector<Keybinding> removals;
    for (Keybinding& binding : bindings) {
        if (is_removal(binding)) {
            binding.command.erase(0, 1);
            removals.push_back(std::move(binding));
        } else {
            kept.push_back(std::move(binding));
        }
    }
    std::erase_if(kept, [&](const Keybinding& binding) {
        return binding.source != KeybindingSource::user &&
               std::ranges::any_of(removals, [&](const Keybinding& removal) {
                   return removal.command == command_of(binding) && removes(removal, binding);
               });
    });

    for (Keybinding& binding : kept) {
        if (binding.command.starts_with('^')) {
            binding.command.erase(0, 1);
            binding.bubble = true;
        }
        if (binding.chord.steps.empty() || (binding.when && binding.when->is_false())) {
            continue;
        }
        const std::size_t position = bindings_.size();
        by_first_step_[binding.chord.steps.front()].push_back(position);
        bindings_.push_back(std::move(binding));

        const Keybinding& added = bindings_.back();
        if (added.command.empty()) {
            continue;
        }
        // Earlier bindings of the same key that the new one covers completely stop being shown
        // for their commands.
        for (const std::size_t other : by_first_step_[added.chord.steps.front()]) {
            if (other == position) {
                continue;
            }
            const Keybinding& conflict = bindings_[other];
            if (conflict.command == added.command || conflict.command.empty()) {
                continue;
            }
            const std::size_t shared =
                std::min(conflict.chord.steps.size(), added.chord.steps.size());
            if (!std::ranges::equal(std::span(conflict.chord.steps).first(shared),
                                    std::span(added.chord.steps).first(shared))) {
                continue;
            }
            if (when_is_included(conflict.when, added.when)) {
                auto& shown = by_command_[conflict.command];
                std::erase(shown, other);
            }
        }
        by_command_[added.command].push_back(position);
    }
}

Resolution KeybindingTable::resolve(std::span<const KeyStep> pending, const KeyStep& next,
                                    const ContextKeys& context) const {
    std::vector<KeyStep> pressed(pending.begin(), pending.end());
    pressed.push_back(next);

    const auto candidates = by_first_step_.find(pressed.front());
    if (candidates == by_first_step_.end()) {
        return {};
    }
    const Keybinding* winner = nullptr;
    for (auto position = candidates->second.rbegin(); position != candidates->second.rend();
         ++position) {
        const Keybinding& candidate = bindings_[*position];
        const auto& steps = candidate.chord.steps;
        if (pressed.size() > steps.size() ||
            !std::ranges::equal(std::span(pressed).subspan(1),
                                std::span(steps).subspan(1, pressed.size() - 1))) {
            continue;
        }
        if (when_holds(candidate, context)) {
            winner = &candidate;
            break;
        }
    }
    if (!winner) {
        return {};
    }
    if (pressed.size() < winner->chord.steps.size()) {
        Resolution waiting;
        waiting.kind = Resolution::Kind::more_keys_needed;
        return waiting;
    }
    return {.kind = Resolution::Kind::found,
            .command = winner->command,
            .args = winner->args,
            .bubble = winner->bubble};
}

std::vector<const Keybinding*> KeybindingTable::lookup(std::string_view command) const {
    std::vector<const Keybinding*> result;
    const auto found = by_command_.find(command);
    if (found == by_command_.end()) {
        return result;
    }
    for (auto position = found->second.rbegin(); position != found->second.rend(); ++position) {
        result.push_back(&bindings_[*position]);
    }
    return result;
}

const Keybinding* KeybindingTable::lookup_primary(std::string_view command,
                                                  const ContextKeys& context,
                                                  bool must_hold) const {
    const auto found = by_command_.find(command);
    if (found == by_command_.end() || found->second.empty()) {
        return nullptr;
    }
    const auto& positions = found->second;
    if (positions.size() == 1 && !must_hold) {
        return &bindings_[positions.front()];
    }
    for (auto position = positions.rbegin(); position != positions.rend(); ++position) {
        if (when_holds(bindings_[*position], context)) {
            return &bindings_[*position];
        }
    }
    return must_hold ? nullptr : &bindings_[positions.back()];
}

std::expected<KeybindingFile, Error> read_keybindings(const nlohmann::json& json,
                                                      KeybindingSource source) {
    if (!json.is_array()) {
        return std::unexpected(
            Error{ErrorCode::invalid_argument, "the key bindings are not an array of objects"});
    }
    KeybindingFile file;
    const auto skip = [&](std::size_t index, const std::string& why) {
        file.problems.push_back("entry " + std::to_string(index) + ": " + why);
    };
    for (std::size_t index = 0; index < json.size(); ++index) {
        const nlohmann::json& entry = json[index];
        if (!entry.is_object()) {
            skip(index, "not an object");
            continue;
        }
        const auto command = entry.find("command");
        if (command == entry.end() || !command->is_string() ||
            command->get<std::string>().empty()) {
            skip(index, "\"command\" is missing or not a string");
            continue;
        }
        Keybinding binding;
        binding.command = command->get<std::string>();
        binding.source = source;

        const auto key = entry.find("key");
        if (key != entry.end() && !key->is_string()) {
            skip(index, "\"key\" is not a string");
            continue;
        }
        if (key != entry.end() && !key->get<std::string>().empty()) {
            auto chord = parse_key_chord(key->get<std::string>());
            if (!chord) {
                skip(index, chord.error().message);
                continue;
            }
            binding.chord = *std::move(chord);
        } else if (!is_removal(binding)) {
            skip(index, "\"key\" is missing");
            continue;
        }
        if (const auto when = entry.find("when"); when != entry.end() && !when->is_null()) {
            if (!when->is_string()) {
                skip(index, "\"when\" is not a string");
                continue;
            }
            auto expression = WhenExpression::parse(when->get<std::string>());
            if (!expression) {
                skip(index, "\"when\": " + expression.error().message);
                continue;
            }
            binding.when = *std::move(expression);
        }
        if (const auto args = entry.find("args"); args != entry.end()) {
            binding.args = *args;
        }
        file.bindings.push_back(std::move(binding));
    }
    return file;
}

} // namespace anvil::core
