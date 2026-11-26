# 0017. Commands change settings by editing the settings file in place

- Status: Accepted
- Date: 2026-11-23

## Context

`SettingsStore` reads the user's `settings.json`, watches it, and never writes it. VS Code has many commands that change a setting: Toggle Auto Save, Color Theme, Render Whitespace, Render Control Characters, the multi-cursor modifier, the tab bar modes. In VS Code each one writes the user's `settings.json`, and the change is still there at the next start. Anvil has no way to make such a command's effect survive a restart.

The file is the user's own: JSON with comments, written and ordered by hand, often open in an editor. `core::Settings` accepts a key spelled flat (`"editor.fontSize": 14`) or nested (`"editor": {"fontSize": 14}`). `core::parse_jsonc` returns plain JSON and keeps no source positions, so it cannot say where a value is in the text.

VS Code changes the file by editing its text: `setProperty` in `src/vs/base/common/jsonEdit.ts` computes the edits that replace, insert or remove one property of the parsed tree, and `jsonEditingService.ts` applies them. Comments, order and formatting around the property stay as they were.

## Decision

A command that changes a setting edits the text of the user's settings file in place, as VS Code does.

- **The edit.** `core` gets a function that takes the text of a settings file, a dotted key and a value and returns the new text. It changes the value of the property that holds the key, whether spelled flat or nested, and otherwise appends a flat property `"key": value` as the last member of the root object, indented like the members before it. Nothing else in the text changes: comments, order, spacing and trailing commas stay. A missing or blank file becomes an object with the one property. Removing a property is the same function with no value.
- **The write.** The file is read and edited, and the file's bytes are read again just before the write; if they changed, the command fails and the user can run it again. The write is `platform::write_file`, which writes a temporary file next to the file and moves it into place, so a failure never leaves a truncated file.
- **One path for settings.** `SettingsStore` applies the text it wrote at once, so the window changes without the file watcher's delay, and the watcher's reload that follows finds the same settings and signals nothing.
- **When the file cannot be edited.** A file that does not parse, cannot be written, was changed between the read and the write, or is open in a tab of the window with unsaved changes is left as it is. The command then changes the setting for the running window only, through an override that `SettingsStore` keeps above the file, and the status bar says why the file was not changed ("Auto Save is on for this session; settings.json has a syntax error at line 4"). The override of a key lasts until a command changes that key again or the window closes.
- **Nothing else is written.** A command asks for the change; settings are not saved at startup, on exit or in the background.

## Consequences

- A command's effect is visible in `settings.json`, survives a restart and can be undone by editing the file, as in VS Code. The file stays the one source of truth, and the user's comments are not lost.
- `core` gets a JSON-with-comments tree parser that records the position of every value, and the edit built on it. It is tested against a reference: for random documents with comments and either spelling of a key, the result parses to the intended value, differs from the input only at the changed property, and keeps every comment.
- A write can race with an edit that the user makes in another program; comparing the bytes before replacing narrows the window and does not close it. The failure leaves the file intact and the command can be repeated.
- A window with the file open and unsaved changes refuses to write over them, and the session override keeps the command working.
- A session-only store of settings, with no writing, would lose the user's choice at the next start, which is not what a user who picked a theme expects, and would differ from VS Code. A separate state file read below `settings.json` would keep the user's file untouched, but the user could not see in `settings.json` what a command changed, and a key the user had also set there would silently override the command's choice. Both are rejected.
- The implementation is two pieces: the tree parser and the edit in `core` ([#189](https://github.com/djangbahevans/project-anvil/issues/189)), and `SettingsStore::change` with the session override and the status bar message ([#190](https://github.com/djangbahevans/project-anvil/issues/190)).
