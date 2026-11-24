#include "anvil/core/language_registry.hpp"

#include "anvil/core/glob.hpp"
#include "anvil/core/language_table.hpp"
#include "anvil/core/utf.hpp"

#include <algorithm>
#include <format>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

namespace anvil::core {
namespace detail {

struct FirstLineExpression {
    pcre2_code* code = nullptr;

    explicit FirstLineExpression(pcre2_code* compiled) : code(compiled) {}
    FirstLineExpression(const FirstLineExpression&) = delete;
    FirstLineExpression& operator=(const FirstLineExpression&) = delete;
    ~FirstLineExpression() {
        pcre2_code_free(code);
    }

    bool matches(std::string_view text) const {
        pcre2_match_data* data = pcre2_match_data_create_from_pattern(code, nullptr);
        pcre2_match_context* context = pcre2_match_context_create(nullptr);
        // An expression that backtracks without end on a line counts as not matching, so that the
        // line cannot hang the editor.
        pcre2_set_match_limit(context, 1'000'000);
        const char* subject = text.empty() ? "" : text.data();
        const int result = pcre2_match(code, reinterpret_cast<PCRE2_SPTR>(subject), text.size(), 0,
                                       0, data, context);
        pcre2_match_context_free(context);
        pcre2_match_data_free(data);
        return result >= 0;
    }
};

} // namespace detail

namespace {

constexpr std::string_view byte_order_mark = "\xEF\xBB\xBF";

std::string ascii_lowercase(std::string_view text) {
    std::string lowered(text);
    for (char& c : lowered) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return lowered;
}

// Node's `path.posix.basename`: the last segment, ignoring trailing slashes.
std::string_view base_name(std::string_view path) {
    while (!path.empty() && path.back() == '/') {
        path.remove_suffix(1);
    }
    const std::size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

std::size_t utf16_units(std::string_view text) {
    const auto length = utf16_length(text);
    return length ? *length : text.size();
}

bool ends_with(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

void append_unique(std::vector<std::string>& target, const std::string& value) {
    if (std::ranges::find(target, value) == target.end()) {
        target.push_back(value);
    }
}

// The expressions that VS Code accepts although they match the empty string: they cannot loop, and
// a file whose first line is blank is one they are meant for.
bool is_empty_matching_exemption(std::string_view source) {
    return source == "^" || source == "^$" || source == "$" || source == "^\\s*$";
}

} // namespace

LanguageRegistry LanguageRegistry::with_builtin_languages() {
    LanguageRegistry registry;
    for (const LanguageRow& row : language_table) {
        const auto strings = [](std::span<const std::string_view> views) {
            return std::vector<std::string>(views.begin(), views.end());
        };
        LanguageContribution contribution{.id = std::string(row.id),
                                          .aliases = std::nullopt,
                                          .extensions = strings(row.extensions),
                                          .filenames = strings(row.filenames),
                                          .filename_patterns = strings(row.filename_patterns),
                                          .first_line = std::string(row.first_line),
                                          .mime_types = strings(row.mime_types)};
        if (row.aliases) {
            contribution.aliases = strings(*row.aliases);
        }
        (void)registry.add(contribution);
    }
    return registry;
}

std::size_t LanguageRegistry::language_index(const std::string& id) {
    if (const auto found = index_.find(id); found != index_.end()) {
        return found->second;
    }
    Language created;
    created.id = id;
    languages_.push_back(std::move(created));
    ids_.push_back(id);
    return index_.emplace(id, languages_.size() - 1).first->second;
}

std::optional<std::string> LanguageRegistry::add(const LanguageContribution& contribution) {
    const std::size_t index = language_index(contribution.id);
    Language& language = languages_[index];

    if (contribution.mime_types.empty()) {
        append_unique(language.mime_types, "text/x-" + contribution.id);
    }
    for (const std::string& mime_type : contribution.mime_types) {
        append_unique(language.mime_types, mime_type);
    }
    for (const std::string& extension : contribution.extensions) {
        append_unique(language.extensions, extension);
        if (!extension.empty()) {
            extensions_.push_back({ascii_lowercase(extension), index});
        }
    }
    for (const std::string& filename : contribution.filenames) {
        append_unique(language.filenames, filename);
        filenames_.push_back({ascii_lowercase(filename), index});
    }
    for (const std::string& pattern : contribution.filename_patterns) {
        if (!pattern.empty()) {
            patterns_.push_back(
                {ascii_lowercase(pattern), utf16_units(pattern), pattern.contains('/'), index});
        }
    }

    std::optional<std::string> problem;
    if (!contribution.first_line.empty()) {
        const std::string source = contribution.first_line.starts_with('^')
                                       ? contribution.first_line
                                       : "^" + contribution.first_line;
        int error_code = 0;
        PCRE2_SIZE error_offset = 0;
        pcre2_code* code =
            pcre2_compile(reinterpret_cast<PCRE2_SPTR>(source.data()), source.size(),
                          PCRE2_UTF | PCRE2_DOLLAR_ENDONLY, &error_code, &error_offset, nullptr);
        if (code == nullptr) {
            PCRE2_UCHAR message[256];
            pcre2_get_error_message(error_code, message, sizeof message);
            problem =
                std::format("the first line expression of {} is invalid: {} (at offset {})",
                            contribution.id, reinterpret_cast<const char*>(message), error_offset);
        } else {
            auto expression = std::make_shared<detail::FirstLineExpression>(code);
            if (!is_empty_matching_exemption(source) && expression->matches({})) {
                problem = std::format("the first line expression of {} matches the empty string",
                                      contribution.id);
            } else {
                first_lines_.push_back({std::move(expression), index});
            }
        }
    }

    const auto& aliases = contribution.aliases;
    append_unique(language.aliases, contribution.id);
    if (aliases) {
        for (const std::string& alias : *aliases) {
            if (!alias.empty()) {
                append_unique(language.aliases, alias);
            }
        }
    }
    const bool gives_no_name = aliases && aliases->empty();
    if (!gives_no_name) {
        const bool has_aliases = aliases && !aliases->empty();
        if (has_aliases || !language.name) {
            const std::string_view first =
                has_aliases ? std::string_view((*aliases)[0]) : std::string_view();
            language.name = first.empty() ? contribution.id : std::string(first);
        }
    }
    return problem;
}

void LanguageRegistry::set_user_associations(std::vector<FileAssociation> associations) {
    user_.clear();
    for (FileAssociation& association : associations) {
        if (association.pattern.empty()) {
            continue;
        }
        user_.push_back({{ascii_lowercase(association.pattern), utf16_units(association.pattern),
                          association.pattern.contains('/'), 0},
                         std::move(association.language_id)});
    }
}

const Language* LanguageRegistry::language(std::string_view id) const {
    const auto found = index_.find(id);
    return found == index_.end() ? nullptr : &languages_[found->second];
}

std::string LanguageRegistry::language_for(std::string_view path,
                                           std::string_view first_line) const {
    if (path.empty()) {
        return std::string(plaintext_id);
    }
    const std::string lowered = ascii_lowercase(path);
    const std::string_view name = base_name(lowered);

    const auto matches = [&](const PatternRule& rule) {
        return glob_match(rule.glob, rule.on_path ? std::string_view(lowered) : name);
    };

    // Scans from the last registration, so that it wins among rules of the same strength.
    const UserRule* user_match = nullptr;
    for (auto rule = user_.rbegin(); rule != user_.rend(); ++rule) {
        if ((user_match == nullptr || rule->pattern.length > user_match->pattern.length) &&
            matches(rule->pattern)) {
            user_match = &*rule;
        }
    }
    if (user_match != nullptr) {
        return language(user_match->language_id) != nullptr ? user_match->language_id
                                                            : std::string(plaintext_id);
    }

    for (auto rule = filenames_.rbegin(); rule != filenames_.rend(); ++rule) {
        if (rule->text == name) {
            return languages_[rule->language].id;
        }
    }
    const PatternRule* pattern_match = nullptr;
    for (auto rule = patterns_.rbegin(); rule != patterns_.rend(); ++rule) {
        if ((pattern_match == nullptr || rule->length > pattern_match->length) && matches(*rule)) {
            pattern_match = &*rule;
        }
    }
    if (pattern_match != nullptr) {
        return languages_[pattern_match->language].id;
    }
    const Rule* extension_match = nullptr;
    for (auto rule = extensions_.rbegin(); rule != extensions_.rend(); ++rule) {
        if ((extension_match == nullptr || rule->text.size() > extension_match->text.size()) &&
            ends_with(name, rule->text)) {
            extension_match = &*rule;
        }
    }
    if (extension_match != nullptr) {
        return languages_[extension_match->language].id;
    }

    if (first_line.starts_with(byte_order_mark)) {
        first_line.remove_prefix(byte_order_mark.size());
    }
    if (!first_line.empty()) {
        for (auto rule = first_lines_.rbegin(); rule != first_lines_.rend(); ++rule) {
            if (rule->expression->matches(first_line)) {
                return languages_[rule->language].id;
            }
        }
    }
    return std::string(plaintext_id);
}

} // namespace anvil::core
