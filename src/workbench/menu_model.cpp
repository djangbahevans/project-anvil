#include "anvil/workbench/menu_model.hpp"

#include <iterator>

namespace anvil::workbench {

MenuLabel parse_menu_label(std::string_view label) {
    MenuLabel result;
    for (std::size_t i = 0; i < label.size(); ++i) {
        if (label[i] == '&' && i + 1 < label.size() && label[i + 1] == '&') {
            i += 2;
            if (i >= label.size()) {
                break;
            }
            if (result.mnemonic == MenuLabel::npos) {
                result.mnemonic = result.text.size();
            }
            result.text.push_back(label[i]);
        } else {
            result.text.push_back(label[i]);
        }
    }
    return result;
}

void set_menu_context_defaults(core::ContextKeys& keys) {
    for (const char* key :
         {"customExecutionSupported", "processExecutionSupported", "shellExecutionSupported",
          "terminalProcessSupported", "openFolderWorkspaceSupport",
          "enterMultiRootWorkspaceSupport", "emptyWorkspaceSupport", "debuggersAvailable",
          "viewContainer.workbench.view.explorer.enabled",
          "viewContainer.workbench.view.debug.enabled",
          "viewContainer.workbench.view.extensions.enabled",
          "viewContainer.workbench.view.extension.test.enabled", "workbench.view.search.active",
          "workbench.scm.active", "workbench.panel.markers.view.active",
          "workbench.panel.output.active", "workbench.panel.repl.view.active", "terminal.active",
          "chatSetupHidden"}) {
        keys.set(key, true);
    }
    keys.set("workbenchState", std::string("empty"));
    keys.set("multiCursorModifier", std::string("ctrlCmd"));
    keys.set("titleBarStyle", std::string("native"));
}

namespace {

bool shown(const MenuEntry& entry, const core::ContextKeys& context) {
    if (entry.when.empty()) {
        return true;
    }
    const auto expression = core::WhenExpression::parse(entry.when);
    return !expression || expression->evaluate(context);
}

// Builds the entries of one menu level: those at `depth` that follow `index`, with the entries
// below them, moving `index` past all of them.
std::vector<MenuItem> build(std::size_t& index, std::size_t depth,
                            const core::ContextKeys& context) {
    std::vector<MenuItem> items;
    bool pending_separator = false;
    while (index < std::size(menu_table) && menu_table[index].depth >= depth) {
        const MenuEntry& entry = menu_table[index++];
        if (entry.kind == MenuEntryKind::separator) {
            pending_separator = !items.empty();
            continue;
        }
        MenuItem item{.kind = entry.kind,
                      .label = parse_menu_label(entry.label),
                      .id = std::string(entry.id)};
        const bool visible = shown(entry, context);
        if (entry.kind == MenuEntryKind::submenu) {
            std::vector<MenuItem> children = build(index, depth + 1, context);
            if (!visible || children.empty()) {
                continue;
            }
            item.items = std::move(children);
        } else if (!visible) {
            continue;
        }
        if (pending_separator) {
            items.push_back(MenuItem{.kind = MenuEntryKind::separator});
            pending_separator = false;
        }
        items.push_back(std::move(item));
    }
    return items;
}

} // namespace

std::vector<MenuItem> top_level_menus(const core::ContextKeys& context) {
    std::vector<MenuItem> menus;
    for (const MenuEntry& entry : menu_table) {
        if (entry.depth == 0 && shown(entry, context)) {
            menus.push_back(MenuItem{.kind = MenuEntryKind::submenu,
                                     .label = parse_menu_label(entry.label),
                                     .id = std::string(entry.id)});
        }
    }
    return menus;
}

std::vector<MenuItem> menu_items(std::string_view menu_id, const core::ContextKeys& context) {
    for (std::size_t i = 0; i < std::size(menu_table); ++i) {
        if (menu_table[i].depth == 0 && menu_table[i].id == menu_id) {
            std::size_t index = i + 1;
            return build(index, 1, context);
        }
    }
    return {};
}

} // namespace anvil::workbench
