#pragma once

#include "anvil/core/error.hpp"
#include "anvil/core/key_chord.hpp"
#include "anvil/core/when_expression.hpp"

#include <cstddef>
#include <expected>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace anvil::core {

// Where a key binding comes from. A later source overrides an earlier one: the editor's own
// bindings, then those of extensions, then the user's.
enum class KeybindingSource { builtin, extension, user };

struct Keybinding {
    // At least one step, except for a removal, which can leave it empty to match every key.
    KeyChord chord;
    // A command starting with `-` removes the bindings of that command. One starting with `^` lets
    // the key press go on to the widgets as well: the table drops the `^` and sets `bubble`.
    std::string command;
    bool bubble = false;
    std::optional<WhenExpression> when;
    std::optional<nlohmann::json> args;
    KeybindingSource source = KeybindingSource::builtin;
};

// What a key press does to the bindings.
struct Resolution {
    enum class Kind {
        // Nothing is bound to the presses so far.
        none,
        // Longer bindings start with the presses so far: wait for the next key.
        more_keys_needed,
        // A binding is complete.
        found,
    };
    Kind kind = Kind::none;
    // With `found`: the command, which can be empty for a key that is bound to nothing, its
    // arguments, and whether the press should go on to the widgets after the command ran.
    std::string command;
    std::optional<nlohmann::json> args;
    bool bubble = false;
};

// The key bindings of the editor: the built-in ones, those of extensions and the user's, merged as
// VS Code does (`keybindingResolver.ts`).
//
// The bindings are ordered by source, and by the order they were given in within a source, and a
// key press finds the last binding in that order whose key is the press and whose `when` holds.
// That binding wins whatever the others say, including a shorter one that is a prefix of a longer
// one, and so a binding is complete or waits for more keys depending on the winner alone.
//
// A removal (a command starting with `-`) takes away the built-in and extension bindings of that
// command whose key starts with the key of the removal (every key when it has none) and whose
// `when` is implied by that of the removal, when it has one. The bindings of the user are never
// removed, and removals are not bindings.
class KeybindingTable {
public:
    KeybindingTable() = default;
    explicit KeybindingTable(std::vector<Keybinding> bindings);

    // Resolves a key press, `next`, that follows the presses `pending` of a chord in progress.
    Resolution resolve(std::span<const KeyStep> pending, const KeyStep& next,
                       const ContextKeys& context) const;

    // The bindings of a command, with the most specific first, as menus show them. A binding that
    // a later one for the same key shadows entirely is left out. The result points into the table.
    std::vector<const Keybinding*> lookup(std::string_view command) const;
    // The binding to show for a command: the first one whose `when` holds in `context`, or, unless
    // `must_hold`, the most specific one when none does.
    const Keybinding* lookup_primary(std::string_view command, const ContextKeys& context,
                                     bool must_hold = false) const;

    // All the bindings that are left after the removals, in the order they resolve in.
    const std::vector<Keybinding>& bindings() const {
        return bindings_;
    }

private:
    std::vector<Keybinding> bindings_;
    // The positions in `bindings_` of the bindings that start with a key press, in order.
    std::map<KeyStep, std::vector<std::size_t>> by_first_step_;
    // The bindings of each command, in order, without the ones that others shadow.
    std::map<std::string, std::vector<std::size_t>, std::less<>> by_command_;
};

struct KeybindingFile {
    std::vector<Keybinding> bindings;
    // One message for each entry that was left out, starting with `entry N` (counted from zero).
    std::vector<std::string> problems;
};

// Reads the bindings of a `keybindings.json`: an array of objects with `key`, `command`, and
// optionally `when` and `args`. An entry that cannot be used is reported and skipped. A binding
// without a key is only valid as a removal. Fails when the value is not an array.
std::expected<KeybindingFile, Error> read_keybindings(const nlohmann::json& json,
                                                      KeybindingSource source);

} // namespace anvil::core
