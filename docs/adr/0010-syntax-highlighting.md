# 0010. tree-sitter for syntax highlighting

- Status: Accepted
- Date: 2026-11-17

## Context

Syntax highlighting must be incremental to keep typing latency low on large files. VS Code themes and extension-contributed grammars are expressed in TextMate scope names, so compatibility with existing themes requires mapping highlighting output onto those scopes.

## Decision

Built-in highlighting uses tree-sitter, which parses incrementally and is a C library. Highlight query captures are mapped to TextMate-style scope names so that VS Code color themes apply unchanged. Semantic tokens from language servers and extensions layer over the syntactic highlighting.

Support for extension-contributed TextMate grammars is deferred. The highlighter interface is defined so a TextMate tokenizer can be added as a second implementation.

## Consequences

- Highlighting cost scales with the size of an edit rather than the size of the file.
- Languages without a tree-sitter grammar are unhighlighted until the TextMate implementation exists.
- Capture-to-scope mappings need maintenance per grammar to keep themes visually consistent.
