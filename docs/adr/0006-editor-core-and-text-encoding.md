# 0006. Toolkit-independent editor core with UTF-8 buffer

- Status: Proposed
- Date: 2026-11-17

## Context

The text buffer, undo history, cursors and selections, and line layout are the performance-critical heart of the editor and must be testable without a GUI. Both the `vscode` API and the Language Server Protocol address text positions as zero-based line and UTF-16 code-unit column, while native code and most file formats favor UTF-8.

## Decision

- The editor core is a library with no dependency on Qt or any other UI code.
- The text buffer stores UTF-8 in a balanced tree structure (piece tree or B-tree rope) with cached line-break and UTF-16-length metadata per node, giving logarithmic edits and offset-to-position lookups.
- Positions at the extension and LSP boundaries are UTF-16 code-unit based. Conversion between UTF-8 byte offsets and UTF-16 columns happens in one boundary module, using the cached metadata.
- Files are decoded to UTF-8 on load and encoded back on save according to the detected or configured encoding.

## Consequences

- The core can be unit-tested and benchmarked headlessly.
- Memory per character is lower than a UTF-16 buffer for source code, which is mostly ASCII.
- Every position crossing the extension or LSP boundary pays a conversion cost, bounded by the metadata cache.
- The final choice between piece tree and rope is made by benchmarking on large files and recorded by amending this ADR.
