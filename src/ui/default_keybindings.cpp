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

    // Find.
    {"actions.find", "", {"ctrl+f"}, {"ctrl+f"}, {"cmd+f"}},
    {"editor.action.startFindReplaceAction", "", {"ctrl+h"}, {"ctrl+h"}, {"cmd+alt+f"}},
    {"editor.action.nextMatchFindAction", "editorFocus", {"f3"}, {"f3"}, {"cmd+g", "f3"}},
    {"editor.action.previousMatchFindAction",
     "editorFocus",
     {"shift+f3"},
     {"shift+f3"},
     {"cmd+shift+g", "shift+f3"}},

    // Files and editors.
    {"workbench.action.files.openFile", "", {"ctrl+o"}, {"ctrl+o"}, {"cmd+o"}},
    {"workbench.action.files.save", "", {"ctrl+s"}, {"ctrl+s"}, {"cmd+s"}},
    {"workbench.action.files.saveAs", "", {"ctrl+shift+s"}, {"ctrl+shift+s"}, {"cmd+shift+s"}},
    {"saveAll", "", {}, {"ctrl+k s"}, {"cmd+alt+s"}},
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
