#pragma once

#include "anvil/core/settings.hpp"
#include "anvil/core/when_expression.hpp"
#include "anvil/workbench/menu_table.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace anvil::workbench {

// A menu title as shown: `text` has no markers, and `mnemonic` is the byte of `text` that the
// user presses with Alt, or `npos`.
struct MenuLabel {
    static constexpr std::size_t npos = std::string::npos;

    std::string text;
    std::size_t mnemonic = npos;

    bool operator==(const MenuLabel&) const = default;
};

// Reads VS Code's spelling of a title, in which `&&` precedes the mnemonic character. A single
// `&` is a literal ampersand, and a mnemonic marker at the end of the title is dropped.
MenuLabel parse_menu_label(std::string_view label);

// An entry of a menu that is shown for a context: a submenu holds its own `items`.
struct MenuItem {
    MenuEntryKind kind = MenuEntryKind::item;
    MenuLabel label{};
    std::string id{};
    // False when the item's `precondition` does not hold for the context.
    bool enabled = true;
    // An item with a `toggled` condition is checkable, and checked while the condition holds.
    bool checkable = false;
    bool checked = false;
    std::vector<MenuItem> items{};
};

// The context keys of a desktop VS Code window with nothing open that the `when` clauses of the
// menus read and the window does not otherwise know: the capabilities every desktop window has
// and the views and panels that are listed whether or not they have anything to show. A key
// that depends on state (a folder being open, a debug session) is left unset.
void set_menu_context_defaults(core::ContextKeys& keys);

// Sets `config.<key>` for the settings that the menus' clauses read, to the value in `settings`
// or else to VS Code's default, and for every other value in `settings` too, so that a clause such
// as `config.editor.minimap.enabled` reads what the settings file says. A key that is spelled in
// the file as nested objects (`"editor": {"minimap": {"enabled": false}}`) is set under its dotted
// name. Values that are neither booleans, numbers, strings nor lists of strings are left out.
void set_menu_config_context(core::ContextKeys& keys, const core::Settings& settings);

// The titles of the menu bar, in order, without their items.
std::vector<MenuItem> top_level_menus(const core::ContextKeys& context);

// The entries of the top-level menu `menu_id` (`MenubarFileMenu`) as the context shows them.
// An entry whose `when` clause does not hold is left out, and so is an entry whose id is in
// `unprovided`, with the entries of a submenu that is left out, and a submenu with no entries
// left. A separator is kept only between two entries. An item whose `precondition` does not hold
// is disabled, and one with a `toggled` condition is checkable.
std::vector<MenuItem> menu_items(std::string_view menu_id, const core::ContextKeys& context,
                                 std::span<const std::string_view> unprovided = {});

// The ids of the entries of VS Code's menus that Anvil does not provide and leaves out of its
// menus: the webviews, Microsoft's pages and the tools of Chromium and Electron among the Help
// items. Offering one is removing its id from the list.
std::span<const std::string_view> unprovided_menu_items();

} // namespace anvil::workbench
