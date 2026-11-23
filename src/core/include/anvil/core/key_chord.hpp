#pragma once

#include "anvil/core/error.hpp"
#include "anvil/core/key_table.hpp"

#include <compare>
#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace anvil::core {

struct Modifiers {
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    // The Windows key, the Command key of a Mac, the Super key.
    bool meta = false;

    auto operator<=>(const Modifiers&) const = default;
};

// One press: some modifiers held and a key.
struct KeyStep {
    Modifiers modifiers;
    Key key = Key::backspace;

    auto operator<=>(const KeyStep&) const = default;
};

// A key binding as VS Code writes it: one to `max_steps` presses in a row, such as `ctrl+k ctrl+c`.
// Two spellings of the same presses give equal chords.
struct KeyChord {
    static constexpr std::size_t max_steps = 4;

    std::vector<KeyStep> steps;

    auto operator<=>(const KeyChord&) const = default;
};

// The operating system whose names for the modifiers a chord is written with.
enum class KeyPlatform { linux, windows, macos };

// Reads a chord from `text`: steps separated by white space, each with modifiers (`ctrl`, `shift`,
// `alt`, `meta`, and `cmd`, `win` and `super` for meta) before a key, joined by `+` or `-`.
// Capital letters and the order of the modifiers do not matter. A key is named as in the
// `keybindings.json` of VS Code: a letter or digit, `f1` to `f24`, `escape`, `enter`, `tab`,
// `space`, `backspace`, `delete`, `insert`, `home`, `end`, `pageup`, `pagedown`, `left`, `right`,
// `up`, `down`, a punctuation mark or its `oem_` name, `numpad0` to `numpad9` and `numpad_add` and
// the other numpad operators, and the rest of VS Code's keys (see `key_table`). The modifier keys
// can be keys themselves, as in `shift shift`; a modifier repeated as the key is dropped.
//
// An empty text, a fifth step, an unknown modifier or key and a modifier without a key fail with
// ErrorCode::invalid_argument and a message that gives the position of the problem in `text`.
std::expected<KeyChord, Error> parse_key_chord(std::string_view text);

// The chord as a key binding file spells it: lowercase, modifiers in the order ctrl, shift, alt and
// meta, and meta written as `cmd` on macOS, `win` on Windows and `meta` on Linux.
std::string format_key_chord(const KeyChord& chord, KeyPlatform platform = KeyPlatform::linux);

// The chord as the interface shows it: `Ctrl+Shift+K` on Windows, with `Super` on Linux, and `⌃⇧⌘K`
// on macOS, which has arrows for the arrow keys.
std::string key_chord_label(const KeyChord& chord, KeyPlatform platform);

// The names of keys.
const KeyInfo& key_info(Key key);
std::optional<Key> key_from_name(std::string_view name);

} // namespace anvil::core
