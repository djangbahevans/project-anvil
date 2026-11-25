# Adding a tree-sitter grammar

Syntax highlighting runs a tree-sitter grammar and its `highlights.scm` query ([0010](../adr/0010-syntax-highlighting.md)). A language is added in five steps. `tree-sitter-json` is the worked example for a small grammar, and `tree-sitter-c` and `tree-sitter-cpp` for one with Anvil's own patterns and for one that builds on another.

## 1. Pin the grammar

Add a line to `SOURCES` in `scripts/fetch-tree-sitter.sh`: a name, the URL of the release archive of a tag, and the archive's SHA-256.

```sh
curl -sL <url> | sha256sum
```

Use a tagged release whose `src/parser.c` was generated with an ABI that the pinned runtime supports (the runtime refuses a grammar it cannot load, which `TreeSitterHighlighter::create` reports). The archive is unpacked into `.deps/tree-sitter/<name>/`. Read the grammar's `LICENSE` and add the source to `THIRD_PARTY_NOTICES.md` with its version, copyright and license.

## 2. Build it

Add `anvil_add_tree_sitter_grammar(<name>)` to the `ANVIL_HAVE_TREE_SITTER` block of `src/core/CMakeLists.txt`. It compiles `src/parser.c` and `src/scanner.c` (when there is one) without warnings and embeds `queries/highlights.scm` as `anvil::core::grammar_queries::<name>`. A grammar with a scanner in C++ (`scanner.cc`) needs that file added by hand. A grammar that builds on another has its patterns too: `anvil_add_tree_sitter_grammar(cpp INHERIT c)` puts the query of C in the query of C++, after the C++ patterns, which are the more specific ones.

The grammars' own queries are written for the highlighters of editors that differ in which of two patterns on the same node wins; here the first one does. Where a query is too coarse for the scopes that themes style (one `@keyword` for `if` and for `const`, which VS Code's grammars scope as `keyword.control` and `storage.modifier`), add `src/core/queries/<name>.scm`: its patterns come before the grammar's, so they decide the nodes they name, and their captures are the scope names without the language (`@keyword.control`). A capture that every node of a kind has and that VS Code would not scope (`@variable` on each identifier) is left without a row, so that a more specific pattern further down still applies to the node. Predicates: `#match?` and `#not-match?` are evaluated with PCRE2; a query that uses any other predicate makes `TreeSitterHighlighter::create` fail, because a pattern whose condition is ignored would match what it excludes. Implement the predicate in `tree_sitter_highlighter.cpp` before adding such a grammar.

## 3. Map the captures

Add a function to `capture_scopes.cpp` that returns the table of the language: one `CaptureScope{capture, scope}` per capture name of its query that should be styled, with the scope that its TextMate grammar uses (`string.quoted.double.json`, `keyword.control.cpp`). A row names a capture and the captures below it (`string` also serves `string.escape`); the longest row wins. Read the capture names from `queries/highlights.scm` and the overlay; a name with no row has no scope and its text keeps the enclosing style. The captures that a table leaves out are listed in the test of `TreeSitterHighlighter::unmapped_captures`, so a new one in an upgraded grammar is noticed. Declare the function in `capture_scopes.hpp`.

## 4. Register it

Add a row to `grammars()` in `tree_sitter_highlighter.cpp`: the language id of `core::LanguageRegistry`, the root scope (`source.json`), the grammar's `tree_sitter_<name>` function, its query and the table of step 3.

## 5. Test it

- A sample document in `tests/core/data/highlight_<name>_sample.<extension>` that uses every construct the query captures, and `highlight_<name>_sample.spans` with the expected spans, one line per span as `<line> <begin>-<end> <scope>`. Check every column by hand before checking the file in.
- A document with a syntax error, one with non-ASCII text (the columns are UTF-16), an empty document and one with a very long line, as in `tests/core/tree_sitter_highlighter_test.cpp`.
- A model-based test in the style of `EditedCHighlightingEqualsAFreshParseOfTheNewText`: random edits to the sample, after each of which the incremental spans equal a fresh parse.
- Look at the sample in a window with Dark Modern and Light Modern: `TheSamplesAreShownInDarkAndLightModern` saves the pictures when `ANVIL_SCREENSHOT_DIR` is set.
- Tests of tree-sitter code are compiled only with `ANVIL_TREE_SITTER` and skip otherwise.
- `tests/core/data/.clang-format` keeps `scripts/format.sh` from rewriting the samples.
