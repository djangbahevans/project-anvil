# Adding a tree-sitter grammar

Syntax highlighting runs a tree-sitter grammar and its `highlights.scm` query ([0010](../adr/0010-syntax-highlighting.md)). A language is added in five steps. `tree-sitter-json` is the worked example for a small grammar, `tree-sitter-c` and `tree-sitter-cpp` for one with Anvil's own patterns and for one that builds on another, and `tree-sitter-javascript` and `tree-sitter-typescript` for several languages from one parser and several parsers from one archive.

## 1. Pin the grammar

Add a line to `SOURCES` in `scripts/fetch-tree-sitter.sh`: a name, the URL of the release archive of a tag, and the archive's SHA-256.

```sh
curl -sL <url> | sha256sum
```

Use a tagged release whose `src/parser.c` was generated with an ABI that the pinned runtime supports (the runtime refuses a grammar it cannot load, which `TreeSitterHighlighter::create` reports). The archive is unpacked into `.deps/tree-sitter/<name>/`. Read the grammar's `LICENSE` and add the source to `THIRD_PARTY_NOTICES.md` with its version, copyright and license.

## 2. Build it

In the `ANVIL_HAVE_TREE_SITTER` block of `src/core/CMakeLists.txt`, add the directory of the grammar (the one that holds `src/parser.c`, relative to `.deps/tree-sitter`) to the list that `anvil_add_tree_sitter_parser` compiles: it builds `src/parser.c` and `src/scanner.c` (when there is one) without warnings. A grammar with a scanner in C++ (`scanner.cc`) needs that file added by hand. An archive with several grammars (`typescript/typescript`, `typescript/tsx`) has one entry for each.

Then embed the query of the language with `anvil_add_highlights_query(<language id> OVERLAYS ... UPSTREAM ...)`. The language id is the one of `core::LanguageRegistry`, and the query is available as `anvil::core::grammar_queries::<language id>`. `UPSTREAM` lists the `highlights.scm` files of the grammars, as paths under `.deps/tree-sitter`; a language that builds on another lists its own file before the other's (`UPSTREAM cpp/queries/highlights.scm c/queries/highlights.scm`), and one that has JSX lists `highlights-jsx.scm` too. Languages that share a parser (JavaScript and JavaScript React) share a query.

The grammars' own queries are written for the highlighters of editors that differ in which of two patterns on the same node wins; here the first one does. Where a query is too coarse for the scopes that themes style (one `@keyword` for `if` and for `const`, which VS Code's grammars scope as `keyword.control` and `storage.modifier`), add `src/core/queries/<name>.scm` and list it in `OVERLAYS`: the overlays come before the grammar's files, so their patterns decide the nodes they name, and their captures are the scope names without the language (`@keyword.control`). A pattern that the grammar's query puts before a more specific one (`(identifier) @variable` before the function patterns) is handled in the table, not by editing the query: mark the row `weak`, and any other capture of the node wins.

Predicates: `#match?`, `#not-match?`, `#eq?` and `#not-eq?` are evaluated on the captured text (PCRE2 for the regular expressions), `#is-not? local` always holds and `#is? local` never does, because the highlighter does not track local variables. A query with any other predicate makes `TreeSitterHighlighter::create` fail, because a pattern whose condition is ignored would match what it excludes; implement the predicate in `tree_sitter_highlighter.cpp` before adding such a grammar. `create_with_query` tries a query out.

## 3. Map the captures

Add a function to `capture_scopes.cpp` that returns the table of the language: one `CaptureScope{capture, scope}` per capture name of its query that should be styled, with the scope that its TextMate grammar uses (`string.quoted.double.json`, `keyword.control.cpp`). The scope has no language suffix: the grammar adds it (`json`, `js`, `js.jsx`), so that languages can share rows. A row names a capture and the captures below it (`string` also serves `string.escape`); the longest row wins. Read the capture names from `queries/highlights.scm` and the overlay; a name with no row has no scope and its text keeps the enclosing style. The captures that a table leaves out are listed in the test of `TreeSitterHighlighter::unmapped_captures`, so a new one in an upgraded grammar is noticed. Declare the function in `capture_scopes.hpp`.

## 4. Register it

Add a row to `grammars()` in `tree_sitter_highlighter.cpp`: the language id of `core::LanguageRegistry`, the root scope (`source.json`), the suffix of the scopes (`json`), the grammar's `tree_sitter_<name>` function, its query and the table of step 3. A language that has no grammar of its own but shares one (JavaScript React with JavaScript's) gets a row of its own with its own root scope and suffix.

## 5. Test it

- A sample document in `tests/core/data/highlight_<name>_sample.<extension>` that uses every construct the query captures, and `highlight_<name>_sample.spans` with the expected spans, one line per span as `<line> <begin>-<end> <scope>`. Check every column by hand before checking the file in.
- A document with a syntax error, one with non-ASCII text (the columns are UTF-16), an empty document and one with a very long line, as in `tests/core/tree_sitter_highlighter_test.cpp`.
- A model-based test in the style of `EditedCHighlightingEqualsAFreshParseOfTheNewText`: random edits to the sample, after each of which the incremental spans equal a fresh parse.
- Look at the sample in a window with Dark Modern and Light Modern: `TheSamplesAreShownInDarkAndLightModern` saves the pictures when `ANVIL_SCREENSHOT_DIR` is set.
- Tests of tree-sitter code are compiled only with `ANVIL_TREE_SITTER` and skip otherwise.
- `tests/core/data/.clang-format` keeps `scripts/format.sh` from rewriting the samples.
