# 0018. Word wrap breaks lines into rows by columns, computed in the core

- Status: Accepted
- Date: 2026-11-26

## Context

With `editor.wordWrap` on, a line of the document is shown on several rows. The scrollbar, the vertical position of every line, vertical caret motion and hit testing all depend on how many rows each line takes, including lines that are not shown. `ui::EditorView` places line `n` at `n` times the line height, and `core::EditController` moves up and down by lines of the document ([0012](0012-editing-commands-in-core.md)). Selections belong to the document, and two groups can show one document at different widths.

VS Code computes the breaks from the text, not from a laid out font, by default (`editor.wrappingStrategy: "simple"`, `src/vs/editor/common/viewModel/monospaceLineBreaksComputer.ts` of the pinned release): it counts columns, with tab stops and a width for full-width characters, breaks around sets of characters (`wordWrapBreakBeforeCharacters`, `wordWrapBreakAfterCharacters`, ideographs), and indents the continuation rows like the line (`editor.wrappingIndent: "same"`).

View ▸ Word Wrap in VS Code (`editor.action.toggleWordWrap`, `src/vs/workbench/contrib/codeEditor/browser/toggleWordWrap.ts`) does not write a setting. It keeps an override for the document in memory: the first use wraps a document that is not wrapped, or the reverse, and the next use removes the override.

## Decision

- **Breaks.** `core`, without Qt, computes the row breaks of a line from its text, the tab size, the wrapping column and the width of a full-width character, with the rules of VS Code's monospace computer and its default break characters and wrapping indent. The results are checked against results recorded from the pinned VS Code source.
- **Wrapping column.** It follows `editor.wordWrap`: `on` is the number of character advances that fit the text area, `wordWrapColumn` is `editor.wordWrapColumn` (80), and `bounded` is the smaller of the two. `off` wraps nothing.
- **Row map.** For each wrapped view, `core` keeps the number of rows of every line and maps between lines and rows. All of it is computed when the document is shown or when the column, the tab size or the character widths change. An edit computes only the lines that it changes. Break offsets are computed for the lines in use.
- **Painting.** The view lays out each row of a wrapped line on its own. With wrapping off there is no row map, and lines are laid out as they are without this decision.
- **Carets.** Selections stay offsets in the document. `EditController` takes the row map of its view, and moves up, down, by page, to the start and end of a line and adds cursors by rows and visible columns, as VS Code does. The offset where a line breaks is both the end of one row and the start of the next; the controller keeps which of the two a caret is on, beside the column that vertical motion returns to.
- **Toggle.** `editor.action.toggleWordWrap` keeps an override for the document in memory, which every view of the document follows until the document is closed. It writes no setting. `editor.wordWrap` in the settings file is the default for every document.

## Consequences

- Rows are known for the whole document, so the scrollbar is exact and does not move as lines come into view. The cost is a pass over the text when the wrapping column changes. A measurement of a simplified form of the rules (one greedy pass over the storage chunks of the text at 80 columns, `-O3`, an Intel Core Ultra 9 275HX under WSL2) takes 13 to 17 ms for 100,000 lines of 55 characters, 27 ms for 100,000 lines of 170, about 95 ms for a million lines, and 17 to 30 ms for one line of 10 MB. Summing the counts of a million lines takes 0.25 ms. The full rules cost more; the implementation measures them, and they must not make a change of width noticeably slow in documents of up to 100,000 lines. Dragging a splitter over a document of a million lines lags.
- The counts take four bytes a line for each wrapped view.
- An edit in a very long wrapped line computes all of its rows again, which takes about 5 ms for a line of a million characters.
- Breaks are exact for a monospace font. A proportional font, or glyphs whose width differs from the counted columns, leaves rows short or makes them run past the edge, where horizontal scrolling reaches them. VS Code's simple strategy behaves the same way.
- The breaks and the motion by rows have tests without a window.
- Wrapping with `QTextLayout` would fit any font exactly, but the number of rows of a line would take a layout of it, and one layout costs 1.7 ms for 5,000 characters: too much for a whole document. Its breaks would also differ from VS Code's.
- Counting rows only for the lines in view, and estimating the others, would make the scrollbar and the scroll position shift while scrolling. The measured cost of counting every line does not call for it.
- A row map in `ui` would put vertical motion in the view, against [0012](0012-editing-commands-in-core.md).
- A toggle that writes `editor.wordWrap` ([0017](0017-commands-change-settings-by-editing-the-file.md)) would keep the choice across restarts, but would wrap or unwrap every file when the user wants it for one, and would differ from VS Code.
- VS Code's Markdown extension turns wrapping on for Markdown with a language-specific default (`[markdown]`). Anvil reads no language-specific settings, so Markdown follows `editor.wordWrap` like every other language.
- `editor.wrappingStrategy`, `wrappingIndent`, the break character settings, `wordWrapIndicator` and the override settings (`wordWrapOverride1` and `2`) are not read.
