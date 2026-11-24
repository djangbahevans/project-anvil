#include "anvil/ui/default_keybindings.hpp"

#include <algorithm>
#include <gtest/gtest.h>
#include <set>
#include <string>

using namespace anvil::ui;
using namespace anvil::core;

namespace {

const KeyPlatform platforms[] = {KeyPlatform::linux, KeyPlatform::windows, KeyPlatform::macos};

std::vector<std::string> keys_of(const std::vector<Keybinding>& bindings,
                                 const std::string& command) {
    std::vector<std::string> keys;
    for (const Keybinding& binding : bindings) {
        if (binding.command == command) {
            keys.push_back(format_key_chord(binding.chord));
        }
    }
    // The primary key comes last in the table, and first here.
    std::ranges::reverse(keys);
    return keys;
}

std::vector<std::string> chords(std::initializer_list<const char*> texts) {
    std::vector<std::string> formatted;
    for (const char* text : texts) {
        formatted.push_back(format_key_chord(*parse_key_chord(text)));
    }
    return formatted;
}

KeyStep step(std::string_view text) {
    return parse_key_chord(text)->steps.front();
}

std::string resolved(const KeybindingTable& table, std::string_view key,
                     const ContextKeys& context) {
    const Resolution resolution = table.resolve({}, step(key), context);
    return resolution.kind == Resolution::Kind::found ? resolution.command : "-";
}

ContextKeys context(std::initializer_list<const char*> true_keys) {
    ContextKeys keys;
    for (const char* key : true_keys) {
        keys.set(key, true);
    }
    return keys;
}

} // namespace

TEST(DefaultKeybindingsTest, EveryBindingIsBuiltInAndHasAKeyAndACommand) {
    for (const KeyPlatform platform : platforms) {
        const auto bindings = default_keybindings(platform);
        EXPECT_GT(bindings.size(), 30U);
        for (const Keybinding& binding : bindings) {
            EXPECT_EQ(binding.source, KeybindingSource::builtin);
            EXPECT_FALSE(binding.command.empty());
            EXPECT_FALSE(binding.chord.steps.empty()) << binding.command;
            EXPECT_FALSE(binding.when && binding.when->is_false()) << binding.command;
        }
    }
}

TEST(DefaultKeybindingsTest, EveryPlatformBindsTheSameCommandsExceptWhereVsCodeBindsNone) {
    std::set<std::string> linux_commands;
    std::set<std::string> windows_commands;
    std::set<std::string> macos_commands;
    for (const Keybinding& binding : default_keybindings(KeyPlatform::linux)) {
        linux_commands.insert(binding.command);
    }
    for (const Keybinding& binding : default_keybindings(KeyPlatform::windows)) {
        windows_commands.insert(binding.command);
    }
    for (const Keybinding& binding : default_keybindings(KeyPlatform::macos)) {
        macos_commands.insert(binding.command);
    }
    // Windows has Alt+F4 from the system for quitting.
    windows_commands.insert("workbench.action.quit");
    // VS Code binds Save All on Windows and macOS only.
    linux_commands.insert("saveAll");
    EXPECT_EQ(linux_commands, windows_commands);
    EXPECT_EQ(linux_commands, macos_commands);
}

TEST(DefaultKeybindingsTest, TheStandardShortcutsAreVsCodes) {
    const auto linux = default_keybindings(KeyPlatform::linux);
    EXPECT_EQ(keys_of(linux, "workbench.action.files.save"), chords({"ctrl+s"}));
    EXPECT_EQ(keys_of(linux, "workbench.action.files.saveAs"), chords({"ctrl+shift+s"}));
    EXPECT_EQ(keys_of(linux, "workbench.action.files.openFile"), chords({"ctrl+o"}));
    EXPECT_EQ(keys_of(linux, "undo"), chords({"ctrl+z"}));
    EXPECT_EQ(keys_of(linux, "redo"), chords({"ctrl+y", "ctrl+shift+z"}));
    EXPECT_EQ(keys_of(linux, "actions.find"), chords({"ctrl+f"}));
    EXPECT_EQ(keys_of(linux, "editor.action.startFindReplaceAction"), chords({"ctrl+h"}));
    EXPECT_EQ(keys_of(linux, "editor.action.nextMatchFindAction"), chords({"f3"}));
    EXPECT_EQ(keys_of(linux, "editor.action.previousMatchFindAction"), chords({"shift+f3"}));
    EXPECT_EQ(keys_of(linux, "workbench.action.showCommands"), chords({"ctrl+shift+p", "f1"}));
    EXPECT_EQ(keys_of(linux, "workbench.action.toggleSidebarVisibility"), chords({"ctrl+b"}));
    EXPECT_EQ(keys_of(linux, "editor.action.addSelectionToNextFindMatch"), chords({"ctrl+d"}));
    EXPECT_EQ(keys_of(linux, "editor.action.selectHighlights"), chords({"ctrl+shift+l"}));
    EXPECT_EQ(keys_of(linux, "editor.action.selectAllMatches"), chords({"alt+enter"}));
    EXPECT_EQ(keys_of(default_keybindings(KeyPlatform::macos),
                      "editor.action.addSelectionToNextFindMatch"),
              chords({"cmd+d"}));
    EXPECT_EQ(keys_of(linux, "workbench.action.togglePanel"), chords({"ctrl+j"}));
    EXPECT_EQ(keys_of(linux, "workbench.action.toggleFullScreen"), chords({"f11"}));
    EXPECT_EQ(keys_of(linux, "workbench.action.splitEditor"), chords({"ctrl+\\"}));
    EXPECT_EQ(keys_of(linux, "workbench.action.closeActiveEditor"), chords({"ctrl+w"}));
    EXPECT_EQ(keys_of(linux, "workbench.action.quit"), chords({"ctrl+q"}));
    EXPECT_EQ(keys_of(linux, "editor.action.clipboardPasteAction"),
              chords({"ctrl+v", "shift+insert"}));
    EXPECT_EQ(keys_of(linux, "editor.action.clipboardCutAction"), chords({"ctrl+x"}));
}

TEST(DefaultKeybindingsTest, SaveAllHasAChordOnWindowsAndAShortcutOnMacOSAndNothingOnLinux) {
    EXPECT_TRUE(keys_of(default_keybindings(KeyPlatform::linux), "saveAll").empty());
    EXPECT_EQ(keys_of(default_keybindings(KeyPlatform::windows), "saveAll"), chords({"ctrl+k s"}));
    EXPECT_EQ(keys_of(default_keybindings(KeyPlatform::macos), "saveAll"), chords({"cmd+alt+s"}));
}

TEST(DefaultKeybindingsTest, TheNavigationHistoryKeysAreVSCodesOnEveryPlatform) {
    const auto linux = default_keybindings(KeyPlatform::linux);
    const auto windows = default_keybindings(KeyPlatform::windows);
    const auto macos = default_keybindings(KeyPlatform::macos);
    EXPECT_EQ(keys_of(linux, "workbench.action.navigateBack"),
              chords({"ctrl+alt+-", "browserback"}));
    EXPECT_EQ(keys_of(windows, "workbench.action.navigateBack"),
              chords({"alt+left", "browserback"}));
    EXPECT_EQ(keys_of(macos, "workbench.action.navigateBack"), chords({"ctrl+-", "browserback"}));
    EXPECT_EQ(keys_of(linux, "workbench.action.navigateForward"),
              chords({"ctrl+shift+-", "browserforward"}));
    EXPECT_EQ(keys_of(windows, "workbench.action.navigateForward"),
              chords({"alt+right", "browserforward"}));
    EXPECT_EQ(keys_of(macos, "workbench.action.navigateForward"),
              chords({"ctrl+shift+-", "browserforward"}));
    EXPECT_EQ(keys_of(linux, "workbench.action.navigateToLastEditLocation"),
              chords({"ctrl+k ctrl+q"}));
    EXPECT_EQ(keys_of(windows, "workbench.action.navigateToLastEditLocation"),
              chords({"ctrl+k ctrl+q"}));
    EXPECT_EQ(keys_of(macos, "workbench.action.navigateToLastEditLocation"),
              chords({"cmd+k cmd+q"}));
}

TEST(DefaultKeybindingsTest, ChangeLanguageModeIsACtrlKChordOnEveryPlatform) {
    const char* command = "workbench.action.editor.changeLanguageMode";
    EXPECT_EQ(keys_of(default_keybindings(KeyPlatform::linux), command), chords({"ctrl+k m"}));
    EXPECT_EQ(keys_of(default_keybindings(KeyPlatform::windows), command), chords({"ctrl+k m"}));
    EXPECT_EQ(keys_of(default_keybindings(KeyPlatform::macos), command), chords({"cmd+k m"}));
}

TEST(DefaultKeybindingsTest, GoToLineAndGoToBracketHaveVSCodesKeys) {
    for (const KeyPlatform platform : platforms) {
        EXPECT_EQ(keys_of(default_keybindings(platform), "workbench.action.gotoLine"),
                  chords({"ctrl+g"}));
    }
    EXPECT_EQ(keys_of(default_keybindings(KeyPlatform::linux), "editor.action.jumpToBracket"),
              chords({"ctrl+shift+\\"}));
    EXPECT_EQ(keys_of(default_keybindings(KeyPlatform::windows), "editor.action.jumpToBracket"),
              chords({"ctrl+shift+\\"}));
    EXPECT_EQ(keys_of(default_keybindings(KeyPlatform::macos), "editor.action.jumpToBracket"),
              chords({"cmd+shift+\\"}));
}

TEST(DefaultKeybindingsTest, ExpandAndShrinkSelectionHaveVSCodesKeys) {
    for (const KeyPlatform platform : {KeyPlatform::linux, KeyPlatform::windows}) {
        const auto bindings = default_keybindings(platform);
        EXPECT_EQ(keys_of(bindings, "editor.action.smartSelect.expand"),
                  chords({"shift+alt+right"}));
        EXPECT_EQ(keys_of(bindings, "editor.action.smartSelect.shrink"),
                  chords({"shift+alt+left"}));
    }
    const auto macos = default_keybindings(KeyPlatform::macos);
    EXPECT_EQ(keys_of(macos, "editor.action.smartSelect.expand"),
              chords({"ctrl+cmd+shift+right", "ctrl+shift+right"}));
    EXPECT_EQ(keys_of(macos, "editor.action.smartSelect.shrink"),
              chords({"ctrl+cmd+shift+left", "ctrl+shift+left"}));
}

TEST(DefaultKeybindingsTest, SettingsAndKeyboardShortcutsHaveVSCodesKeys) {
    for (const KeyPlatform platform : platforms) {
        const auto bindings = default_keybindings(platform);
        const bool macos = platform == KeyPlatform::macos;
        EXPECT_EQ(keys_of(bindings, "workbench.action.openSettings"),
                  chords({macos ? "cmd+," : "ctrl+,"}));
        EXPECT_EQ(keys_of(bindings, "workbench.action.openGlobalKeybindings"),
                  chords({macos ? "cmd+k cmd+s" : "ctrl+k ctrl+s"}));
    }
}

TEST(DefaultKeybindingsTest, ReopenClosedEditorIsCtrlShiftT) {
    EXPECT_EQ(
        keys_of(default_keybindings(KeyPlatform::linux), "workbench.action.reopenClosedEditor"),
        chords({"ctrl+shift+t"}));
    EXPECT_EQ(
        keys_of(default_keybindings(KeyPlatform::windows), "workbench.action.reopenClosedEditor"),
        chords({"ctrl+shift+t"}));
    EXPECT_EQ(
        keys_of(default_keybindings(KeyPlatform::macos), "workbench.action.reopenClosedEditor"),
        chords({"cmd+shift+t"}));
}

TEST(DefaultKeybindingsTest, ColorThemeIsACtrlKChordOnEveryPlatform) {
    const char* command = "workbench.action.selectTheme";
    EXPECT_EQ(keys_of(default_keybindings(KeyPlatform::linux), command), chords({"ctrl+k ctrl+t"}));
    EXPECT_EQ(keys_of(default_keybindings(KeyPlatform::windows), command),
              chords({"ctrl+k ctrl+t"}));
    EXPECT_EQ(keys_of(default_keybindings(KeyPlatform::macos), command), chords({"cmd+k cmd+t"}));
}

TEST(DefaultKeybindingsTest, NewWindowIsCtrlShiftN) {
    const char* command = "workbench.action.newWindow";
    EXPECT_EQ(keys_of(default_keybindings(KeyPlatform::linux), command), chords({"ctrl+shift+n"}));
    EXPECT_EQ(keys_of(default_keybindings(KeyPlatform::windows), command),
              chords({"ctrl+shift+n"}));
    EXPECT_EQ(keys_of(default_keybindings(KeyPlatform::macos), command), chords({"cmd+shift+n"}));
}

TEST(DefaultKeybindingsTest, TheLineCommandsHaveVSCodesKeysAndLinuxAddsCtrlToTheCopyKeys) {
    for (const KeyPlatform platform : platforms) {
        const auto bindings = default_keybindings(platform);
        EXPECT_EQ(keys_of(bindings, "editor.action.moveLinesUpAction"), chords({"alt+up"}));
        EXPECT_EQ(keys_of(bindings, "editor.action.moveLinesDownAction"), chords({"alt+down"}));
        EXPECT_TRUE(keys_of(bindings, "editor.action.duplicateSelection").empty());
    }
    EXPECT_EQ(keys_of(default_keybindings(KeyPlatform::linux), "editor.action.copyLinesUpAction"),
              chords({"ctrl+alt+shift+up"}));
    EXPECT_EQ(
        keys_of(default_keybindings(KeyPlatform::windows), "editor.action.copyLinesDownAction"),
        chords({"alt+shift+down"}));
    EXPECT_EQ(keys_of(default_keybindings(KeyPlatform::macos), "editor.action.copyLinesUpAction"),
              chords({"alt+shift+up"}));
}

TEST(DefaultKeybindingsTest, TheAddCursorKeysAreVSCodesWithLinuxsOwn) {
    const auto linux = default_keybindings(KeyPlatform::linux);
    EXPECT_EQ(keys_of(linux, "editor.action.insertCursorAbove"),
              chords({"shift+alt+up", "ctrl+shift+up"}));
    EXPECT_EQ(keys_of(linux, "editor.action.insertCursorBelow"),
              chords({"shift+alt+down", "ctrl+shift+down"}));
    EXPECT_EQ(keys_of(default_keybindings(KeyPlatform::windows), "editor.action.insertCursorBelow"),
              chords({"ctrl+alt+down"}));
    EXPECT_EQ(keys_of(default_keybindings(KeyPlatform::macos), "editor.action.insertCursorAbove"),
              chords({"cmd+alt+up"}));
    for (const KeyPlatform platform : platforms) {
        const auto bindings = default_keybindings(platform);
        EXPECT_EQ(keys_of(bindings, "editor.action.insertCursorAtEndOfEachLineSelected"),
                  chords({"shift+alt+i"}));
        EXPECT_TRUE(keys_of(bindings, "editor.action.addSelectionToPreviousFindMatch").empty());
    }
}

TEST(DefaultKeybindingsTest, TheTabAndGroupKeysAreVSCodes) {
    const auto linux = default_keybindings(KeyPlatform::linux);
    EXPECT_EQ(keys_of(linux, "workbench.action.nextEditor"), chords({"ctrl+pagedown"}));
    EXPECT_EQ(keys_of(linux, "workbench.action.previousEditor"), chords({"ctrl+pageup"}));
    EXPECT_EQ(keys_of(linux, "workbench.action.nextEditorInGroup"),
              chords({"ctrl+k ctrl+pagedown"}));
    EXPECT_EQ(keys_of(linux, "workbench.action.focusFirstEditorGroup"), chords({"ctrl+1"}));
    EXPECT_EQ(keys_of(linux, "workbench.action.focusEighthEditorGroup"), chords({"ctrl+8"}));
    EXPECT_EQ(keys_of(linux, "workbench.action.focusLeftGroup"), chords({"ctrl+k ctrl+left"}));
    EXPECT_EQ(keys_of(linux, "workbench.action.focusBelowGroup"), chords({"ctrl+k ctrl+down"}));
    const auto macos = default_keybindings(KeyPlatform::macos);
    EXPECT_EQ(keys_of(macos, "workbench.action.nextEditor"),
              chords({"cmd+alt+right", "cmd+shift+]"}));
    EXPECT_EQ(keys_of(macos, "workbench.action.previousEditorInGroup"),
              chords({"cmd+k cmd+alt+left"}));
    EXPECT_EQ(keys_of(macos, "workbench.action.focusThirdEditorGroup"), chords({"cmd+3"}));
    for (const KeyPlatform platform : platforms) {
        const auto bindings = default_keybindings(platform);
        EXPECT_TRUE(keys_of(bindings, "workbench.action.openNextRecentlyUsedEditor").empty());
        EXPECT_TRUE(
            keys_of(bindings, "workbench.action.openPreviousRecentlyUsedEditorInGroup").empty());
    }
}

TEST(DefaultKeybindingsTest, FlipLayoutHasVSCodesKeyAndTheLayoutsHaveNone) {
    EXPECT_EQ(keys_of(default_keybindings(KeyPlatform::linux),
                      "workbench.action.toggleEditorGroupLayout"),
              chords({"shift+alt+0"}));
    EXPECT_EQ(keys_of(default_keybindings(KeyPlatform::macos),
                      "workbench.action.toggleEditorGroupLayout"),
              chords({"cmd+alt+0"}));
    EXPECT_TRUE(keys_of(default_keybindings(KeyPlatform::linux),
                        "workbench.action.editorLayoutTwoByTwoGrid")
                    .empty());
}

TEST(DefaultKeybindingsTest, WindowsHasItsOwnSecondaryKeys) {
    const auto windows = default_keybindings(KeyPlatform::windows);
    EXPECT_EQ(keys_of(windows, "workbench.action.closeActiveEditor"),
              chords({"ctrl+f4", "ctrl+w"}));
    EXPECT_EQ(keys_of(windows, "editor.action.clipboardCutAction"),
              chords({"ctrl+x", "shift+delete"}));
    EXPECT_EQ(keys_of(windows, "editor.action.clipboardCopyAction"),
              chords({"ctrl+c", "ctrl+insert"}));
    EXPECT_TRUE(keys_of(windows, "workbench.action.quit").empty());
}

TEST(DefaultKeybindingsTest, MacUsesCommandWhereVsCodeUsesCtrlCmdAndControlForEmacsKeys) {
    const auto mac = default_keybindings(KeyPlatform::macos);
    EXPECT_EQ(keys_of(mac, "workbench.action.files.save"), chords({"cmd+s"}));
    EXPECT_EQ(keys_of(mac, "redo"), chords({"cmd+shift+z"}));
    EXPECT_EQ(keys_of(mac, "cursorLeft"), chords({"left", "ctrl+b"}));
    EXPECT_EQ(keys_of(mac, "cursorHome"), chords({"home", "cmd+left"}));
    EXPECT_EQ(keys_of(mac, "deleteLeft"),
              chords({"backspace", "shift+backspace", "ctrl+h", "ctrl+backspace"}));
    EXPECT_EQ(keys_of(mac, "editor.action.startFindReplaceAction"), chords({"cmd+alt+f"}));
    EXPECT_EQ(keys_of(mac, "workbench.action.toggleFullScreen"), chords({"ctrl+cmd+f"}));
    EXPECT_EQ(keys_of(mac, "editor.action.nextMatchFindAction"), chords({"cmd+g", "f3"}));
    EXPECT_EQ(keys_of(mac, "workbench.action.quit"), chords({"cmd+q"}));
}

TEST(DefaultKeybindingsTest, TheCommandsOfTheEditorApplyWhereTheEditorHasTheFocus) {
    const KeybindingTable table(default_keybindings(KeyPlatform::linux));
    EXPECT_EQ(resolved(table, "ctrl+z", context({"editorTextFocus"})), "undo");
    EXPECT_EQ(resolved(table, "ctrl+z", context({"inputFocus"})), "-");
    EXPECT_EQ(resolved(table, "left", context({"editorTextFocus"})), "cursorLeft");
    EXPECT_EQ(resolved(table, "left", context({})), "-");
    EXPECT_EQ(resolved(table, "ctrl+c", context({"editorTextFocus"})),
              "editor.action.clipboardCopyAction");
    EXPECT_EQ(resolved(table, "ctrl+c", context({"inputFocus"})), "-");
    EXPECT_EQ(resolved(table, "ctrl+s", context({})), "workbench.action.files.save");
    EXPECT_EQ(resolved(table, "f3", context({"editorFocus"})), "editor.action.nextMatchFindAction");
    EXPECT_EQ(resolved(table, "f3", context({})), "-");
}

TEST(DefaultKeybindingsTest, AltEnterSelectsTheFindMatchesOnlyWhileTheFindBarIsShown) {
    const KeybindingTable table(default_keybindings(KeyPlatform::linux));
    EXPECT_EQ(resolved(table, "alt+enter", context({"editorFocus", "findWidgetVisible"})),
              "editor.action.selectAllMatches");
    EXPECT_EQ(resolved(table, "alt+enter", context({"editorFocus"})), "-");
    EXPECT_EQ(resolved(table, "alt+enter", context({"findWidgetVisible"})), "-");
}

TEST(DefaultKeybindingsTest, ControlWClosesAnEmptyGroupWhenThereIsMoreThanOne) {
    const KeybindingTable table(default_keybindings(KeyPlatform::linux));
    EXPECT_EQ(resolved(table, "ctrl+w", context({})), "workbench.action.closeActiveEditor");
    EXPECT_EQ(resolved(table, "ctrl+w", context({"activeEditorGroupEmpty"})),
              "workbench.action.closeActiveEditor");
    EXPECT_EQ(resolved(table, "ctrl+w", context({"multipleEditorGroups"})),
              "workbench.action.closeActiveEditor");
    EXPECT_EQ(
        resolved(table, "ctrl+w", context({"activeEditorGroupEmpty", "multipleEditorGroups"})),
        "workbench.action.closeGroup");
}

TEST(DefaultKeybindingsTest, NoTwoBindingsHaveTheSameKeyCommandAndCondition) {
    for (const KeyPlatform platform : platforms) {
        std::set<std::string> seen;
        for (const Keybinding& binding : default_keybindings(platform)) {
            const std::string identity = format_key_chord(binding.chord) + " " + binding.command +
                                         " " + (binding.when ? binding.when->canonical() : "");
            EXPECT_TRUE(seen.insert(identity).second) << identity;
        }
    }
}

TEST(DefaultKeybindingsTest, ABindingOnMacNeverUsesTheWindowsKeyAndControlIsOnlyForEmacsKeys) {
    for (const Keybinding& binding : default_keybindings(KeyPlatform::macos)) {
        for (const KeyStep& step : binding.chord.steps) {
            // VS Code binds full screen and Expand and Shrink Selection to Control and Command
            // together.
            const bool smart_select = binding.command.starts_with("editor.action.smartSelect.");
            if (binding.command != "workbench.action.toggleFullScreen" && !smart_select) {
                EXPECT_FALSE(step.modifiers.meta && step.modifiers.ctrl) << binding.command;
            }
            // VS Code binds Back and Forward to Control and the minus key, and Go to Line to
            // Control and G; Expand and Shrink Selection have Control and Shift with the arrows.
            const bool history = binding.command == "workbench.action.navigateBack" ||
                                 binding.command == "workbench.action.navigateForward" ||
                                 binding.command == "workbench.action.gotoLine" || smart_select;
            if (step.modifiers.ctrl && !history) {
                EXPECT_TRUE(step.key == Key::key_b || step.key == Key::key_f ||
                            step.key == Key::key_p || step.key == Key::key_n ||
                            step.key == Key::key_h || step.key == Key::key_d ||
                            step.key == Key::backspace || step.key == Key::delete_key)
                    << binding.command;
            }
        }
    }
}

TEST(DefaultKeybindingsTest, TheMenuShowsThePrimaryKeyOfACommandThatHasSeveral) {
    const KeybindingTable table(default_keybindings(KeyPlatform::linux));
    const auto label = [&](std::string_view command) {
        return format_key_chord(table.lookup_primary(command, ContextKeys())->chord);
    };
    EXPECT_EQ(label("workbench.action.showCommands"), "ctrl+shift+p");
    EXPECT_EQ(label("redo"), "ctrl+y");
    EXPECT_EQ(label("editor.action.clipboardPasteAction"), "ctrl+v");
}

TEST(DefaultKeybindingsTest, ZenModeHasVSCodesKeys) {
    for (const KeyPlatform platform : {KeyPlatform::linux, KeyPlatform::windows}) {
        const auto bindings = default_keybindings(platform);
        EXPECT_EQ(keys_of(bindings, "workbench.action.toggleZenMode"), chords({"ctrl+k z"}));
        EXPECT_EQ(keys_of(bindings, "workbench.action.exitZenMode"), chords({"escape escape"}));
    }
    const auto macos = default_keybindings(KeyPlatform::macos);
    EXPECT_EQ(keys_of(macos, "workbench.action.toggleZenMode"), chords({"cmd+k z"}));
    EXPECT_EQ(keys_of(macos, "workbench.action.exitZenMode"), chords({"escape escape"}));
}
