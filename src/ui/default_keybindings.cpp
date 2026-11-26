#include "anvil/ui/default_keybindings.hpp"

#include <ranges>

namespace anvil::ui {
namespace {

using Keys = std::initializer_list<const char*>;

struct Row {
    const char* command;
    // The condition under which the binding applies; empty for always.
    const char* when;
    Keys linux;
    Keys windows;
    Keys macos;
};

constexpr const char* in_editor = "editorTextFocus";
constexpr const char* in_explorer = "filesExplorerFocus && explorerViewletVisible && !inputFocus";

// A row lists the primary key first and the secondary keys after it. A menu shows the last binding
// of a command that applies, so the bindings are added in the opposite order, as VS Code orders
// the secondary keys of a command before its primary one.
//
// A platform's keys are written for that platform: `ctrl` is the Control key, and `cmd` is the
// Command key on macOS. Where VS Code writes `CtrlCmd`, this is `ctrl` on Windows and Linux and
// `cmd` on macOS.
const Row rows[] = {
    // The editor.
    {"undo", in_editor, {"ctrl+z"}, {"ctrl+z"}, {"cmd+z"}},
    {"redo", in_editor, {"ctrl+y", "ctrl+shift+z"}, {"ctrl+y", "ctrl+shift+z"}, {"cmd+shift+z"}},
    {"cursorLeft", in_editor, {"left"}, {"left"}, {"left", "ctrl+b"}},
    {"cursorRight", in_editor, {"right"}, {"right"}, {"right", "ctrl+f"}},
    {"cursorUp", in_editor, {"up"}, {"up"}, {"up", "ctrl+p"}},
    {"cursorDown", in_editor, {"down"}, {"down"}, {"down", "ctrl+n"}},
    {"cursorHome", in_editor, {"home"}, {"home"}, {"home", "cmd+left"}},
    {"cursorEnd", in_editor, {"end"}, {"end"}, {"end", "cmd+right"}},
    {"deleteLeft",
     in_editor,
     {"backspace", "shift+backspace"},
     {"backspace", "shift+backspace"},
     {"backspace", "shift+backspace", "ctrl+h", "ctrl+backspace"}},
    {"deleteRight", in_editor, {"delete"}, {"delete"}, {"delete", "ctrl+d", "ctrl+delete"}},
    {"editor.action.selectAll", in_editor, {"ctrl+a"}, {"ctrl+a"}, {"cmd+a"}},
    {"editor.action.clipboardCutAction",
     in_editor,
     {"ctrl+x"},
     {"ctrl+x", "shift+delete"},
     {"cmd+x"}},
    {"editor.action.clipboardCopyAction",
     in_editor,
     {"ctrl+c"},
     {"ctrl+c", "ctrl+insert"},
     {"cmd+c"}},
    {"editor.action.clipboardPasteAction",
     in_editor,
     {"ctrl+v", "shift+insert"},
     {"ctrl+v", "shift+insert"},
     {"cmd+v"}},

    // Several cursors.
    {"editor.action.addSelectionToNextFindMatch", "editorFocus", {"ctrl+d"}, {"ctrl+d"}, {"cmd+d"}},
    {"editor.action.selectHighlights",
     "editorFocus",
     {"ctrl+shift+l"},
     {"ctrl+shift+l"},
     {"cmd+shift+l"}},
    {"editor.action.selectAllMatches",
     "editorFocus && findWidgetVisible",
     {"alt+enter"},
     {"alt+enter"},
     {"alt+enter"}},

    {"editor.action.insertCursorAbove",
     in_editor,
     {"shift+alt+up", "ctrl+shift+up"},
     {"ctrl+alt+up"},
     {"cmd+alt+up"}},
    {"editor.action.insertCursorBelow",
     in_editor,
     {"shift+alt+down", "ctrl+shift+down"},
     {"ctrl+alt+down"},
     {"cmd+alt+down"}},
    {"editor.action.insertCursorAtEndOfEachLineSelected",
     in_editor,
     {"shift+alt+i"},
     {"shift+alt+i"},
     {"shift+alt+i"}},

    {"workbench.action.toggleEditorGroupLayout",
     "",
     {"shift+alt+0"},
     {"shift+alt+0"},
     {"cmd+alt+0"}},

    // Tabs and groups.
    {"workbench.action.nextEditor",
     "",
     {"ctrl+pagedown"},
     {"ctrl+pagedown"},
     {"cmd+alt+right", "cmd+shift+]"}},
    {"workbench.action.previousEditor",
     "",
     {"ctrl+pageup"},
     {"ctrl+pageup"},
     {"cmd+alt+left", "cmd+shift+["}},
    {"workbench.action.nextEditorInGroup",
     "",
     {"ctrl+k ctrl+pagedown"},
     {"ctrl+k ctrl+pagedown"},
     {"cmd+k cmd+alt+right"}},
    {"workbench.action.previousEditorInGroup",
     "",
     {"ctrl+k ctrl+pageup"},
     {"ctrl+k ctrl+pageup"},
     {"cmd+k cmd+alt+left"}},
    {"workbench.action.focusFirstEditorGroup", "", {"ctrl+1"}, {"ctrl+1"}, {"cmd+1"}},
    {"workbench.action.focusSecondEditorGroup", "", {"ctrl+2"}, {"ctrl+2"}, {"cmd+2"}},
    {"workbench.action.focusThirdEditorGroup", "", {"ctrl+3"}, {"ctrl+3"}, {"cmd+3"}},
    {"workbench.action.focusFourthEditorGroup", "", {"ctrl+4"}, {"ctrl+4"}, {"cmd+4"}},
    {"workbench.action.focusFifthEditorGroup", "", {"ctrl+5"}, {"ctrl+5"}, {"cmd+5"}},
    {"workbench.action.focusSixthEditorGroup", "", {"ctrl+6"}, {"ctrl+6"}, {"cmd+6"}},
    {"workbench.action.focusSeventhEditorGroup", "", {"ctrl+7"}, {"ctrl+7"}, {"cmd+7"}},
    {"workbench.action.focusEighthEditorGroup", "", {"ctrl+8"}, {"ctrl+8"}, {"cmd+8"}},
    {"workbench.action.focusLeftGroup",
     "",
     {"ctrl+k ctrl+left"},
     {"ctrl+k ctrl+left"},
     {"cmd+k cmd+left"}},
    {"workbench.action.focusRightGroup",
     "",
     {"ctrl+k ctrl+right"},
     {"ctrl+k ctrl+right"},
     {"cmd+k cmd+right"}},
    {"workbench.action.focusAboveGroup",
     "",
     {"ctrl+k ctrl+up"},
     {"ctrl+k ctrl+up"},
     {"cmd+k cmd+up"}},
    {"workbench.action.focusBelowGroup",
     "",
     {"ctrl+k ctrl+down"},
     {"ctrl+k ctrl+down"},
     {"cmd+k cmd+down"}},

    // Lines.
    {"editor.action.copyLinesUpAction",
     in_editor,
     {"ctrl+alt+shift+up"},
     {"alt+shift+up"},
     {"alt+shift+up"}},
    {"editor.action.copyLinesDownAction",
     in_editor,
     {"ctrl+alt+shift+down"},
     {"alt+shift+down"},
     {"alt+shift+down"}},
    {"editor.action.moveLinesUpAction", in_editor, {"alt+up"}, {"alt+up"}, {"alt+up"}},
    {"editor.action.moveLinesDownAction", in_editor, {"alt+down"}, {"alt+down"}, {"alt+down"}},

    // Find.
    {"actions.find", "", {"ctrl+f"}, {"ctrl+f"}, {"cmd+f"}},
    {"editor.action.startFindReplaceAction", "", {"ctrl+h"}, {"ctrl+h"}, {"cmd+alt+f"}},
    {"editor.action.nextMatchFindAction", "editorFocus", {"f3"}, {"f3"}, {"cmd+g", "f3"}},
    {"editor.action.previousMatchFindAction",
     "editorFocus",
     {"shift+f3"},
     {"shift+f3"},
     {"cmd+shift+g", "shift+f3"}},

    {"workbench.action.gotoLine", "", {"ctrl+g"}, {"ctrl+g"}, {"ctrl+g"}},
    {"editor.action.jumpToBracket",
     in_editor,
     {"ctrl+shift+\\"},
     {"ctrl+shift+\\"},
     {"cmd+shift+\\"}},
    {"editor.action.commentLine", in_editor, {"ctrl+/"}, {"ctrl+/"}, {"cmd+/"}},
    {"editor.action.addCommentLine",
     in_editor,
     {"ctrl+k ctrl+c"},
     {"ctrl+k ctrl+c"},
     {"cmd+k cmd+c"}},
    {"editor.action.removeCommentLine",
     in_editor,
     {"ctrl+k ctrl+u"},
     {"ctrl+k ctrl+u"},
     {"cmd+k cmd+u"}},
    {"editor.action.blockComment", in_editor, {"ctrl+shift+a"}, {"shift+alt+a"}, {"shift+alt+a"}},
    {"editor.action.smartSelect.expand",
     in_editor,
     {"shift+alt+right"},
     {"shift+alt+right"},
     {"ctrl+cmd+shift+right", "ctrl+shift+right"}},
    {"editor.action.smartSelect.shrink",
     in_editor,
     {"shift+alt+left"},
     {"shift+alt+left"},
     {"ctrl+cmd+shift+left", "ctrl+shift+left"}},
    {"workbench.action.toggleZenMode", "", {"ctrl+k z"}, {"ctrl+k z"}, {"cmd+k z"}},
    {"workbench.action.exitZenMode",
     "inZenMode && editorTextFocus && !editorHasMultipleSelections",
     {"escape escape"},
     {"escape escape"},
     {"escape escape"}},
    {"editor.action.toggleWordWrap", "", {"alt+z"}, {"alt+z"}, {"alt+z"}},
    {"workbench.action.openSettings", "", {"ctrl+,"}, {"ctrl+,"}, {"cmd+,"}},
    {"workbench.action.openGlobalKeybindings",
     "",
     {"ctrl+k ctrl+s"},
     {"ctrl+k ctrl+s"},
     {"cmd+k cmd+s"}},
    {"workbench.action.keybindingsReference",
     "",
     {"ctrl+k ctrl+r"},
     {"ctrl+k ctrl+r"},
     {"cmd+k cmd+r"}},
    {"workbench.action.zoomIn",
     "",
     {"ctrl+=", "ctrl+shift+=", "ctrl+numpad_add"},
     {"ctrl+=", "ctrl+shift+=", "ctrl+numpad_add"},
     {"cmd+=", "cmd+shift+=", "cmd+numpad_add"}},
    {"workbench.action.zoomOut",
     "",
     {"ctrl+-", "ctrl+numpad_subtract"},
     {"ctrl+-", "ctrl+shift+-", "ctrl+numpad_subtract"},
     {"cmd+-", "cmd+shift+-", "cmd+numpad_subtract"}},
    {"workbench.action.zoomReset", "", {"ctrl+numpad0"}, {"ctrl+numpad0"}, {"cmd+numpad0"}},
    {"workbench.action.newWindow", "", {"ctrl+shift+n"}, {"ctrl+shift+n"}, {"cmd+shift+n"}},
    {"workbench.action.reopenClosedEditor",
     "",
     {"ctrl+shift+t"},
     {"ctrl+shift+t"},
     {"cmd+shift+t"}},
    {"workbench.action.selectTheme", "", {"ctrl+k ctrl+t"}, {"ctrl+k ctrl+t"}, {"cmd+k cmd+t"}},
    // Navigation history.
    {"workbench.action.navigateBack",
     "",
     {"ctrl+alt+-", "browserback"},
     {"alt+left", "browserback"},
     {"ctrl+-", "browserback"}},
    {"workbench.action.navigateForward",
     "",
     {"ctrl+shift+-", "browserforward"},
     {"alt+right", "browserforward"},
     {"ctrl+shift+-", "browserforward"}},
    {"workbench.action.navigateToLastEditLocation",
     "",
     {"ctrl+k ctrl+q"},
     {"ctrl+k ctrl+q"},
     {"cmd+k cmd+q"}},
    // Files and editors.
    {"workbench.action.files.openFile", "", {"ctrl+o"}, {"ctrl+o"}, {"cmd+o"}},
    {"workbench.action.files.save", "", {"ctrl+s"}, {"ctrl+s"}, {"cmd+s"}},
    {"workbench.action.files.saveAs", "", {"ctrl+shift+s"}, {"ctrl+shift+s"}, {"cmd+shift+s"}},
    {"saveAll", "", {}, {"ctrl+k s"}, {"cmd+alt+s"}},
    {"workbench.action.editor.changeLanguageMode", "", {"ctrl+k m"}, {"ctrl+k m"}, {"cmd+k m"}},
    {"workbench.action.closeActiveEditor", "", {"ctrl+w"}, {"ctrl+f4", "ctrl+w"}, {"cmd+w"}},
    // After closing the editor, because it is the later of two bindings of a key that apply to an
    // empty group.
    {"workbench.action.closeGroup",
     "activeEditorGroupEmpty && multipleEditorGroups",
     {"ctrl+w"},
     {"ctrl+f4", "ctrl+w"},
     {"cmd+w"}},
    {"workbench.action.splitEditor", "", {"ctrl+\\"}, {"ctrl+\\"}, {"cmd+\\"}},
    {"workbench.action.quit", "", {"ctrl+q"}, {}, {"cmd+q"}},

    // The explorer. A key that has an editor command too, such as Delete, is the explorer's only
    // while the tree has the focus.
    {"renameFile",
     "filesExplorerFocus && explorerViewletVisible && !inputFocus && !explorerResourceIsRoot",
     {"f2"},
     {"f2"},
     {"enter"}},
    {"moveFileToTrash",
     "filesExplorerFocus && explorerViewletVisible && !inputFocus && "
     "explorerResourceMoveableToTrash",
     {"delete"},
     {"delete"},
     {"cmd+backspace"}},
    {"deleteFile", in_explorer, {"shift+delete"}, {"shift+delete"}, {"cmd+alt+backspace"}},
    {"deleteFile",
     "filesExplorerFocus && explorerViewletVisible && !inputFocus && "
     "!explorerResourceMoveableToTrash",
     {"delete"},
     {"delete"},
     {"cmd+backspace"}},

    // The window.
    {"workbench.action.showCommands",
     "",
     {"ctrl+shift+p", "f1"},
     {"ctrl+shift+p", "f1"},
     {"cmd+shift+p", "f1"}},
    {"workbench.action.toggleSidebarVisibility", "", {"ctrl+b"}, {"ctrl+b"}, {"cmd+b"}},
    {"workbench.action.togglePanel", "", {"ctrl+j"}, {"ctrl+j"}, {"cmd+j"}},
    {"workbench.action.toggleFullScreen", "", {"f11"}, {"f11"}, {"ctrl+cmd+f"}},
};

} // namespace

std::vector<core::Keybinding> default_keybindings(core::KeyPlatform platform) {
    std::vector<core::Keybinding> bindings;
    for (const Row& row : rows) {
        const Keys& keys = platform == core::KeyPlatform::macos     ? row.macos
                           : platform == core::KeyPlatform::windows ? row.windows
                                                                    : row.linux;
        for (const char* key : std::views::reverse(keys)) {
            core::Keybinding binding;
            binding.chord = *core::parse_key_chord(key);
            binding.command = row.command;
            if (row.when[0] != '\0') {
                binding.when = *core::WhenExpression::parse(row.when);
            }
            binding.source = core::KeybindingSource::builtin;
            bindings.push_back(std::move(binding));
        }
    }
    return bindings;
}

} // namespace anvil::ui
