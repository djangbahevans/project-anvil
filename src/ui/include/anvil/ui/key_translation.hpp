#pragma once

#include "anvil/core/key_chord.hpp"

#include <QKeyEvent>
#include <optional>

namespace anvil::ui {

// The key press of a Qt key event as a step of a key binding, or nothing for a key that bindings
// cannot name.
//
// The key is found by its place on the keyboard where the toolkit says it (on Linux, from the
// scan code, so that a binding written for `[` works on a layout where that key makes another
// character), and otherwise by the key that Qt reports. Qt reports the character that a key makes
// with Shift held, as in `!` for the key of `1`, so the characters of the shifted keys of a US
// keyboard are taken back to their keys. The numeric keypad is told by Qt's keypad modifier.
//
// Qt's modifiers are the keyboard's, except on macOS, where Qt swaps Control and Command: a
// binding written with `cmd` has Qt's Control modifier there. `platform` says which numbering to
// use, so that all of them can be tested anywhere.
std::optional<core::KeyStep> key_step_from_event(const QKeyEvent& event,
                                                 core::KeyPlatform platform);

// Qt's key for the key of a binding, for the shortcut text of menus and for tests.
std::optional<core::Key> key_from_qt(int qt_key, Qt::KeyboardModifiers modifiers);

} // namespace anvil::ui
