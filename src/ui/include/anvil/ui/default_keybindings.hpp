#pragma once

#include "anvil/core/keybindings.hpp"

#include <vector>

namespace anvil::ui {

// The key bindings of the editor's built-in commands for a platform, as VS Code binds the same
// commands: the order is the one in which they resolve, so a later binding of a key wins.
//
// Two differences from VS Code come from the commands, which act on the editor and not on any text
// input: the cursor, delete, undo, clipboard and find-step bindings apply where `editorTextFocus`
// holds, where VS Code uses `textInputFocus` or none, so that the keys of the find field and of
// the command palette keep their own meaning; and Open File has Command+O on macOS, where VS Code's
// Command+O is Open (a file or a folder).
std::vector<core::Keybinding> default_keybindings(core::KeyPlatform platform);

} // namespace anvil::ui
