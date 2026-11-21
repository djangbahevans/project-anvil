# Architecture Decision Records

Each ADR records one architectural decision: the context, the choice, and its consequences. An accepted ADR is not edited except to correct errors or to mark it superseded; a changed decision is a new ADR that supersedes the old one.

## Statuses

- **Proposed**: the direction is chosen but depends on an unvalidated assumption stated in the ADR.
- **Accepted**: in force.
- **Superseded by NNNN**: replaced by a later ADR.

## Adding an ADR

Copy [template.md](template.md) to `NNNN-short-title.md` using the next free number, and add a row to the index below.

## Index

| # | Title | Status |
| --- | --- | --- |
| [0001](0001-record-architecture-decisions.md) | Record architecture decisions | Accepted |
| [0002](0002-language-and-build-system.md) | C++23 with CMake and vcpkg | Accepted |
| [0003](0003-ui-toolkit-qt6-widgets.md) | Qt 6 Widgets for UI chrome | Accepted |
| [0004](0004-extension-host-node-child-process.md) | Run extensions in a Node child process | Accepted |
| [0005](0005-reuse-upstream-extension-host.md) | Reuse the upstream VS Code extension host | Accepted |
| [0006](0006-editor-core-and-text-encoding.md) | Toolkit-independent editor core with a UTF-8 rope buffer | Accepted |
| [0007](0007-extension-registry-open-vsx.md) | Open VSX as the extension registry | Proposed |
| [0008](0008-cross-platform-from-the-start.md) | Cross-platform from the start, Linux first | Accepted |
| [0009](0009-scope-and-non-goals.md) | Scope and non-goals | Accepted |
| [0010](0010-syntax-highlighting.md) | tree-sitter for syntax highlighting | Proposed |
| [0011](0011-document-model.md) | Document model with edit-based undo history | Accepted |
| [0012](0012-editing-commands-in-core.md) | Editing commands in the core, painting in the view | Accepted |
| [0013](0013-pcre2-for-search.md) | PCRE2 for regular expressions and case folding in search | Accepted |
| [0014](0014-workbench-layout.md) | Workbench window built from fixed parts in a purpose-built layout | Proposed |
