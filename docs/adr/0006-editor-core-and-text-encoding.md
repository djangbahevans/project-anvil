# 0006. Toolkit-independent editor core with a UTF-8 rope buffer

- Status: Accepted
- Date: 2026-11-17

## Context

The text buffer, cursors, undo history and line layout are the performance-critical heart of the editor and must be testable without a GUI. Both the `vscode` API and the Language Server Protocol address text positions as zero-based line and UTF-16 code-unit column, while native code and most file formats favor UTF-8.

Other parts of the editor need a stable view of the text while the user keeps typing: background syntax parsing, search, and sending document state to the extension host. Undo needs earlier versions of the text.

## Decision

- The editor core is a library with no dependency on Qt or any other UI code.
- `Text` is an immutable persistent rope: an AVL-balanced binary tree whose leaves hold UTF-8 chunks of at most 2 KiB. Every node caches the byte, UTF-16 and newline counts of its subtree. Nodes are shared between versions, so copying a `Text` is a snapshot that costs one pointer copy, stays valid while the text is edited, and is safe to read from any thread. Snapshots also serve as undo states.
- `TextBuffer` holds the current `Text`. An edit replaces it and returns a `TextChange` that describes the replaced range in bytes and in UTF-16 positions, for notifying the extension host and other observers.
- The text contains only `\n` line breaks. Loading and inserting convert `\r\n` and lone `\r`. The document layer records a file's line ending style, restores it when saving, and accounts for the width of the line ending in any offset it exposes to extensions.
- Offsets inside the core are byte offsets on character boundaries. Positions at the extension and LSP boundaries are `(line, UTF-16 column)`. Conversion in both directions descends the tree by the cached counts, then scans one chunk. A column inside a surrogate pair maps to the start of the pair; lines and columns beyond the text clamp to its end.
- An edit splits the tree at both ends of the replaced range, merges the chunks adjacent to the edit with the new text, and joins the parts. Merging keeps chunks from fragmenting under many small edits.
- The chunk size is the constant `rope::max_chunk_bytes`.

## Consequences

Measured with `bench/text_bench.cpp` (Linux, Clang release build, a WSL2 workstation with run-to-run variance of up to a few times; the orders of magnitude are what matter). Figures are for a 100 MB document of source-like text, compared with a plain `std::string`.

| Operation | Rope | `std::string` |
| --- | --- | --- |
| Insert a few bytes in the middle | 2 to 11 µs | 3.5 to 14 ms |
| Snapshot | about 5 ns | 6 to 43 ms (a copy) |
| Offset to line and column | 1 to 5 µs | 0.2 to 1.8 µs, with a line index that must be rebuilt after edits |
| Load | about 0.6 GB/s | not applicable |
| Heap used beyond the text | 13.5% | 0% |

- Snapshot cost is independent of document size, typing latency grows only logarithmically with it, and holding a snapshot for a background job adds no copying to an edit.
- Random access conversions cost microseconds, so code that converts many positions in order, such as sending semantic tokens, needs a sequential iteration API that does not descend the tree per position.
- Per-chunk overhead is nearly constant, so the chunk size sets the memory overhead: about 27% at 1 KiB, 13.5% at 2 KiB and 6.7% at 4 KiB, while edits and conversions slow by roughly 1.5 times per doubling. 2 KiB is chosen because every operation stays far below a keystroke's time budget. A leaf layout without a separate string allocation and a smaller shared-pointer header would lower the overhead at any chunk size.
- Content is held in memory; opening a very large file does not map it from disk.
- Files that are not valid UTF-8 are decoded on load and encoded on save according to the detected or configured encoding; `Text` accepts only valid UTF-8.
- A piece tree over an append-only original buffer would avoid copying the loaded file and keep undo states small, but it needs its own scheme for sharing versions across threads. The rope trades some load-time copying for free snapshots.
