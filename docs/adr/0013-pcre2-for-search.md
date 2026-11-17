# 0013. PCRE2 for regular expressions and case folding in search

- Status: Accepted
- Date: 2026-11-17

## Context

Find and replace offers literal, case-insensitive, whole-word and regular-expression queries. Users coming from VS Code expect regular expressions with look-around and backreferences, and case-insensitive matching of non-ASCII text, which needs Unicode case-folding tables. Documents can be large ([0006](0006-editor-core-and-text-encoding.md)), so the engine must work on UTF-8 bytes in bounded windows, report byte offsets, and not slow down on long texts.

## Decision

- Search uses PCRE2 (the 8-bit library in UTF mode, with its JIT compiler when the platform has one) for every query. A plain query is compiled with `PCRE2_LITERAL`, so literal and regular-expression searches share one engine and one definition of case-insensitivity: Unicode simple case folding, as implemented by PCRE2.
- Only `core` links PCRE2, privately; no PCRE2 type appears in a public header.
- A regular expression is matched against one line at a time, so a match never contains a line break and `^` and `$` match at the ends of lines. A literal query is matched over windows of the text and may contain line breaks. Windows reach past the positions they own by the longest possible match, so a match is never cut at a window edge.
- Whole-word matching is checked by the editor, with the same word classes as word motion and selection (`classify`), instead of `\b`, so every command agrees on what a word is.
- The engine's backtracking limit is set, and exceeding it is an error reported to the user, so a pattern such as `(a+)+$` cannot freeze the window.
- `\C`, which matches a single byte and could end a match inside a character, is rejected.
- Replacement text for regular expressions is expanded by the editor (`$1`, `$&`, `$$`, `\n`, `\t`, `\\`) instead of PCRE2's substitution, so literal queries and regular expressions share the matching loop and a single replacement is computed the same way as Replace All.

## Consequences

- Look-around, backreferences, lazy quantifiers and Unicode-aware case-insensitive matching work without further dependencies, and JIT-compiled matching is fast on large texts.
- PCRE2 must be installed to build: `libpcre2-dev` on Debian and Ubuntu, and `pcre2` from vcpkg. CMake uses its package configuration when present and pkg-config otherwise.
- Simple case folding does not match one character against several, so `ß` does not match `SS`.
- `\w`, `\d` and `\b` in a regular expression are ASCII-only, as in JavaScript regular expressions, which is what VS Code users know; whole-word matching treats every non-ASCII character as a word character.
- Regular expressions cannot match across lines. Supporting that would need windows whose edges fall on line breaks that the pattern can bridge, with a bound on how far a match may reach.
- `std::regex` would be slower and has no UTF-8 awareness. RE2 has no look-around or backreferences. ICU would bring a large data file and a second library to a project that aims for low memory use and fast startup.
- This decision covers search only. Grapheme cluster segmentation for cursor motion is a separate decision.
