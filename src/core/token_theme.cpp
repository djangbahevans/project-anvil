#include "anvil/core/token_theme.hpp"

#include <algorithm>
#include <map>

namespace anvil::core {
namespace {

// A rule as the file wrote it, with one scope selector.
struct ParsedRule {
    std::string scope;
    // The words before the last, nearest first; none when there is a single word.
    std::optional<std::vector<std::string>> parent_scopes;
    // Position of the entry in the file.
    std::size_t index = 0;
    std::optional<std::uint8_t> font_style;
    std::optional<Rgba> foreground;
    std::optional<Rgba> background;
};

// What a rule of the trie says: the scope depth it was written at decides its specificity.
struct Rule {
    int scope_depth = 0;
    std::vector<std::string> parent_scopes;
    std::optional<std::uint8_t> font_style;
    std::optional<Rgba> foreground;
    std::optional<Rgba> background;

    void accept_overwrite(int depth, std::optional<std::uint8_t> font, std::optional<Rgba> fg,
                          std::optional<Rgba> bg) {
        scope_depth = depth;
        if (font) {
            font_style = font;
        }
        if (fg) {
            foreground = fg;
        }
        if (bg) {
            background = bg;
        }
    }
};

std::string_view trim(std::string_view text) {
    constexpr std::string_view space = " \t\n\r\v\f";
    const auto first = text.find_first_not_of(space);
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(space) - first + 1);
}

std::vector<std::string_view> split(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        const auto end = text.find(separator, start);
        if (end == std::string_view::npos) {
            parts.push_back(text.substr(start));
            return parts;
        }
        parts.push_back(text.substr(start, end - start));
        start = end + 1;
    }
}

int compare_arrays(const std::optional<std::vector<std::string>>& a,
                   const std::optional<std::vector<std::string>>& b) {
    if (!a && !b) {
        return 0;
    }
    if (!a) {
        return -1;
    }
    if (!b) {
        return 1;
    }
    if (a->size() != b->size()) {
        return a->size() < b->size() ? -1 : 1;
    }
    for (std::size_t i = 0; i < a->size(); ++i) {
        if (const int by_word = (*a)[i].compare((*b)[i]); by_word != 0) {
            return by_word < 0 ? -1 : 1;
        }
    }
    return 0;
}

bool names_scope(std::string_view scope, std::string_view word) {
    return scope == word || (scope.starts_with(word) && scope[word.size()] == '.');
}

// Whether the ancestors of the scope at `position` of the stack satisfy the words, which are
// nearest first: each word is found at or beyond the ancestor after the one the word before it
// found, or exactly at it after a `>`.
bool ancestors_match(std::span<const std::string_view> stack, std::size_t position,
                     const std::vector<std::string>& words) {
    auto ancestor = static_cast<std::ptrdiff_t>(position) - 1;
    for (std::size_t n = 0; n < words.size(); ++n) {
        std::string_view word = words[n];
        bool adjacent = false;
        if (word == ">") {
            if (n == words.size() - 1) {
                return false;
            }
            word = words[++n];
            adjacent = true;
        }
        while (ancestor >= 0 && !names_scope(stack[static_cast<std::size_t>(ancestor)], word)) {
            if (adjacent) {
                return false;
            }
            --ancestor;
        }
        if (ancestor < 0) {
            return false;
        }
        --ancestor;
    }
    return true;
}

// Negative when `a` is the more specific rule.
int compare_by_specificity(const Rule& a, const Rule& b) {
    if (a.scope_depth != b.scope_depth) {
        return b.scope_depth - a.scope_depth;
    }
    std::size_t n = 0;
    std::size_t s = 0;
    while (true) {
        if (n < a.parent_scopes.size() && a.parent_scopes[n] == ">") {
            ++n;
        }
        if (s < b.parent_scopes.size() && b.parent_scopes[s] == ">") {
            ++s;
        }
        if (n >= a.parent_scopes.size() || s >= b.parent_scopes.size()) {
            break;
        }
        const auto by_length = static_cast<int>(b.parent_scopes[s].size()) -
                               static_cast<int>(a.parent_scopes[n].size());
        if (by_length != 0) {
            return by_length;
        }
        ++n;
        ++s;
    }
    return static_cast<int>(b.parent_scopes.size()) - static_cast<int>(a.parent_scopes.size());
}

std::optional<std::uint8_t> read_font_style(const nlohmann::json& settings) {
    const auto found = settings.find("fontStyle");
    if (found == settings.end() || !found->is_string()) {
        return std::nullopt;
    }
    std::uint8_t bits = 0;
    for (const std::string_view word : split(found->get_ref<const std::string&>(), ' ')) {
        if (word == "italic") {
            bits |= FontStyle::italic;
        } else if (word == "bold") {
            bits |= FontStyle::bold;
        } else if (word == "underline") {
            bits |= FontStyle::underline;
        } else if (word == "strikethrough") {
            bits |= FontStyle::strikethrough;
        }
    }
    return bits;
}

std::optional<Rgba> read_color(const nlohmann::json& settings, const char* key, std::size_t index,
                               std::vector<std::string>& warnings) {
    const auto found = settings.find(key);
    if (found == settings.end() || !found->is_string()) {
        return std::nullopt;
    }
    const auto color = parse_color(found->get_ref<const std::string&>());
    if (!color) {
        warnings.push_back("rule " + std::to_string(index) + ": " + key + " '" +
                           found->get_ref<const std::string&>() + "' is not a color");
    }
    return color;
}

std::vector<ParsedRule> parse_rules(const nlohmann::json& rules,
                                    std::vector<std::string>& warnings) {
    std::vector<ParsedRule> parsed;
    if (!rules.is_array()) {
        warnings.push_back("the token colors are not an array of rules");
        return parsed;
    }
    for (std::size_t index = 0; index < rules.size(); ++index) {
        const nlohmann::json& entry = rules[index];
        if (!entry.is_object()) {
            warnings.push_back("rule " + std::to_string(index) + " is not an object");
            continue;
        }
        const auto settings_entry = entry.find("settings");
        if (settings_entry == entry.end() || !settings_entry->is_object()) {
            continue;
        }
        const nlohmann::json& settings = *settings_entry;

        std::vector<std::string> selectors;
        const auto scope = entry.find("scope");
        if (scope != entry.end() && scope->is_string()) {
            std::string_view text = scope->get_ref<const std::string&>();
            while (text.starts_with(',')) {
                text.remove_prefix(1);
            }
            while (text.ends_with(',')) {
                text.remove_suffix(1);
            }
            for (const std::string_view selector : split(text, ',')) {
                selectors.emplace_back(selector);
            }
        } else if (scope != entry.end() && scope->is_array()) {
            for (const nlohmann::json& selector : *scope) {
                if (selector.is_string()) {
                    selectors.push_back(selector.get<std::string>());
                } else {
                    warnings.push_back("rule " + std::to_string(index) +
                                       ": a scope that is not a string is skipped");
                }
            }
        } else {
            selectors.emplace_back();
        }

        const auto font_style = read_font_style(settings);
        const auto foreground = read_color(settings, "foreground", index, warnings);
        const auto background = read_color(settings, "background", index, warnings);
        for (const std::string& selector : selectors) {
            std::vector<std::string_view> words = split(trim(selector), ' ');
            ParsedRule rule;
            rule.scope = std::string(words.back());
            if (words.size() > 1) {
                std::vector<std::string> parents;
                for (auto it = words.rbegin() + 1; it != words.rend(); ++it) {
                    parents.emplace_back(*it);
                }
                rule.parent_scopes = std::move(parents);
            }
            rule.index = index;
            rule.font_style = font_style;
            rule.foreground = foreground;
            rule.background = background;
            parsed.push_back(std::move(rule));
        }
    }
    return parsed;
}

} // namespace

// The theme as a trie of scope segments. A node holds the rule for its own scope, which inherits
// from the nodes above it, and the rules that also need ancestors.
struct TokenTheme::Node {
    Rule main;
    std::vector<Rule> with_parents;
    std::map<std::string, std::unique_ptr<Node>, std::less<>> children;
    // The rules that name this scope, the most specific first; made once the trie is built.
    std::vector<Rule> candidates;

    void insert(int depth, std::string_view scope,
                const std::optional<std::vector<std::string>>& parents, const ParsedRule& source) {
        if (scope.empty()) {
            insert_here(depth, parents, source);
            return;
        }
        const auto dot = scope.find('.');
        const std::string_view head = scope.substr(0, dot);
        const std::string_view tail =
            dot == std::string_view::npos ? std::string_view() : scope.substr(dot + 1);
        auto child = children.find(head);
        if (child == children.end()) {
            auto made = std::make_unique<Node>();
            made->main = main;
            made->with_parents = with_parents;
            child = children.emplace(std::string(head), std::move(made)).first;
        }
        child->second->insert(depth + 1, tail, parents, source);
    }

    void insert_here(int depth, const std::optional<std::vector<std::string>>& parents,
                     const ParsedRule& source) {
        if (!parents) {
            main.accept_overwrite(depth, source.font_style, source.foreground, source.background);
            return;
        }
        for (Rule& rule : with_parents) {
            if (rule.parent_scopes == *parents) {
                rule.accept_overwrite(depth, source.font_style, source.foreground,
                                      source.background);
                return;
            }
        }
        Rule rule;
        rule.scope_depth = depth;
        rule.parent_scopes = *parents;
        rule.font_style = source.font_style ? source.font_style : main.font_style;
        rule.foreground = source.foreground ? source.foreground : main.foreground;
        rule.background = source.background ? source.background : main.background;
        with_parents.push_back(std::move(rule));
    }

    void finish() {
        candidates = with_parents;
        candidates.push_back(main);
        std::ranges::stable_sort(candidates, [](const Rule& a, const Rule& b) {
            return compare_by_specificity(a, b) < 0;
        });
        for (auto& [name, child] : children) {
            child->finish();
        }
    }

    // The node whose rules decide for a scope: the deepest one along its dotted segments.
    const Node& find(std::string_view scope) const {
        const Node* node = this;
        while (!scope.empty()) {
            const auto dot = scope.find('.');
            const auto child = node->children.find(scope.substr(0, dot));
            if (child == node->children.end()) {
                break;
            }
            node = child->second.get();
            scope = dot == std::string_view::npos ? std::string_view() : scope.substr(dot + 1);
        }
        return *node;
    }
};

TokenTheme::TokenTheme() : root_(std::make_unique<Node>()) {}
TokenTheme::TokenTheme(TokenTheme&&) noexcept = default;
TokenTheme& TokenTheme::operator=(TokenTheme&&) noexcept = default;
TokenTheme::~TokenTheme() = default;

TokenTheme TokenTheme::from_json(const nlohmann::json& rules) {
    TokenTheme theme;
    std::vector<ParsedRule> parsed = parse_rules(rules, theme.warnings_);
    std::ranges::stable_sort(parsed, [](const ParsedRule& a, const ParsedRule& b) {
        if (const int by_scope = a.scope.compare(b.scope); by_scope != 0) {
            return by_scope < 0;
        }
        if (const int by_parents = compare_arrays(a.parent_scopes, b.parent_scopes);
            by_parents != 0) {
            return by_parents < 0;
        }
        return a.index < b.index;
    });

    std::size_t first = 0;
    while (first < parsed.size() && parsed[first].scope.empty()) {
        const ParsedRule& rule = parsed[first++];
        if (rule.font_style) {
            theme.defaults_.font_style = rule.font_style;
        }
        if (rule.foreground) {
            theme.defaults_.foreground = rule.foreground;
        }
        if (rule.background) {
            theme.defaults_.background = rule.background;
        }
    }
    for (std::size_t i = first; i < parsed.size(); ++i) {
        theme.root_->insert(0, parsed[i].scope, parsed[i].parent_scopes, parsed[i]);
    }
    theme.root_->finish();
    return theme;
}

TokenStyle TokenTheme::style_for(std::span<const std::string_view> scope_stack) const {
    TokenStyle style = defaults_;
    for (std::size_t position = 0; position < scope_stack.size(); ++position) {
        const Node& node = root_->find(scope_stack[position]);
        for (const Rule& rule : node.candidates) {
            if (!ancestors_match(scope_stack, position, rule.parent_scopes)) {
                continue;
            }
            if (rule.font_style) {
                style.font_style = rule.font_style;
            }
            if (rule.foreground) {
                style.foreground = rule.foreground;
            }
            if (rule.background) {
                style.background = rule.background;
            }
            break;
        }
    }
    return style;
}

} // namespace anvil::core
