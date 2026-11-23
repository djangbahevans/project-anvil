#pragma once

#include "anvil/core/key_chord.hpp"
#include "anvil/core/keybindings.hpp"
#include "anvil/core/log.hpp"

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace anvil::exthost {

// A command that an extension declares in `contributes.commands`.
struct ContributedCommand {
    // The id of the extension, `publisher.name`.
    std::string extension;
    std::string command;
    // Both already have the extension's `package.nls.json` applied. The category is empty when the
    // command has none.
    std::string title;
    std::string category;
    // A `when` expression that decides whether the command can run; empty means always.
    std::string enablement;

    // What the command palette shows: `Category: Title`, or the title alone.
    std::string label() const;
};

// A key binding from `contributes.keybindings`. `mac`, `linux` and `win` replace `key` on their
// platform.
struct ContributedKeybinding {
    std::string extension;
    std::string command;
    std::string key;
    std::string mac;
    std::string linux;
    std::string win;
    std::string when;
    // The arguments as JSON text; empty when there are none.
    std::string args;
};

// A rule of `contributes.menus.commandPalette`: the command is in the palette only while `when`
// holds. A command without a rule is always in it.
struct PaletteRule {
    std::string extension;
    std::string command;
    std::string when;
};

struct Contributions {
    std::vector<ContributedCommand> commands;
    std::vector<ContributedKeybinding> keybindings;
    std::vector<PaletteRule> palette_rules;
};

// Reads the `contributes` sections of the manifests of the extensions in `directories`. A manifest
// that cannot be read is left to `build_init_data` to report. A section that is not what VS Code
// documents, and an entry of a section that cannot be used, is logged once, with the extension
// that declares it, and skipped; the other sections and extensions are read as usual.
Contributions read_contributions(std::span<const std::filesystem::path> directories,
                                 core::Logger& logger);

// The key bindings of the extensions for `platform`, with the source `extension`. A binding with
// no key for the platform is left out, and one whose key or `when` cannot be read is reported in
// `problems` and left out.
core::KeybindingFile extension_keybindings(const Contributions& contributions,
                                           core::KeyPlatform platform);

} // namespace anvil::exthost
