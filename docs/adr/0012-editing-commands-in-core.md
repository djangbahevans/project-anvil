# 0012. Editing commands in the core, painting in the view

- Status: Accepted
- Date: 2026-11-17

## Context

The behavior of an editor, what typing, backspace, arrow keys, Home and End do with several cursors, which keystrokes undo together, and where the caret lands after a vertical move across short lines, is intricate and easy to get subtly wrong. It must be testable without a window, and it must not depend on the UI toolkit ([0003](0003-ui-toolkit-qt6-widgets.md)).

## Decision

- `core::EditController` implements the editing commands against a `Document`: typing, paste, newline with indentation, deleting, cursor motion by character, line, word and document, selecting, adding cursors, and undo and redo. Every command acts on all selections, so multiple cursors need no special case. A command returns an `EditOutcome` that carries the `DocumentChange`, if the text changed, and whether the selections changed.
- The controller keeps only the state that is not part of the document: the column that vertical movement returns to, and the identity of the last joinable edit. Consecutive typing, and consecutive deletion in one direction, join into one undo step through the document's undo groups ([0011](0011-document-model.md)). Moving the cursor, editing elsewhere, undoing, and typing a line break each end a group.
- `ui::EditorView` paints the document and translates keyboard, mouse and input method events into controller commands. It holds no editing logic. Only the visible lines are laid out with `QTextLayout`; UTF-16 columns of the document map directly onto its string indices.
- The view reports what the user did through signals: `text_changed` with the `DocumentChange`, and `selections_changed`. The window forwards these to the extension host, and passes edits that extensions make back to the view.
- Characters are Unicode code points. Combining marks and emoji sequences count as several characters for motion and deletion until grapheme cluster handling is added.

## Consequences

- Editing behavior has unit tests that run in milliseconds without a display; the view's tests only need to show that events reach the controller and that the result is painted.
- A second front end, such as a terminal view or a test driver, can reuse the commands.
- The layouts of the most recently used lines are cached, so that painting and hit testing do not lay out the same line again. Laying out one very long line is slow, so the view cuts lines off at 5000 characters.
- Cursor motion over combining sequences differs from most editors until graphemes are supported.
