#include "anvil/workbench/menu_model.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <gtest/gtest.h>
#include <iterator>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

namespace anvil::workbench {
namespace {

core::ContextKeys desktop() {
    core::ContextKeys keys;
    set_menu_context_defaults(keys);
    return keys;
}

std::vector<std::string> texts(const std::vector<MenuItem>& items) {
    std::vector<std::string> result;
    for (const MenuItem& item : items) {
        result.push_back(item.kind == MenuEntryKind::separator ? "-" : item.label.text);
    }
    return result;
}

const MenuItem* find_id(const std::vector<MenuItem>& items, std::string_view id) {
    for (const MenuItem& item : items) {
        if (item.id == id) {
            return &item;
        }
        if (const MenuItem* inner = find_id(item.items, id)) {
            return inner;
        }
    }
    return nullptr;
}

void for_each_menu(const std::vector<MenuItem>& items,
                   const std::function<void(const std::vector<MenuItem>&)>& visit) {
    visit(items);
    for (const MenuItem& item : items) {
        if (item.kind == MenuEntryKind::submenu) {
            for_each_menu(item.items, visit);
        }
    }
}

TEST(MenuLabel, TheMarkersGiveTheMnemonic) {
    EXPECT_EQ(parse_menu_label("&&File"), (MenuLabel{"File", 0}));
    EXPECT_EQ(parse_menu_label("New &&Window"), (MenuLabel{"New Window", 4}));
    EXPECT_EQ(parse_menu_label("Save A&&ll"), (MenuLabel{"Save All", 6}));
    EXPECT_EQ(parse_menu_label("Zen Mode"), (MenuLabel{"Zen Mode", MenuLabel::npos}));
}

TEST(MenuLabel, ASingleAmpersandIsLiteralAndATrailingMarkerIsDropped) {
    EXPECT_EQ(parse_menu_label("Tom & Jerry"), (MenuLabel{"Tom & Jerry", MenuLabel::npos}));
    EXPECT_EQ(parse_menu_label("End&&"), (MenuLabel{"End", MenuLabel::npos}));
    EXPECT_EQ(parse_menu_label(""), (MenuLabel{"", MenuLabel::npos}));
}

TEST(MenuLabel, OnlyTheFirstMarkerIsTheMnemonic) {
    const MenuLabel label = parse_menu_label("&&A and &&B");
    EXPECT_EQ(label.text, "A and B");
    EXPECT_EQ(label.mnemonic, 0u);
}

TEST(MenuTable, IsWellFormed) {
    ASSERT_GT(std::size(menu_table), 0u);
    EXPECT_EQ(menu_table[0].depth, 0);
    EXPECT_EQ(menu_table[0].kind, MenuEntryKind::submenu);
    int previous_depth = 0;
    for (std::size_t i = 0; i < std::size(menu_table); ++i) {
        const MenuEntry& entry = menu_table[i];
        EXPECT_LE(entry.depth, previous_depth + 1) << i;
        if (entry.kind == MenuEntryKind::separator) {
            EXPECT_GE(entry.depth, 1) << i;
            EXPECT_TRUE(entry.id.empty() && entry.label.empty()) << i;
        } else {
            EXPECT_FALSE(entry.id.empty()) << i;
            EXPECT_FALSE(entry.label.empty()) << entry.id;
        }
        for (const std::string_view clause : {entry.when, entry.precondition, entry.toggled}) {
            if (!clause.empty()) {
                EXPECT_TRUE(core::WhenExpression::parse(clause).has_value()) << clause;
            }
        }
        previous_depth = entry.depth;
    }
}

TEST(MenuTable, EveryConfigKeyThatAClauseReadsHasADefault) {
    std::set<std::string> defaults;
    for (const MenuConfigDefault& entry : menu_config_defaults) {
        defaults.insert(std::string(entry.key));
        EXPECT_FALSE(nlohmann::json::parse(entry.json, nullptr, false).is_discarded()) << entry.key;
    }
    const std::string_view prefix = "config.";
    for (const MenuEntry& entry : menu_table) {
        for (const std::string_view clause : {entry.when, entry.precondition, entry.toggled}) {
            for (std::size_t at = clause.find(prefix); at != std::string_view::npos;
                 at = clause.find(prefix, at + 1)) {
                std::size_t end = at + prefix.size();
                while (end < clause.size() &&
                       (std::isalnum(static_cast<unsigned char>(clause[end])) != 0 ||
                        clause[end] == '.')) {
                    ++end;
                }
                const std::string key(clause.substr(at + prefix.size(), end - at - prefix.size()));
                EXPECT_TRUE(defaults.contains(key)) << key;
            }
        }
    }
}

TEST(MenuTable, TheMinimapIsToggledByItsSetting) {
    const auto found = std::ranges::find_if(menu_table, [](const MenuEntry& entry) {
        return entry.id == "editor.action.toggleMinimap";
    });
    ASSERT_NE(found, std::end(menu_table));
    EXPECT_EQ(found->toggled, "config.editor.minimap.enabled");
    const auto save = std::ranges::find_if(menu_table, [](const MenuEntry& entry) {
        return entry.id == "workbench.action.files.save";
    });
    ASSERT_NE(save, std::end(menu_table));
    EXPECT_EQ(save->precondition, "activeEditor || foldersViewVisible && sideBarFocus");
}

TEST(MenuModel, TheMenuBarIsVSCodes) {
    EXPECT_EQ(texts(top_level_menus(desktop())),
              (std::vector<std::string>{"File", "Edit", "Selection", "View", "Go", "Run",
                                        "Terminal", "Help"}));
}

TEST(MenuModel, TheMenuBarKeepsTheMnemonicsOfVSCode) {
    for (const MenuItem& menu : top_level_menus(desktop())) {
        EXPECT_EQ(menu.label.mnemonic, 0u) << menu.label.text;
    }
    const auto menus = top_level_menus(desktop());
    EXPECT_EQ(menus[0].id, "MenubarFileMenu");
    EXPECT_EQ(menus[5].id, "MenubarDebugMenu");
}

TEST(MenuModel, ThePreferencesMenuBelongsToMacOS) {
    core::ContextKeys keys = desktop();
    EXPECT_EQ(top_level_menus(keys).size(), 8u);
    keys.set("isMacNative", true);
    const auto menus = top_level_menus(keys);
    ASSERT_EQ(menus.size(), 9u);
    EXPECT_EQ(menus.back().label.text, "Preferences");
}

TEST(MenuModel, TheEditMenuHasTheClipboardAndFindCommandsInOrder) {
    const auto edit = menu_items("MenubarEditMenu", desktop());
    std::vector<std::string> ids;
    for (const MenuItem& item : edit) {
        if (item.kind != MenuEntryKind::separator) {
            ids.push_back(item.id);
        }
    }
    const std::vector<std::string> expected = {
        "undo",
        "redo",
        "editor.action.clipboardCutAction",
        "editor.action.clipboardCopyAction",
        "editor.action.clipboardPasteAction",
        "actions.find",
        "editor.action.startFindReplaceAction",
    };
    std::size_t next = 0;
    for (const std::string& id : ids) {
        if (next < expected.size() && id == expected[next]) {
            ++next;
        }
    }
    EXPECT_EQ(next, expected.size());
}

TEST(MenuModel, AnEntryIsShownOnlyWhileItsWhenClauseHolds) {
    core::ContextKeys keys = desktop();
    EXPECT_EQ(find_id(menu_items("MenubarFileMenu", keys), "workbench.action.closeFolder"),
              nullptr);

    keys.set("workbenchState", std::string("folder"));
    auto items = menu_items("MenubarFileMenu", keys);
    const MenuItem* close = find_id(items, "workbench.action.closeFolder");
    ASSERT_NE(close, nullptr);
    EXPECT_EQ(close->label.text, "Close Folder");

    keys.set("workbenchState", std::string("workspace"));
    items = menu_items("MenubarFileMenu", keys);
    close = find_id(items, "workbench.action.closeFolder");
    ASSERT_NE(close, nullptr);
    EXPECT_EQ(close->label.text, "Close Workspace");
}

TEST(MenuModel, ASubmenuWithNothingToShowIsLeftOut) {
    core::ContextKeys keys = desktop();
    EXPECT_EQ(find_id(menu_items("MenubarEditMenu", keys), "MenubarCopy"), nullptr);
    EXPECT_NE(find_id(menu_items("MenubarDebugMenu", keys), "MenubarNewBreakpointMenu"), nullptr);
    keys.unset("debuggersAvailable");
    EXPECT_EQ(find_id(menu_items("MenubarDebugMenu", keys), "MenubarNewBreakpointMenu"), nullptr);
}

TEST(MenuModel, SeparatorsAreOnlyBetweenEntriesInEveryContext) {
    std::vector<core::ContextKeys> contexts(4);
    contexts[1] = desktop();
    contexts[2] = desktop();
    contexts[2].set("isMacNative", true);
    contexts[2].set("workbenchState", std::string("folder"));
    contexts[3] = desktop();
    contexts[3].unset("debuggersAvailable");
    contexts[3].unset("terminalProcessSupported");
    for (const core::ContextKeys& keys : contexts) {
        for (const MenuItem& top : top_level_menus(keys)) {
            const auto items = menu_items(top.id, keys);
            EXPECT_FALSE(items.empty()) << top.id;
            for_each_menu(items, [&](const std::vector<MenuItem>& level) {
                ASSERT_FALSE(level.empty());
                EXPECT_NE(level.front().kind, MenuEntryKind::separator) << top.id;
                EXPECT_NE(level.back().kind, MenuEntryKind::separator) << top.id;
                for (std::size_t i = 1; i < level.size(); ++i) {
                    EXPECT_FALSE(level[i].kind == MenuEntryKind::separator &&
                                 level[i - 1].kind == MenuEntryKind::separator)
                        << top.id;
                }
            });
        }
    }
}

TEST(MenuModel, EveryTopLevelMenuHasItemsAndEveryItemHasACommand) {
    const core::ContextKeys keys = desktop();
    for (const MenuItem& top : top_level_menus(keys)) {
        const auto items = menu_items(top.id, keys);
        EXPECT_FALSE(items.empty()) << top.id;
        for_each_menu(items, [&](const std::vector<MenuItem>& level) {
            for (const MenuItem& item : level) {
                if (item.kind == MenuEntryKind::item) {
                    EXPECT_FALSE(item.id.empty());
                    EXPECT_TRUE(item.items.empty());
                }
            }
        });
    }
}

core::ContextKeys with_settings(std::string_view jsonc) {
    core::ContextKeys keys = desktop();
    const auto settings = core::Settings::parse(jsonc);
    EXPECT_TRUE(settings.has_value());
    set_menu_config_context(keys, settings.value_or(core::Settings{}));
    return keys;
}

// A copy, so that it outlives the temporary list of items that it was found in.
MenuItem item_of(const std::vector<MenuItem>& menu, std::string_view id) {
    const MenuItem* found = find_id(menu, id);
    EXPECT_NE(found, nullptr) << id;
    return found ? *found : MenuItem{};
}

TEST(MenuModel, AToggledItemIsCheckedWhileItsConditionHolds) {
    const MenuItem minimap_default =
        item_of(menu_items("MenubarViewMenu", with_settings("{}")), "editor.action.toggleMinimap");
    EXPECT_TRUE(minimap_default.checkable);
    EXPECT_TRUE(minimap_default.checked) << "VS Code shows the minimap by default";

    const MenuItem minimap_off = item_of(
        menu_items("MenubarViewMenu", with_settings(R"j({"editor.minimap.enabled": false})j")),
        "editor.action.toggleMinimap");
    EXPECT_TRUE(minimap_off.checkable);
    EXPECT_FALSE(minimap_off.checked);

    EXPECT_FALSE(item_of(menu_items("MenubarFileMenu", with_settings("{}")),
                         "workbench.action.toggleAutoSave")
                     .checked);
    EXPECT_TRUE(item_of(menu_items("MenubarFileMenu",
                                   with_settings(R"j({"files.autoSave": "onFocusChange"})j")),
                        "workbench.action.toggleAutoSave")
                    .checked);
}

TEST(MenuModel, AnItemWithoutAToggledConditionIsNotCheckable) {
    const MenuItem open =
        item_of(menu_items("MenubarFileMenu", desktop()), "workbench.action.files.openFile");
    EXPECT_FALSE(open.checkable);
    EXPECT_FALSE(open.checked);
}

TEST(MenuModel, AnItemIsEnabledWhileItsPreconditionHolds) {
    core::ContextKeys keys = desktop();
    EXPECT_FALSE(
        item_of(menu_items("MenubarFileMenu", keys), "workbench.action.files.save").enabled);
    keys.set("activeEditor", true);
    EXPECT_TRUE(
        item_of(menu_items("MenubarFileMenu", keys), "workbench.action.files.save").enabled);
    EXPECT_TRUE(
        item_of(menu_items("MenubarFileMenu", keys), "workbench.action.files.openFile").enabled);
}

TEST(MenuConfigContext, TheDefaultsApplyWhileTheSettingsSayNothing) {
    const core::ContextKeys keys = with_settings("{}");
    ASSERT_NE(keys.find("config.editor.minimap.enabled"), nullptr);
    EXPECT_EQ(std::get<bool>(*keys.find("config.editor.minimap.enabled")), true);
    EXPECT_EQ(std::get<std::string>(*keys.find("config.files.autoSave")), "off");
}

TEST(MenuConfigContext, TheFileOverridesTheDefaultsInEitherSpelling) {
    const core::ContextKeys flat = with_settings(R"j({"editor.minimap.enabled": false})j");
    EXPECT_EQ(std::get<bool>(*flat.find("config.editor.minimap.enabled")), false);
    const core::ContextKeys nested =
        with_settings(R"j({"editor": {"minimap": {"enabled": false}}})j");
    EXPECT_EQ(std::get<bool>(*nested.find("config.editor.minimap.enabled")), false);
    const core::ContextKeys both = with_settings(
        R"j({"editor": {"minimap": {"enabled": true}}, "editor.minimap.enabled": false})j");
    EXPECT_EQ(std::get<bool>(*both.find("config.editor.minimap.enabled")), false)
        << "a flat key wins over a nested one, as it does for the settings";
    const core::ContextKeys mixed = with_settings(
        R"j({"editor": {"minimap.enabled": false}, "editor.minimap": {"enabled": true}})j");
    EXPECT_EQ(std::get<bool>(*mixed.find("config.editor.minimap.enabled")), false)
        << "the shorter prefix is read first, as the settings find a key";
}

TEST(MenuConfigContext, EverySettingOfTheFileIsAKey) {
    const core::ContextKeys keys = with_settings(
        R"j({"editor.fontSize": 14, "files.exclude": {"a": true}, "custom.list": ["x", "y"],
             "custom.mixed": [1, "y"], "custom.none": null, "files.eol": "\n"})j");
    EXPECT_EQ(std::get<double>(*keys.find("config.editor.fontSize")), 14);
    EXPECT_EQ(std::get<bool>(*keys.find("config.files.exclude.a")), true);
    EXPECT_EQ(std::get<std::vector<std::string>>(*keys.find("config.custom.list")),
              (std::vector<std::string>{"x", "y"}));
    EXPECT_EQ(std::get<std::string>(*keys.find("config.files.eol")), "\n");
    EXPECT_EQ(keys.find("config.custom.mixed"), nullptr);
    EXPECT_EQ(keys.find("config.custom.none"), nullptr);
}

TEST(MenuModel, AnUnknownMenuHasNoItems) {
    EXPECT_TRUE(menu_items("MenubarNoSuchMenu", desktop()).empty());
}

} // namespace
} // namespace anvil::workbench
