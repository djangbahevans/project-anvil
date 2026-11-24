#include "anvil/workbench/menu_model.hpp"

#include <algorithm>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>

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
    keys.set("multiCursorModifier", std::string("altKey"));
    keys.set("titleBarStyle", std::string("native"));
}

namespace {

std::optional<core::ContextValue> context_value(const nlohmann::json& value) {
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_number()) {
        return value.get<double>();
    }
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_array() &&
        std::ranges::all_of(value, [](const auto& item) { return item.is_string(); })) {
        return value.get<std::vector<std::string>>();
    }
    return std::nullopt;
}

void set_config_key(core::ContextKeys& keys, const std::string& name, const nlohmann::json& value) {
    if (auto converted = context_value(value)) {
        keys.set("config." + name, std::move(*converted));
    }
}

void set_config_values(core::ContextKeys& keys, const std::string& prefix,
                       const nlohmann::json& object) {
    for (const auto& [name, value] : object.items()) {
        const std::string path = prefix.empty() ? name : prefix + "." + name;
        if (value.is_object()) {
            set_config_values(keys, path, value);
        } else {
            set_config_key(keys, path, value);
        }
    }
}

// A clause that does not parse holds, so that a mistake in the table shows an entry that works
// instead of hiding it.
bool holds(std::string_view clause, const core::ContextKeys& context) {
    if (clause.empty()) {
        return true;
    }
    const auto expression = core::WhenExpression::parse(clause);
    return !expression || expression->evaluate(context);
}

bool shown(const MenuEntry& entry, const core::ContextKeys& context) {
    return holds(entry.when, context);
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
        } else {
            item.enabled = holds(entry.precondition, context);
            item.checkable = !entry.toggled.empty();
            item.checked = item.checkable && holds(entry.toggled, context);
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

void set_menu_config_context(core::ContextKeys& keys, const core::Settings& settings) {
    for (const MenuConfigDefault& entry : menu_config_defaults) {
        set_config_key(keys, std::string(entry.key),
                       nlohmann::json::parse(entry.json, nullptr, false));
    }
    set_config_values(keys, "", settings.root());
    for (const MenuConfigDefault& entry : menu_config_defaults) {
        if (const nlohmann::json* value = settings.find(entry.key)) {
            set_config_key(keys, std::string(entry.key), *value);
        }
    }
}

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
