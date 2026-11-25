#pragma once

#include "anvil/core/key_chord.hpp"
#include "anvil/core/keybindings.hpp"

#include <map>
#include <string>

namespace anvil::ui {

// The titles of the commands by id, as the command palette shows them.
using CommandTitles = std::map<std::string, std::string, std::less<>>;

// The text of the Keyboard Shortcuts Reference: a heading, a blank line and a line
// `<title><TAB><keys>` for every command that has a key in `table`, sorted by title without regard
// to case and then by id. The keys of a command are separated by `, ` and spelled as `platform`'s
// key table spells them. A command with no title, or an empty one, is listed by its id.
std::string keybindings_reference(const CommandTitles& titles, const core::KeybindingTable& table,
                                  core::KeyPlatform platform);

} // namespace anvil::ui
