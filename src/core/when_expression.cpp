#include "anvil/core/when_expression.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

namespace anvil::core {

void ContextKeys::set(std::string key, ContextValue value) {
    values_.insert_or_assign(std::move(key), std::move(value));
}

void ContextKeys::unset(std::string_view key) {
    if (const auto it = values_.find(key); it != values_.end()) {
        values_.erase(it);
    }
}

const ContextValue* ContextKeys::find(std::string_view key) const {
    const auto it = values_.find(key);
    return it == values_.end() ? nullptr : &it->second;
}

namespace detail {
namespace {

constexpr double not_a_number = std::numeric_limits<double>::quiet_NaN();
constexpr double infinity = std::numeric_limits<double>::infinity();

// Bounds the backtracking of one match, so a pattern such as (a+)+$ gives up instead of freezing
// the caller. A match that gives up is no match.
constexpr std::uint32_t regex_match_limit = 1'000'000;

} // namespace

// The numbering fixes the order of terms in a normalized expression, which is VS Code's.
enum class Kind {
    False,
    True,
    Defined,
    Not,
    Equals,
    NotEquals,
    And,
    Regex,
    NotRegex,
    Or,
    In,
    NotIn,
    Greater,
    GreaterEquals,
    Smaller,
    SmallerEquals,
};

class CompiledRegex {
public:
    // The message of the engine when the pattern does not compile.
    static std::expected<std::shared_ptr<const CompiledRegex>, std::string>
    compile(std::string_view pattern, std::uint32_t flag_options) {
        // ALT_BSUX and MATCH_UNSET_BACKREF make PCRE2 read \uXXXX and references to unset groups
        // as JavaScript does, DOLLAR_ENDONLY stops `$` from matching before a final line break,
        // and \C would match a single byte of a character.
        const std::uint32_t options = PCRE2_UTF | PCRE2_ALT_BSUX | PCRE2_MATCH_UNSET_BACKREF |
                                      PCRE2_DOLLAR_ENDONLY | PCRE2_NEVER_BACKSLASH_C | flag_options;
        int error_code = 0;
        PCRE2_SIZE error_offset = 0;
        pcre2_code* code =
            pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.data()), pattern.size(), options,
                          &error_code, &error_offset, nullptr);
        if (code == nullptr) {
            PCRE2_UCHAR buffer[256];
            pcre2_get_error_message(error_code, buffer, sizeof buffer);
            return std::unexpected(std::format("{} (at offset {} of the pattern)",
                                               reinterpret_cast<const char*>(buffer),
                                               error_offset));
        }
        return std::shared_ptr<const CompiledRegex>(new CompiledRegex(code));
    }

    ~CompiledRegex() {
        pcre2_code_free(code_);
    }

    CompiledRegex(const CompiledRegex&) = delete;
    CompiledRegex& operator=(const CompiledRegex&) = delete;

    bool test(std::string_view subject) const {
        pcre2_match_data* data = pcre2_match_data_create_from_pattern(code_, nullptr);
        pcre2_match_context* context = pcre2_match_context_create(nullptr);
        if (data == nullptr || context == nullptr) {
            pcre2_match_data_free(data);
            pcre2_match_context_free(context);
            return false;
        }
        pcre2_set_match_limit(context, regex_match_limit);
        const int result = pcre2_match(code_, reinterpret_cast<PCRE2_SPTR>(subject.data()),
                                       subject.size(), 0, 0, data, context);
        pcre2_match_data_free(data);
        pcre2_match_context_free(context);
        return result >= 0;
    }

private:
    explicit CompiledRegex(pcre2_code* code) : code_(code) {}

    pcre2_code* code_;
};

struct Node;
using NodePtr = std::shared_ptr<const Node>;

// Only the fields that a kind uses are set: `key` and `text` for terms on a key (`text` is the
// compared text, the key that holds the list for `in`, the pattern source for a regular
// expression, or the non-numeric operand of a comparison), `flags` and `regex` for regular
// expressions, `numeric` and `number` for comparisons with a number, and `terms` for And and Or.
// The negation of an And or Or refers to it in `negation_of`, so that negating twice gives back the
// expression that was written instead of a longer equivalent one.
struct Node {
    Kind kind = Kind::True;
    std::string key;
    std::string text;
    std::string flags;
    bool numeric = false;
    double number = 0;
    std::shared_ptr<const CompiledRegex> regex;
    std::vector<NodePtr> terms;
    NodePtr negation_of;
};

struct WhenImpl {
    NodePtr root;
    std::string canonical;
};

namespace {

NodePtr make_term(Kind kind, std::string key, std::string text = {}) {
    Node node;
    node.kind = kind;
    node.key = std::move(key);
    node.text = std::move(text);
    return std::make_shared<const Node>(std::move(node));
}

NodePtr make_constant(bool value) {
    static const NodePtr true_node = make_term(Kind::True, {});
    static const NodePtr false_node = make_term(Kind::False, {});
    return value ? true_node : false_node;
}

NodePtr with_kind(const Node& node, Kind kind) {
    Node copy = node;
    copy.kind = kind;
    return std::make_shared<const Node>(std::move(copy));
}

bool is_white_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && is_white_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_white_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

int digit_value(char c) {
    if (is_digit(c)) {
        return c - '0';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'Z') {
        return c - 'A' + 10;
    }
    return 99;
}

// The length of the longest prefix of `text` that is an unsigned decimal literal: digits with an
// optional fraction, or a fraction alone, and an optional exponent.
std::size_t decimal_prefix_length(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size() && is_digit(text[i])) {
        ++i;
    }
    const std::size_t integer_digits = i;
    std::size_t fraction_digits = 0;
    if (i < text.size() && text[i] == '.') {
        std::size_t j = i + 1;
        while (j < text.size() && is_digit(text[j])) {
            ++j;
        }
        fraction_digits = j - i - 1;
        if (integer_digits > 0 || fraction_digits > 0) {
            i = j;
        }
    }
    if (integer_digits == 0 && fraction_digits == 0) {
        return 0;
    }
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        std::size_t j = i + 1;
        if (j < text.size() && (text[j] == '+' || text[j] == '-')) {
            ++j;
        }
        if (j < text.size() && is_digit(text[j])) {
            while (j < text.size() && is_digit(text[j])) {
                ++j;
            }
            i = j;
        }
    }
    return i;
}

double parse_decimal(std::string_view literal) {
    // std::from_chars does not accept a fraction point that no digit follows ("5.", "5.e3").
    std::string normalized;
    normalized.reserve(literal.size());
    for (std::size_t i = 0; i < literal.size(); ++i) {
        const bool dangling_point =
            literal[i] == '.' && (i + 1 == literal.size() || !is_digit(literal[i + 1]));
        if (!dangling_point) {
            normalized += literal[i];
        }
    }
    double value = 0;
    const auto result =
        std::from_chars(normalized.data(), normalized.data() + normalized.size(), value);
    if (result.ec == std::errc::result_out_of_range) {
        return infinity;
    }
    return result.ec == std::errc() ? value : not_a_number;
}

// JavaScript's parseFloat: the longest numeric prefix after leading white space.
std::optional<double> parse_float(std::string_view text) {
    while (!text.empty() && is_white_space(text.front())) {
        text.remove_prefix(1);
    }
    bool negative = false;
    if (!text.empty() && (text.front() == '+' || text.front() == '-')) {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }
    if (text.starts_with("Infinity")) {
        return negative ? -infinity : infinity;
    }
    const std::size_t length = decimal_prefix_length(text);
    if (length == 0) {
        return std::nullopt;
    }
    const double value = parse_decimal(text.substr(0, length));
    return negative ? -value : value;
}

// JavaScript's conversion of text to a number, which `==` applies to a string compared with a
// number or boolean: empty text is 0, and the whole text must be a number.
double string_to_number(std::string_view text) {
    text = trim(text);
    if (text.empty()) {
        return 0;
    }
    if (text.size() > 2 && text[0] == '0') {
        int base = 0;
        switch (text[1]) {
        case 'x':
        case 'X':
            base = 16;
            break;
        case 'o':
        case 'O':
            base = 8;
            break;
        case 'b':
        case 'B':
            base = 2;
            break;
        default:
            break;
        }
        if (base != 0) {
            double value = 0;
            for (const char c : text.substr(2)) {
                const int digit = digit_value(c);
                if (digit >= base) {
                    return not_a_number;
                }
                value = value * base + digit;
            }
            return value;
        }
    }
    bool negative = false;
    std::string_view body = text;
    if (body.front() == '+' || body.front() == '-') {
        negative = body.front() == '-';
        body.remove_prefix(1);
    }
    if (body == "Infinity") {
        return negative ? -infinity : infinity;
    }
    const std::size_t length = decimal_prefix_length(body);
    if (length == 0 || length != body.size()) {
        return not_a_number;
    }
    const double value = parse_decimal(body);
    return negative ? -value : value;
}

std::string number_to_string(double value) {
    if (std::isnan(value)) {
        return "NaN";
    }
    if (std::isinf(value)) {
        return value < 0 ? "-Infinity" : "Infinity";
    }
    if (value == std::trunc(value) && std::fabs(value) < 1e15) {
        return std::format("{}", static_cast<std::int64_t>(value));
    }
    return std::format("{}", value);
}

std::string join(const std::vector<std::string>& items) {
    std::string result;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) {
            result += ',';
        }
        result += items[i];
    }
    return result;
}

bool is_truthy(const ContextValue* value) {
    if (value == nullptr) {
        return false;
    }
    if (const bool* flag = std::get_if<bool>(value)) {
        return *flag;
    }
    if (const double* number = std::get_if<double>(value)) {
        return *number != 0 && !std::isnan(*number);
    }
    if (const std::string* text = std::get_if<std::string>(value)) {
        return !text->empty();
    }
    return true;
}

// JavaScript's String(value), which regular expression tests apply to their subject.
std::string to_text(const ContextValue* value) {
    if (value == nullptr) {
        return "undefined";
    }
    if (const bool* flag = std::get_if<bool>(value)) {
        return *flag ? "true" : "false";
    }
    if (const double* number = std::get_if<double>(value)) {
        return number_to_string(*number);
    }
    if (const std::string* text = std::get_if<std::string>(value)) {
        return *text;
    }
    return join(std::get<std::vector<std::string>>(*value));
}

// JavaScript's `value == text`, where `text` is the written operand.
bool loosely_equals(const ContextValue* value, std::string_view text) {
    if (value == nullptr) {
        return false;
    }
    if (const bool* flag = std::get_if<bool>(value)) {
        return (*flag ? 1.0 : 0.0) == string_to_number(text);
    }
    if (const double* number = std::get_if<double>(value)) {
        return *number == string_to_number(text);
    }
    return to_text(value) == text;
}

// JavaScript's parseFloat(value), which the comparison operators apply to the key.
double to_number(const ContextValue* value) {
    if (value == nullptr || std::holds_alternative<bool>(*value)) {
        return not_a_number;
    }
    if (const double* number = std::get_if<double>(value)) {
        return *number;
    }
    return parse_float(to_text(value)).value_or(not_a_number);
}

int compare_text(std::string_view a, std::string_view b) {
    const int result = a.compare(b);
    return result < 0 ? -1 : result > 0 ? 1 : 0;
}

// A total order on terms. Terms sort by kind first, then by key and operand.
int compare(const Node& a, const Node& b) {
    if (a.kind != b.kind) {
        return a.kind < b.kind ? -1 : 1;
    }
    if (const int result = compare_text(a.key, b.key); result != 0) {
        return result;
    }
    if (const int result = compare_text(a.text, b.text); result != 0) {
        return result;
    }
    if (const int result = compare_text(a.flags, b.flags); result != 0) {
        return result;
    }
    if (a.numeric != b.numeric) {
        return a.numeric ? -1 : 1;
    }
    if (a.number != b.number) {
        return a.number < b.number ? -1 : 1;
    }
    if (a.terms.size() != b.terms.size()) {
        return a.terms.size() < b.terms.size() ? -1 : 1;
    }
    for (std::size_t i = 0; i < a.terms.size(); ++i) {
        if (const int result = compare(*a.terms[i], *b.terms[i]); result != 0) {
            return result;
        }
    }
    return 0;
}

bool node_less(const NodePtr& a, const NodePtr& b) {
    return compare(*a, *b) < 0;
}

bool node_equal(const NodePtr& a, const NodePtr& b) {
    return compare(*a, *b) == 0;
}

void sort_and_unique(std::vector<NodePtr>& terms) {
    std::ranges::sort(terms, node_less);
    terms.erase(std::ranges::unique(terms, node_equal).begin(), terms.end());
}

NodePtr negate(const NodePtr& node);
NodePtr make_or(std::vector<NodePtr> terms, bool check_complements, NodePtr negation_of = nullptr);

// Whether a pair of terms are each other's negation, which makes `&&` of them false and `||` of
// them true.
bool has_complementary_pair(const std::vector<NodePtr>& terms) {
    for (std::size_t i = 0; i < terms.size(); ++i) {
        for (std::size_t j = i + 1; j < terms.size(); ++j) {
            if (node_equal(negate(terms[i]), terms[j])) {
                return true;
            }
        }
    }
    return false;
}

NodePtr make_node_with_terms(Kind kind, std::vector<NodePtr> terms, NodePtr negation_of) {
    Node node;
    node.kind = kind;
    node.terms = std::move(terms);
    node.negation_of = std::move(negation_of);
    return std::make_shared<const Node>(std::move(node));
}

NodePtr make_and(std::vector<NodePtr> input, bool check_complements) {
    std::vector<NodePtr> terms;
    for (NodePtr& term : input) {
        switch (term->kind) {
        case Kind::True:
            break;
        case Kind::False:
            return make_constant(false);
        case Kind::And:
            terms.insert(terms.end(), term->terms.begin(), term->terms.end());
            break;
        default:
            terms.push_back(std::move(term));
            break;
        }
    }
    if (terms.empty()) {
        return make_constant(true);
    }
    if (terms.size() == 1) {
        return terms.front();
    }
    sort_and_unique(terms);
    if (terms.size() == 1) {
        return terms.front();
    }

    // `||` groups are distributed over the remaining terms, so that the result is an `||` of `&&`
    // groups.
    while (terms.size() > 1) {
        const auto last_or = std::ranges::find_if(
            terms.rbegin(), terms.rend(), [](const NodePtr& t) { return t->kind == Kind::Or; });
        if (last_or == terms.rend()) {
            break;
        }
        const NodePtr group = *last_or;
        terms.erase(std::next(last_or).base());
        const NodePtr other = terms.back();
        terms.pop_back();
        const bool finished = terms.empty();

        std::vector<NodePtr> distributed;
        distributed.reserve(group->terms.size());
        for (const NodePtr& member : group->terms) {
            distributed.push_back(make_and({member, other}, check_complements));
        }
        NodePtr result = make_or(std::move(distributed), finished);
        if (result->kind == Kind::False) {
            return result;
        }
        terms.push_back(std::move(result));
        std::ranges::sort(terms, node_less);
    }
    if (terms.size() == 1) {
        return terms.front();
    }

    // A distributed group that collapsed into a single `&&` must not stay nested.
    if (std::ranges::any_of(terms, [](const NodePtr& t) { return t->kind == Kind::And; })) {
        std::vector<NodePtr> flat;
        for (const NodePtr& term : terms) {
            if (term->kind == Kind::And) {
                flat.insert(flat.end(), term->terms.begin(), term->terms.end());
            } else {
                flat.push_back(term);
            }
        }
        sort_and_unique(flat);
        terms = std::move(flat);
    }

    if (check_complements && has_complementary_pair(terms)) {
        return make_constant(false);
    }
    return make_node_with_terms(Kind::And, std::move(terms), nullptr);
}

NodePtr make_or(std::vector<NodePtr> input, bool check_complements, NodePtr negation_of) {
    std::vector<NodePtr> terms;
    for (NodePtr& term : input) {
        switch (term->kind) {
        case Kind::False:
            break;
        case Kind::True:
            return make_constant(true);
        case Kind::Or:
            terms.insert(terms.end(), term->terms.begin(), term->terms.end());
            break;
        default:
            terms.push_back(std::move(term));
            break;
        }
    }
    if (terms.empty()) {
        return make_constant(false);
    }
    sort_and_unique(terms);
    if (terms.size() == 1) {
        return terms.front();
    }
    if (check_complements && has_complementary_pair(terms)) {
        return make_constant(true);
    }
    return make_node_with_terms(Kind::Or, std::move(terms), std::move(negation_of));
}

std::vector<NodePtr> alternatives(const NodePtr& node) {
    return node->kind == Kind::Or ? node->terms : std::vector<NodePtr>{node};
}

// The negation of `||` of groups: each group is negated, and the negations are combined with `&&`
// by distributing over their alternatives.
NodePtr negate_or(const NodePtr& original) {
    std::vector<NodePtr> negated;
    negated.reserve(original->terms.size());
    for (const NodePtr& term : original->terms) {
        negated.push_back(negate(term));
    }
    while (negated.size() > 1) {
        const NodePtr left = negated[0];
        const NodePtr right = negated[1];
        negated.erase(negated.begin(), negated.begin() + 2);
        std::vector<NodePtr> products;
        for (const NodePtr& l : alternatives(left)) {
            for (const NodePtr& r : alternatives(right)) {
                products.push_back(make_and({l, r}, true));
            }
        }
        negated.insert(negated.begin(), make_or(std::move(products), false));
    }
    return make_or(std::move(negated), true, original);
}

NodePtr negate(const NodePtr& node) {
    switch (node->kind) {
    case Kind::False:
        return make_constant(true);
    case Kind::True:
        return make_constant(false);
    case Kind::Defined:
        return with_kind(*node, Kind::Not);
    case Kind::Not:
        return with_kind(*node, Kind::Defined);
    case Kind::Equals:
        return with_kind(*node, Kind::NotEquals);
    case Kind::NotEquals:
        return with_kind(*node, Kind::Equals);
    case Kind::Regex:
        return with_kind(*node, Kind::NotRegex);
    case Kind::NotRegex:
        return with_kind(*node, Kind::Regex);
    case Kind::In:
        return with_kind(*node, Kind::NotIn);
    case Kind::NotIn:
        return with_kind(*node, Kind::In);
    case Kind::Greater:
        return with_kind(*node, Kind::SmallerEquals);
    case Kind::GreaterEquals:
        return with_kind(*node, Kind::Smaller);
    case Kind::Smaller:
        return with_kind(*node, Kind::GreaterEquals);
    case Kind::SmallerEquals:
        return with_kind(*node, Kind::Greater);
    case Kind::And: {
        std::vector<NodePtr> negated;
        negated.reserve(node->terms.size());
        for (const NodePtr& term : node->terms) {
            negated.push_back(negate(term));
        }
        return make_or(std::move(negated), true, node);
    }
    case Kind::Or:
        return node->negation_of ? node->negation_of : negate_or(node);
    }
    return node;
}

NodePtr make_comparison(Kind kind, std::string key, const std::string& operand) {
    Node node;
    node.kind = kind;
    node.key = std::move(key);
    if (const auto number = parse_float(operand)) {
        node.numeric = true;
        node.number = *number;
    } else {
        node.text = operand;
    }
    return std::make_shared<const Node>(std::move(node));
}

bool evaluate(const Node& node, const ContextKeys& keys);

bool evaluate_comparison(const Node& node, const ContextKeys& keys) {
    if (!node.numeric) {
        return false;
    }
    const double value = to_number(keys.find(node.key));
    switch (node.kind) {
    case Kind::Greater:
        return value > node.number;
    case Kind::GreaterEquals:
        return value >= node.number;
    case Kind::Smaller:
        return value < node.number;
    default:
        return value <= node.number;
    }
}

bool evaluate_in(const Node& node, const ContextKeys& keys) {
    const ContextValue* source = keys.find(node.text);
    const ContextValue* item = keys.find(node.key);
    if (source == nullptr || item == nullptr) {
        return false;
    }
    const auto* list = std::get_if<std::vector<std::string>>(source);
    const auto* text = std::get_if<std::string>(item);
    return list != nullptr && text != nullptr && std::ranges::find(*list, *text) != list->end();
}

bool evaluate(const Node& node, const ContextKeys& keys) {
    switch (node.kind) {
    case Kind::False:
        return false;
    case Kind::True:
        return true;
    case Kind::Defined:
        return is_truthy(keys.find(node.key));
    case Kind::Not:
        return !is_truthy(keys.find(node.key));
    case Kind::Equals:
        return loosely_equals(keys.find(node.key), node.text);
    case Kind::NotEquals:
        return !loosely_equals(keys.find(node.key), node.text);
    case Kind::Regex:
        return node.regex->test(to_text(keys.find(node.key)));
    case Kind::NotRegex:
        return !node.regex->test(to_text(keys.find(node.key)));
    case Kind::In:
        return evaluate_in(node, keys);
    case Kind::NotIn:
        return !evaluate_in(node, keys);
    case Kind::Greater:
    case Kind::GreaterEquals:
    case Kind::Smaller:
    case Kind::SmallerEquals:
        return evaluate_comparison(node, keys);
    case Kind::And:
        return std::ranges::all_of(node.terms,
                                   [&](const NodePtr& term) { return evaluate(*term, keys); });
    case Kind::Or:
        return std::ranges::any_of(node.terms,
                                   [&](const NodePtr& term) { return evaluate(*term, keys); });
    }
    return false;
}

std::string serialize(const Node& node) {
    switch (node.kind) {
    case Kind::False:
        return "false";
    case Kind::True:
        return "true";
    case Kind::Defined:
        return node.key;
    case Kind::Not:
        return "!" + node.key;
    case Kind::Equals:
        return std::format("{} == '{}'", node.key, node.text);
    case Kind::NotEquals:
        return std::format("{} != '{}'", node.key, node.text);
    case Kind::Regex:
        return std::format("{} =~ /{}/{}", node.key, node.text, node.flags);
    case Kind::NotRegex:
        return std::format("!({} =~ /{}/{})", node.key, node.text, node.flags);
    case Kind::In:
        return std::format("{} in '{}'", node.key, node.text);
    case Kind::NotIn:
        return std::format("{} not in '{}'", node.key, node.text);
    case Kind::Greater:
    case Kind::GreaterEquals:
    case Kind::Smaller:
    case Kind::SmallerEquals: {
        const std::string_view op = node.kind == Kind::Greater         ? ">"
                                    : node.kind == Kind::GreaterEquals ? ">="
                                    : node.kind == Kind::Smaller       ? "<"
                                                                       : "<=";
        return std::format("{} {} {}", node.key, op,
                           node.numeric ? number_to_string(node.number) : node.text);
    }
    case Kind::And:
    case Kind::Or: {
        std::string result;
        for (const NodePtr& term : node.terms) {
            if (!result.empty()) {
                result += node.kind == Kind::And ? " && " : " || ";
            }
            result += serialize(*term);
        }
        return result;
    }
    }
    return {};
}

enum class Tok {
    left_paren,
    right_paren,
    bang,
    equal,
    not_equal,
    less,
    less_equal,
    greater,
    greater_equal,
    regex_match,
    regex_literal,
    true_keyword,
    false_keyword,
    in_keyword,
    not_keyword,
    and_and,
    or_or,
    word,
    quoted,
    invalid,
    end,
};

struct Token {
    Tok type = Tok::end;
    std::size_t offset = 0;
    // The text of the token. A quoted token has the text between its quotes, and `offset` is the
    // opening quote.
    std::string_view lexeme;
    // For an invalid token, what the author probably meant.
    std::string_view hint;
};

bool is_word_byte(char c) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte >= 0x80) {
        return true;
    }
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || is_digit(c) ||
           std::string_view("_<>-./\\:*?+[]^,#@;\"%$").find(c) != std::string_view::npos;
}

bool is_no_break_space(std::string_view text, std::size_t i) {
    return i + 1 < text.size() && static_cast<unsigned char>(text[i]) == 0xC2 &&
           static_cast<unsigned char>(text[i + 1]) == 0xA0;
}

// Splits a `when` expression into tokens. Text that is not a token becomes an invalid token that
// the parser reports where it stands, so errors have the position of the text at fault.
class Scanner {
public:
    explicit Scanner(std::string_view input) : input_(input) {}

    std::vector<Token> scan() {
        while (pos_ < input_.size()) {
            start_ = pos_;
            scan_one();
        }
        tokens_.push_back({Tok::end, input_.size(), {}, {}});
        return std::move(tokens_);
    }

private:
    void scan_one() {
        const char c = input_[pos_++];
        switch (c) {
        case '(':
            add(Tok::left_paren);
            break;
        case ')':
            add(Tok::right_paren);
            break;
        case '!':
            if (match('=')) {
                match('=');
                add(Tok::not_equal);
            } else {
                add(Tok::bang);
            }
            break;
        case '=':
            if (match('=')) {
                match('=');
                add(Tok::equal);
            } else if (match('~')) {
                add(Tok::regex_match);
            } else {
                invalid("did you mean '==' or '=~'?");
            }
            break;
        case '<':
            add(match('=') ? Tok::less_equal : Tok::less);
            break;
        case '>':
            add(match('=') ? Tok::greater_equal : Tok::greater);
            break;
        case '&':
            if (match('&')) {
                add(Tok::and_and);
            } else {
                invalid("did you mean '&&'?");
            }
            break;
        case '|':
            if (match('|')) {
                add(Tok::or_or);
            } else {
                invalid("did you mean '||'?");
            }
            break;
        case '\'':
            quoted();
            break;
        case '/':
            regex();
            break;
        case ' ':
        case '\t':
        case '\n':
        case '\r':
            break;
        default:
            if (is_no_break_space(input_, start_)) {
                ++pos_;
            } else if (is_word_byte(c)) {
                word();
            } else {
                invalid({});
            }
            break;
        }
    }

    bool match(char expected) {
        if (pos_ < input_.size() && input_[pos_] == expected) {
            ++pos_;
            return true;
        }
        return false;
    }

    void add(Tok type) {
        tokens_.push_back({type, start_, input_.substr(start_, pos_ - start_), {}});
    }

    void invalid(std::string_view hint) {
        tokens_.push_back({Tok::invalid, start_, input_.substr(start_, pos_ - start_), hint});
    }

    void word() {
        while (pos_ < input_.size() && is_word_byte(input_[pos_]) &&
               !is_no_break_space(input_, pos_)) {
            ++pos_;
        }
        const std::string_view lexeme = input_.substr(start_, pos_ - start_);
        Tok type = Tok::word;
        if (lexeme == "true") {
            type = Tok::true_keyword;
        } else if (lexeme == "false") {
            type = Tok::false_keyword;
        } else if (lexeme == "in") {
            type = Tok::in_keyword;
        } else if (lexeme == "not") {
            type = Tok::not_keyword;
        }
        add(type);
    }

    void quoted() {
        const std::size_t close = input_.find('\'', pos_);
        if (close == std::string_view::npos) {
            pos_ = input_.size();
            invalid("did you forget to close the quote?");
            return;
        }
        tokens_.push_back({Tok::quoted, start_, input_.substr(pos_, close - pos_), {}});
        pos_ = close + 1;
    }

    // `/pattern/flags`. A slash inside the pattern must be escaped or be in a character class.
    void regex() {
        bool escaped = false;
        bool in_class = false;
        while (true) {
            if (pos_ >= input_.size()) {
                invalid("did you forget to escape a '/' in the pattern with a backslash?");
                return;
            }
            const char c = input_[pos_++];
            if (escaped) {
                escaped = false;
            } else if (c == '/' && !in_class) {
                break;
            } else if (c == '[') {
                in_class = true;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == ']') {
                in_class = false;
            }
        }
        while (pos_ < input_.size() &&
               std::string_view("igsmyu").find(input_[pos_]) != std::string_view::npos) {
            ++pos_;
        }
        add(Tok::regex_literal);
    }

    std::string_view input_;
    std::size_t pos_ = 0;
    std::size_t start_ = 0;
    std::vector<Token> tokens_;
};

class Parser {
public:
    explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

    std::expected<NodePtr, Error> parse() {
        if (peek().type == Tok::end) {
            return make_constant(true);
        }
        NodePtr result = parse_or();
        if (result && peek().type != Tok::end) {
            const Token& token = peek();
            fail(token, token.type == Tok::word ? "did you forget '&&' or '||' before it?"
                                                : std::string_view());
            result = nullptr;
        }
        if (!result) {
            return std::unexpected(std::move(*error_));
        }
        return result;
    }

private:
    struct Value {
        std::string text;
        bool quoted = false;
    };

    const Token& peek() const {
        return tokens_[position_];
    }

    const Token& advance() {
        const Token& token = tokens_[position_];
        if (token.type != Tok::end) {
            ++position_;
        }
        return token;
    }

    bool match(Tok type) {
        if (peek().type == type) {
            advance();
            return true;
        }
        return false;
    }

    static std::string describe(const Token& token) {
        switch (token.type) {
        case Tok::end:
            return "end of the expression";
        case Tok::quoted:
            return std::format("'{}' in quotes", token.lexeme);
        default:
            return std::format("'{}'", token.lexeme);
        }
    }

    // Records the first problem only: a later one is a consequence of it.
    NodePtr fail(const Token& token, std::string_view hint, std::string_view expected = {}) {
        if (!error_) {
            std::string message =
                std::format("offset {}: unexpected {}", token.offset, describe(token));
            if (!expected.empty()) {
                message += std::format("; expected {}", expected);
            }
            const std::string_view help = token.hint.empty() ? hint : token.hint;
            if (!help.empty()) {
                message += std::format("; {}", help);
            }
            error_ = Error{ErrorCode::invalid_argument, std::move(message)};
        }
        return nullptr;
    }

    NodePtr parse_or() {
        NodePtr first = parse_and();
        if (!first || peek().type != Tok::or_or) {
            return first;
        }
        std::vector<NodePtr> terms{std::move(first)};
        while (match(Tok::or_or)) {
            NodePtr next = parse_and();
            if (!next) {
                return nullptr;
            }
            terms.push_back(std::move(next));
        }
        return make_or(std::move(terms), true);
    }

    NodePtr parse_and() {
        NodePtr first = parse_term();
        if (!first || peek().type != Tok::and_and) {
            return first;
        }
        std::vector<NodePtr> terms{std::move(first)};
        while (match(Tok::and_and)) {
            NodePtr next = parse_term();
            if (!next) {
                return nullptr;
            }
            terms.push_back(std::move(next));
        }
        return make_and(std::move(terms), true);
    }

    NodePtr parse_term() {
        if (!match(Tok::bang)) {
            return parse_primary();
        }
        const Token& token = peek();
        switch (token.type) {
        case Tok::true_keyword:
            advance();
            return make_constant(false);
        case Tok::false_keyword:
            advance();
            return make_constant(true);
        case Tok::left_paren: {
            advance();
            NodePtr inner = parse_or();
            if (!inner || !consume_right_paren()) {
                return nullptr;
            }
            return negate(inner);
        }
        case Tok::word:
            advance();
            return make_term(Kind::Not, std::string(token.lexeme));
        default:
            return fail(token, {}, "a context key, 'true', 'false' or '(' after '!'");
        }
    }

    bool consume_right_paren() {
        if (match(Tok::right_paren)) {
            return true;
        }
        fail(peek(), {}, "')'");
        return false;
    }

    Value parse_value() {
        const Token& token = peek();
        switch (token.type) {
        case Tok::word:
            advance();
            return {std::string(token.lexeme), false};
        case Tok::quoted:
            advance();
            return {std::string(token.lexeme), true};
        case Tok::true_keyword:
        case Tok::false_keyword:
        case Tok::in_keyword:
            advance();
            return {std::string(token.lexeme), false};
        default:
            // An operator without an operand compares with the empty text, as in `foo == `.
            return {};
        }
    }

    NodePtr parse_regex(std::string key) {
        const Token& token = peek();
        if (token.type != Tok::regex_literal) {
            return fail(token, {}, "a regular expression such as /pattern/");
        }
        advance();
        const std::size_t closing = token.lexeme.rfind('/');
        const std::string_view pattern = token.lexeme.substr(1, closing - 1);
        std::string flags;
        std::uint32_t options = 0;
        for (const char flag : token.lexeme.substr(closing + 1)) {
            if (flag == 'g' || flag == 'y') {
                continue;
            }
            if (flags.find(flag) != std::string::npos) {
                return fail_message(token, std::format("the flag '{}' is repeated", flag));
            }
            flags += flag;
            options |= flag == 'i'   ? PCRE2_CASELESS
                       : flag == 'm' ? PCRE2_MULTILINE
                       : flag == 's' ? PCRE2_DOTALL
                                     : 0;
        }
        std::ranges::sort(flags);
        auto regex = CompiledRegex::compile(pattern, options);
        if (!regex) {
            return fail_message(token, "invalid regular expression: " + regex.error());
        }
        Node node;
        node.kind = Kind::Regex;
        node.key = std::move(key);
        node.text = std::string(pattern);
        node.flags = std::move(flags);
        node.regex = *std::move(regex);
        return std::make_shared<const Node>(std::move(node));
    }

    NodePtr fail_message(const Token& token, std::string message) {
        if (!error_) {
            error_ = Error{ErrorCode::invalid_argument,
                           std::format("offset {}: {}", token.offset, message)};
        }
        return nullptr;
    }

    NodePtr parse_primary() {
        const Token& token = peek();
        switch (token.type) {
        case Tok::true_keyword:
            advance();
            return make_constant(true);
        case Tok::false_keyword:
            advance();
            return make_constant(false);
        case Tok::left_paren: {
            advance();
            NodePtr inner = parse_or();
            if (!inner || !consume_right_paren()) {
                return nullptr;
            }
            return inner;
        }
        case Tok::word:
            advance();
            return parse_key_term(std::string(token.lexeme));
        case Tok::end:
            return fail(token, "did you forget to write a context key?");
        default:
            return fail(token, {}, "a context key, 'true', 'false' or '('");
        }
    }

    // What follows a context key: nothing, or an operator with its operand.
    NodePtr parse_key_term(std::string key) {
        if (match(Tok::regex_match)) {
            return parse_regex(std::move(key));
        }
        if (match(Tok::not_keyword)) {
            if (!match(Tok::in_keyword)) {
                return fail(peek(), {}, "'in' after 'not'");
            }
            return make_term(Kind::NotIn, std::move(key), parse_value().text);
        }
        switch (peek().type) {
        case Tok::equal:
        case Tok::not_equal: {
            const bool equal = advance().type == Tok::equal;
            Value value = parse_value();
            if (!value.quoted && (value.text == "true" || value.text == "false")) {
                const bool wants_defined = (value.text == "true") == equal;
                return make_term(wants_defined ? Kind::Defined : Kind::Not, std::move(key));
            }
            return make_term(equal ? Kind::Equals : Kind::NotEquals, std::move(key),
                             std::move(value.text));
        }
        case Tok::less:
            advance();
            return make_comparison(Kind::Smaller, std::move(key), parse_value().text);
        case Tok::less_equal:
            advance();
            return make_comparison(Kind::SmallerEquals, std::move(key), parse_value().text);
        case Tok::greater:
            advance();
            return make_comparison(Kind::Greater, std::move(key), parse_value().text);
        case Tok::greater_equal:
            advance();
            return make_comparison(Kind::GreaterEquals, std::move(key), parse_value().text);
        case Tok::in_keyword:
            advance();
            return make_term(Kind::In, std::move(key), parse_value().text);
        default:
            return make_term(Kind::Defined, std::move(key));
        }
    }

    std::vector<Token> tokens_;
    std::size_t position_ = 0;
    std::optional<Error> error_;
};

} // namespace
} // namespace detail

WhenExpression::WhenExpression() {
    static const std::shared_ptr<const detail::WhenImpl> always_true =
        std::make_shared<const detail::WhenImpl>(
            detail::WhenImpl{detail::make_constant(true), "true"});
    impl_ = always_true;
}

std::expected<WhenExpression, Error> WhenExpression::parse(std::string_view text) {
    auto root = detail::Parser(detail::Scanner(text).scan()).parse();
    if (!root) {
        return std::unexpected(std::move(root.error()));
    }
    auto canonical = detail::serialize(**root);
    return WhenExpression(std::make_shared<const detail::WhenImpl>(
        detail::WhenImpl{*std::move(root), std::move(canonical)}));
}

bool WhenExpression::evaluate(const ContextKeys& keys) const {
    return detail::evaluate(*impl_->root, keys);
}

namespace detail {
namespace {

// Whether every term of `p` is among those of `q`; both are sorted.
bool all_included(const std::vector<NodePtr>& p, const std::vector<NodePtr>& q) {
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < p.size() && j < q.size()) {
        const int order = compare(*p[i], *q[j]);
        if (order < 0) {
            return false;
        }
        if (order == 0) {
            ++i;
        }
        ++j;
    }
    return i == p.size();
}

// The way VS Code proves it, on the normalized expressions: a proof that is not found does not mean
// that there is none.
bool implies_node(const NodePtr& p, const NodePtr& q) {
    if (p->kind == Kind::False || q->kind == Kind::True) {
        return true;
    }
    if (p->kind == Kind::Or) {
        return q->kind == Kind::Or && all_included(p->terms, q->terms);
    }
    if (q->kind == Kind::Or) {
        return std::ranges::any_of(q->terms,
                                   [&](const NodePtr& term) { return implies_node(p, term); });
    }
    if (p->kind == Kind::And) {
        if (q->kind == Kind::And) {
            return all_included(q->terms, p->terms);
        }
        return std::ranges::any_of(p->terms,
                                   [&](const NodePtr& term) { return implies_node(term, q); });
    }
    return node_equal(p, q);
}

} // namespace
} // namespace detail

bool WhenExpression::implies(const WhenExpression& other) const {
    return detail::implies_node(impl_->root, other.impl_->root);
}

bool WhenExpression::is_false() const {
    return impl_->root->kind == detail::Kind::False;
}

const std::string& WhenExpression::canonical() const {
    return impl_->canonical;
}

} // namespace anvil::core
