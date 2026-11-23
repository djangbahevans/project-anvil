#pragma once

#include "anvil/core/when_expression.hpp"
#include "anvil/workbench/menu_table.hpp"

#include <cstddef>
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
    std::vector<MenuItem> items{};
};

// The context keys of a desktop VS Code window with nothing open that the `when` clauses of the
// menus read and the window does not otherwise know: the capabilities every desktop window has
// and the views and panels that are listed whether or not they have anything to show. A key
// that depends on state (a folder being open, a debug session) is left unset.
void set_menu_context_defaults(core::ContextKeys& keys);

// The titles of the menu bar, in order, without their items.
std::vector<MenuItem> top_level_menus(const core::ContextKeys& context);

// The entries of the top-level menu `menu_id` (`MenubarFileMenu`) as the context shows them.
// An entry whose `when` clause does not hold is left out, and so is a submenu with no entries
// left. A separator is kept only between two entries.
std::vector<MenuItem> menu_items(std::string_view menu_id, const core::ContextKeys& context);

} // namespace anvil::workbench
