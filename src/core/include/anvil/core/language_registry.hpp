#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace anvil::core {

// The rules that one `contributes.languages` entry of an extension manifest gives a language.
struct LanguageContribution {
    std::string id;
    // Absent: the language is named by its id unless an earlier contribution named it. Present and
    // empty: the contribution gives the language no name. Otherwise the first entry is the name.
    std::optional<std::vector<std::string>> aliases;
    // Matched against the end of the file name, so `.ts` and `.d.ts` are both extensions.
    std::vector<std::string> extensions;
    std::vector<std::string> filenames;
    // Globs, matched against the file name, or against the whole path when they contain `/`.
    std::vector<std::string> filename_patterns;
    // An ECMAScript regular expression for the first line of a file, anchored at its start.
    std::string first_line;
    std::vector<std::string> mime_types;
};

// A registered language, merged from every contribution with its id.
struct Language {
    std::string id;
    // What the interface shows; empty for a language that has no name (an empty `aliases`).
    std::optional<std::string> name;
    // The id first, then the aliases, without repeats.
    std::vector<std::string> aliases;
    // The first is the primary one: `text/x-<id>` stands in for a contribution without any.
    std::vector<std::string> mime_types;
    std::vector<std::string> extensions;
    std::vector<std::string> filenames;
};

// A `files.associations` entry: files that match the glob have the language.
struct FileAssociation {
    std::string pattern;
    std::string language_id;
};

namespace detail {
struct FirstLineExpression;
}

// Decides the language of a file as VS Code does (`languagesAssociations.ts`,
// `languagesRegistry.ts`).
//
// For a path, in order: the user's associations; then the registered rules, where an exact file
// name beats a file name pattern, which beats an extension; then, when the path decides nothing,
// the first line against the registered expressions. Inside each kind the longest pattern or
// extension wins and, among equal ones, the contribution registered last. Matching ignores case
// (ASCII only). A language that no rule gives, or whose id is not registered, is `plaintext`.
class LanguageRegistry {
public:
    static constexpr std::string_view plaintext_id = "plaintext";

    // `plaintext` and the languages of VS Code's built-in extensions (`language_table.hpp`).
    static LanguageRegistry with_builtin_languages();

    // Registers a contribution after the earlier ones. A first-line expression that does not
    // compile, or that matches the empty string (which VS Code skips because it can loop forever),
    // is not used: the reason is returned and the rest of the contribution is registered.
    std::optional<std::string> add(const LanguageContribution& contribution);

    // Replaces the associations of the user, in the order of the setting. Among equal patterns the
    // later one wins.
    void set_user_associations(std::vector<FileAssociation> associations);

    // `path` has forward slashes. `first_line` is the first line of the file without its line
    // break; a byte order mark in front of it is ignored. An empty path has no language, and then
    // the first line is not read, as in VS Code.
    std::string language_for(std::string_view path, std::string_view first_line = {}) const;

    const Language* language(std::string_view id) const;
    // In the order of registration.
    const std::vector<std::string>& language_ids() const {
        return ids_;
    }

private:
    struct Rule {
        std::string text;
        std::size_t language = 0;
    };
    struct PatternRule {
        std::string glob;
        // Length in UTF-16 units of the pattern as written, which VS Code compares.
        std::size_t length = 0;
        bool on_path = false;
        std::size_t language = 0;
    };
    struct FirstLineRule {
        std::shared_ptr<const detail::FirstLineExpression> expression;
        std::size_t language = 0;
    };
    struct UserRule {
        PatternRule pattern;
        std::string language_id;
    };

    std::vector<Language> languages_;
    std::vector<std::string> ids_;
    std::map<std::string, std::size_t, std::less<>> index_;
    std::vector<Rule> filenames_;
    std::vector<PatternRule> patterns_;
    std::vector<Rule> extensions_;
    std::vector<FirstLineRule> first_lines_;
    std::vector<UserRule> user_;

    std::size_t language_index(const std::string& id);
};

} // namespace anvil::core
