# 0011. Document model with edit-based undo history

- Status: Accepted
- Date: 2026-11-17

## Context

A document is more than its text: it has selections, an undo history, a saved state, and the file format details (line endings, byte order mark) needed to write it back unchanged. The extension host and other observers need each mutation described as a list of changes they can apply to their own copy of the text, with a version number. Multi-cursor editing makes one user action many edits, and consecutive typing should undo as one unit.

## Decision

- `Document` owns a `TextBuffer`, the selections, the undo and redo stacks, and the line ending and byte order mark of the file it was loaded from. `from_file_bytes` and `to_file_bytes` restore the format on save; the in-memory text always uses `\n` ([0006](0006-editor-core-and-text-encoding.md)).
- An edit batch is a set of non-overlapping replacements in coordinates of the current text, given in any order. It is validated as a whole before anything changes and applied from the end of the text to the start. The `DocumentChange` it returns lists the changes in that order, each described against the text left by the ones before it, so an observer applies them one after another. Undo and redo return their changes the same way. Every change, including undo and redo, increments the document version.
- Each batch is one undo step holding its forward replacements and the inverse replacements that restore the previous text. Removed text is kept as `Text` slices that share storage with the rope, so the history of a large deletion costs only the tree nodes it touches. Undoing replays inverse replacements through the normal edit path, which yields exact change descriptions.
- Steps carry a group. An edit that sets `join_previous` joins the previous step's group, and undo and redo move a whole group at a time. The input handler decides which edits join, for example consecutive typing; the document applies no heuristics. A save seals the current group, so later edits never join it.
- Selections are byte offsets on character boundaries. A selection moves with edits: an offset inside replaced text moves to the end of its replacement, a caret follows text inserted at it, and a range grows when text is inserted at either edge. An edit may set the selections that follow it instead. Undo restores the selections from before the step and redo the selections from after it. Selections that overlap, or a caret that touches a selection, are merged.
- A document is dirty when the identity of its current undo step differs from the one at the last save, so undoing back to the saved state is clean and reverting an edit by hand is not.
- `extension_offset` converts a byte offset to the UTF-16 offset that extensions use, counting each line break at the width of the file's line ending.

## Consequences

- Observers need no diffing: every mutation arrives as an ordered change list with positions in bytes and UTF-16 columns.
- History memory grows with the size of the inserted and removed text, not the size of the document. The history is not bounded; a limit on the number of steps can be added without changing the interface.
- Redo history is discarded by any new edit, as in most editors.
- Only UTF-8 files are modeled. Other encodings need a decoding layer in front of `from_file_bytes` and a matching one behind `to_file_bytes`.
- Changing a document's line ending style is not an edit and does not enter the history.
- Snapshots of the text, which are cheap, remain the right tool for background readers; the history is only for undo.
