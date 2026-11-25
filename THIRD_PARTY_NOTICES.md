# Third-party notices

Anvil includes the following material from other projects. Each keeps its own license.

## Visual Studio Code default color themes

- **What:** the color theme files in `resources/themes/`, copied unchanged, and `resources/themes/themes.json`, which lists them and is derived from the extension's manifest.
- **From:** the built-in `theme-defaults` extension of Visual Studio Code (https://github.com/microsoft/vscode, `extensions/theme-defaults`), at the release pinned in `scripts/fetch-vscode.sh`.
- **Copyright:** Copyright (c) 2015 - present Microsoft Corporation.
- **License:** MIT. The full text is in `resources/themes/LICENSE-vscode.txt` and is installed with the themes.

## Codicons

- **What:** the icon font `resources/codicons/codicon.ttf`, copied unchanged and embedded in the editor, and the table of its icon names and code points in `src/ui/codicon_table.hpp`, which is generated from the font's `codicon.csv`.
- **From:** the `@vscode/codicons` package (https://github.com/microsoft/vscode-codicons), version 0.0.46-49, as installed in the checkout of Visual Studio Code that `scripts/fetch-vscode.sh` pins.
- **Copyright:** Copyright (c) Microsoft Corporation.
- **License:** Creative Commons Attribution 4.0 International (CC BY 4.0, https://creativecommons.org/licenses/by/4.0/). The full text is in `resources/codicons/LICENSE-codicons.txt` and is installed with the editor's documentation. The font is not modified.

## Visual Studio Code menus

- **What:** the titles, mnemonics, order, grouping and `when` clauses of the menu bar in `src/workbench/include/anvil/workbench/menu_table.hpp`, which is generated from the menus that Visual Studio Code registers.
- **From:** Visual Studio Code (https://github.com/microsoft/vscode), at the release pinned in `scripts/fetch-vscode.sh`.
- **Copyright:** Copyright (c) Microsoft Corporation.
- **License:** MIT. The text is in `resources/themes/LICENSE-vscode.txt`.

## Visual Studio Code languages

- **What:** the ids, names, file extensions, file names, file name patterns, first line expressions and mime types of the languages in `src/core/include/anvil/core/language_table.hpp`, which is generated from the `contributes.languages` of the built-in extensions of Visual Studio Code and from its plain text language, and the cases in `tests/core/data/language_cases.json` that record what its language service decides for them. The comment tokens of the languages in `src/core/include/anvil/core/language_comments_table.hpp` are generated from the `comments` of the language configuration files that those contributions name.
- **From:** Visual Studio Code (https://github.com/microsoft/vscode), at the release pinned in `scripts/fetch-vscode.sh`.
- **Copyright:** Copyright (c) Microsoft Corporation.
- **License:** MIT. The text is in `resources/themes/LICENSE-vscode.txt`.

## tree-sitter

- **What:** the runtime library (`lib/src`, `lib/include`), compiled into the core library when `.deps/tree-sitter` exists. Nothing is copied into this repository. The runtime includes a few headers of ICU in `lib/src/unicode`, (Copyright (c) 1991-2019 Unicode, Inc.) under the Unicode license that the `LICENSE` file next to them holds.
- **From:** https://github.com/tree-sitter/tree-sitter, release v0.27.2, fetched and checked by `scripts/fetch-tree-sitter.sh`.
- **Copyright:** Copyright (c) 2018 Max Brunsfeld.
- **License:** MIT.

## tree-sitter-json

- **What:** the JSON grammar (`src/parser.c`) and its `queries/highlights.scm`, compiled and embedded into the core library when `.deps/tree-sitter` exists. Nothing is copied into this repository.
- **From:** https://github.com/tree-sitter/tree-sitter-json, release v0.24.8, fetched and checked by `scripts/fetch-tree-sitter.sh`.
- **Copyright:** Copyright (c) 2014 Max Brunsfeld.
- **License:** MIT.

## tree-sitter-c

- **What:** the C grammar (`src/parser.c`) and its `queries/highlights.scm`, compiled and embedded into the core library when `.deps/tree-sitter` exists. Nothing is copied into this repository.
- **From:** https://github.com/tree-sitter/tree-sitter-c, release v0.24.1, fetched and checked by `scripts/fetch-tree-sitter.sh`.
- **Copyright:** Copyright (c) 2014 Max Brunsfeld.
- **License:** MIT.

## tree-sitter-cpp

- **What:** the C++ grammar (`src/parser.c`, `src/scanner.c`) and its `queries/highlights.scm`, compiled and embedded into the core library when `.deps/tree-sitter` exists. Nothing is copied into this repository.
- **From:** https://github.com/tree-sitter/tree-sitter-cpp, release v0.23.4, fetched and checked by `scripts/fetch-tree-sitter.sh`.
- **Copyright:** Copyright (c) 2014 Max Brunsfeld.
- **License:** MIT.
