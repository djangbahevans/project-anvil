#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace anvil::core {

// The pair of tokens that a block comment starts and ends with.
struct BlockComment {
    std::string open;
    std::string close;

    friend bool operator==(const BlockComment&, const BlockComment&) = default;
};

// The comment tokens of a language, as VS Code's language configurations give them
// (`comments.lineComment` and `comments.blockComment`).
struct LanguageComments {
    std::optional<std::string> line;
    // `comments.lineComment.noIndent`: the line token starts at the first column and not at the
    // indentation of the line.
    bool line_no_indent = false;
    std::optional<BlockComment> block;

    bool empty() const {
        return !line && !block;
    }

    // The tokens of `overlay` take the place of the ones here, for each of the two kinds that
    // `overlay` sets: a configuration of an extension for a language replaces what it says and
    // leaves the rest.
    LanguageComments overlaid_with(const LanguageComments& overlay) const;

    friend bool operator==(const LanguageComments&, const LanguageComments&) = default;
};

struct ReadComments {
    LanguageComments comments;
    // What was wrong with the text, one message each; the parts that are right are still in
    // `comments`.
    std::vector<std::string> problems;
};

// Reads the `comments` object of a language configuration (JSON with comments and trailing
// commas), ignoring everything else in it, with VS Code's rules: `lineComment` is a string or an
// object with a string `comment` and an optional boolean `noIndent`, and `blockComment` is an array
// of two strings. An empty line token, which configurations use to say that a language has none, is
// no token and no problem; an empty token of a block comment is a problem. Each of the two kinds is
// checked on its own, so a wrong `lineComment` leaves a right `blockComment` in place. Text that is
// not valid, a configuration that is not an object and a `comments` that is not an object give no
// tokens and a problem. A configuration without `comments` has no tokens and no problem.
ReadComments read_language_comments(std::string_view text);

// The tokens of a language of VS Code's built-in extensions, from `language_comments_table.hpp`;
// null for a language that has none.
const LanguageComments* builtin_language_comments(std::string_view language_id);

} // namespace anvil::core
