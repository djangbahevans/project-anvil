#include "anvil/core/jsonc.hpp"

#include "anvil/core/utf.hpp"

#include <charconv>
#include <cstdint>
#include <format>
#include <string>

namespace anvil::core {
namespace {

using nlohmann::json;

constexpr int max_depth = 256;
constexpr std::string_view byte_order_mark = "\xEF\xBB\xBF";

struct Failure {
    std::size_t offset;
    std::string message;
};

template <class T> using Result = std::expected<T, Failure>;

bool is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

void append_utf8(std::string& out, char32_t code_point) {
    if (code_point < 0x80) {
        out += static_cast<char>(code_point);
    } else if (code_point < 0x800) {
        out += static_cast<char>(0xC0 | (code_point >> 6));
        out += static_cast<char>(0x80 | (code_point & 0x3F));
    } else if (code_point < 0x10000) {
        out += static_cast<char>(0xE0 | (code_point >> 12));
        out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code_point & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (code_point >> 18));
        out += static_cast<char>(0x80 | ((code_point >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code_point & 0x3F));
    }
}

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {
        if (text_.starts_with(byte_order_mark)) {
            offset_ = byte_order_mark.size();
        }
        start_ = offset_;
    }

    Result<json> parse_document(JsoncNode* root = nullptr) {
        if (const auto invalid = find_invalid_utf8(text_.substr(start_))) {
            return fail(start_ + *invalid, "invalid UTF-8");
        }
        if (auto skipped = skip_trivia(); !skipped) {
            return std::unexpected(std::move(skipped.error()));
        }
        if (at_end()) {
            return fail("expected a value");
        }
        auto value = parse_value(0, root);
        if (!value) {
            return value;
        }
        if (auto skipped = skip_trivia(); !skipped) {
            return std::unexpected(std::move(skipped.error()));
        }
        if (!at_end()) {
            return fail("unexpected content after the value");
        }
        return value;
    }

    Result<bool> is_blank() {
        if (auto skipped = skip_trivia(); !skipped) {
            return std::unexpected(std::move(skipped.error()));
        }
        return at_end();
    }

    Error to_error(const Failure& failure) const {
        std::size_t line = 1;
        std::size_t column = 1;
        for (std::size_t i = start_; i < failure.offset && i < text_.size(); ++i) {
            const char c = text_[i];
            if (c == '\n' || (c == '\r' && (i + 1 >= text_.size() || text_[i + 1] != '\n'))) {
                ++line;
                column = 1;
            } else if ((static_cast<unsigned char>(c) & 0xC0u) != 0x80u && c != '\r') {
                ++column;
            }
        }
        return Error{ErrorCode::invalid_argument,
                     std::format("line {}, column {}: {}", line, column, failure.message)};
    }

private:
    bool at_end() const {
        return offset_ >= text_.size();
    }
    char peek() const {
        return text_[offset_];
    }

    std::unexpected<Failure> fail(std::string message) const {
        return fail(offset_, std::move(message));
    }
    static std::unexpected<Failure> fail(std::size_t offset, std::string message) {
        return std::unexpected(Failure{offset, std::move(message)});
    }

    std::string describe_next() const {
        if (at_end()) {
            return "the end of the input";
        }
        const auto lead = static_cast<unsigned char>(peek());
        if (lead < 0x20) {
            return std::format("control character U+{:04X}", lead);
        }
        std::size_t length = 1;
        while (offset_ + length < text_.size() &&
               (static_cast<unsigned char>(text_[offset_ + length]) & 0xC0u) == 0x80u) {
            ++length;
        }
        return std::format("'{}'", text_.substr(offset_, length));
    }

    Result<void> skip_trivia() {
        while (!at_end()) {
            const char c = peek();
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++offset_;
            } else if (c == '/' && offset_ + 1 < text_.size() && text_[offset_ + 1] == '/') {
                while (!at_end() && peek() != '\n' && peek() != '\r') {
                    ++offset_;
                }
            } else if (c == '/' && offset_ + 1 < text_.size() && text_[offset_ + 1] == '*') {
                const std::size_t begin = offset_;
                const std::size_t close = text_.find("*/", offset_ + 2);
                if (close == std::string_view::npos) {
                    return fail(begin, "unterminated comment");
                }
                offset_ = close + 2;
            } else {
                break;
            }
        }
        return {};
    }

    // `node`, when given, receives the value's kind and range, and for an object or an array what
    // it holds.
    Result<json> parse_value(int depth, JsoncNode* node = nullptr) {
        if (depth > max_depth) {
            return fail("nesting is too deep");
        }
        if (node) {
            node->begin = offset_;
        }
        auto value = parse_value_of_kind(depth, node);
        if (value && node) {
            node->end = offset_;
        }
        return value;
    }

    Result<json> parse_value_of_kind(int depth, JsoncNode* node) {
        using Kind = JsoncNode::Kind;
        const auto kind = [&](Kind of) {
            if (node) {
                node->kind = of;
            }
        };
        switch (peek()) {
        case '{':
            kind(Kind::object);
            return parse_object(depth, node);
        case '[':
            kind(Kind::array);
            return parse_array(depth, node);
        case '"': {
            kind(Kind::string);
            auto text = parse_string();
            if (!text) {
                return std::unexpected(std::move(text.error()));
            }
            return json(std::move(*text));
        }
        case 't':
            kind(Kind::boolean);
            return parse_literal("true", json(true));
        case 'f':
            kind(Kind::boolean);
            return parse_literal("false", json(false));
        case 'n':
            kind(Kind::null);
            return parse_literal("null", json(nullptr));
        default:
            if (peek() == '-' || is_digit(peek())) {
                kind(Kind::number);
                return parse_number();
            }
            return fail(std::format("unexpected {}, expected a value", describe_next()));
        }
    }

    Result<json> parse_literal(std::string_view word, json value) {
        if (!text_.substr(offset_).starts_with(word)) {
            return fail(std::format("unexpected {}, expected a value", describe_next()));
        }
        offset_ += word.size();
        return value;
    }

    Result<json> parse_number() {
        const std::size_t begin = offset_;
        std::size_t i = offset_;
        auto digits_at = [&](std::size_t from) {
            std::size_t end = from;
            while (end < text_.size() && is_digit(text_[end])) {
                ++end;
            }
            return end;
        };
        bool integral = true;
        if (text_[i] == '-') {
            ++i;
        }
        if (i >= text_.size() || !is_digit(text_[i])) {
            offset_ = i;
            return fail("expected a digit");
        }
        if (text_[i] == '0') {
            ++i;
        } else {
            i = digits_at(i);
        }
        if (i < text_.size() && text_[i] == '.') {
            integral = false;
            const std::size_t end = digits_at(i + 1);
            if (end == i + 1) {
                offset_ = i + 1;
                return fail("expected a digit after the decimal point");
            }
            i = end;
        }
        if (i < text_.size() && (text_[i] == 'e' || text_[i] == 'E')) {
            integral = false;
            std::size_t exponent = i + 1;
            if (exponent < text_.size() && (text_[exponent] == '+' || text_[exponent] == '-')) {
                ++exponent;
            }
            const std::size_t end = digits_at(exponent);
            if (end == exponent) {
                offset_ = exponent;
                return fail("expected a digit in the exponent");
            }
            i = end;
        }

        const std::string_view number = text_.substr(begin, i - begin);
        offset_ = i;
        if (integral) {
            if (number.front() == '-') {
                std::int64_t value = 0;
                if (std::from_chars(number.data(), number.data() + number.size(), value).ec ==
                    std::errc{}) {
                    return json(value);
                }
            } else {
                std::uint64_t value = 0;
                if (std::from_chars(number.data(), number.data() + number.size(), value).ec ==
                    std::errc{}) {
                    return json(value);
                }
            }
        }
        double value = 0;
        const auto parsed = std::from_chars(number.data(), number.data() + number.size(), value);
        if (parsed.ec != std::errc{}) {
            return fail(begin, "the number is out of range");
        }
        return json(value);
    }

    Result<char32_t> parse_hex4() {
        if (offset_ + 4 > text_.size()) {
            return fail(text_.size(), "unterminated string");
        }
        char32_t value = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            const char c = text_[offset_ + i];
            if (!is_hex_digit(c)) {
                return fail(offset_ + i, "expected four hexadecimal digits after \\u");
            }
            value = value * 16 + static_cast<char32_t>(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
        }
        offset_ += 4;
        return value;
    }

    Result<std::string> parse_string() {
        const std::size_t begin = offset_;
        ++offset_;
        std::string out;
        while (true) {
            if (at_end()) {
                return fail(begin, "unterminated string");
            }
            const char c = peek();
            if (c == '"') {
                ++offset_;
                return out;
            }
            if (c == '\n' || c == '\r') {
                return fail(begin, "unterminated string");
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return fail(std::format("unexpected {} in a string", describe_next()));
            }
            if (c != '\\') {
                out += c;
                ++offset_;
                continue;
            }

            const std::size_t escape = offset_;
            ++offset_;
            if (at_end()) {
                return fail(begin, "unterminated string");
            }
            const char kind = peek();
            ++offset_;
            switch (kind) {
            case '"':
            case '\\':
            case '/':
                out += kind;
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            case 'u': {
                auto unit = parse_hex4();
                if (!unit) {
                    return std::unexpected(std::move(unit.error()));
                }
                char32_t code_point = *unit;
                if (code_point >= 0xD800 && code_point <= 0xDBFF) {
                    if (!text_.substr(offset_).starts_with("\\u")) {
                        return fail(escape, "a high surrogate must be followed by a low surrogate");
                    }
                    offset_ += 2;
                    auto low = parse_hex4();
                    if (!low) {
                        return std::unexpected(std::move(low.error()));
                    }
                    if (*low < 0xDC00 || *low > 0xDFFF) {
                        return fail(escape, "a high surrogate must be followed by a low surrogate");
                    }
                    code_point = 0x10000 + ((code_point - 0xD800) << 10) + (*low - 0xDC00);
                } else if (code_point >= 0xDC00 && code_point <= 0xDFFF) {
                    return fail(escape, "a low surrogate must follow a high surrogate");
                }
                append_utf8(out, code_point);
                break;
            }
            default:
                return fail(escape, std::format("invalid escape sequence '\\{}'", kind));
            }
        }
    }

    Result<json> parse_array(int depth, JsoncNode* node) {
        ++offset_;
        json result = json::array();
        while (true) {
            if (auto skipped = skip_trivia(); !skipped) {
                return std::unexpected(std::move(skipped.error()));
            }
            if (at_end()) {
                return fail("unterminated array");
            }
            if (peek() == ']') {
                if (node) {
                    node->close = offset_;
                }
                ++offset_;
                return result;
            }
            JsoncNode* slot = nullptr;
            if (node) {
                slot = &node->elements.emplace_back();
            }
            auto element = parse_value(depth + 1, slot);
            if (!element) {
                return element;
            }
            result.push_back(std::move(*element));
            if (auto skipped = skip_trivia(); !skipped) {
                return std::unexpected(std::move(skipped.error()));
            }
            if (at_end()) {
                return fail("unterminated array");
            }
            if (peek() == ',') {
                ++offset_;
            } else if (peek() != ']') {
                return fail(std::format("unexpected {}, expected ',' or ']'", describe_next()));
            }
        }
    }

    Result<json> parse_object(int depth, JsoncNode* node) {
        ++offset_;
        json result = json::object();
        while (true) {
            if (auto skipped = skip_trivia(); !skipped) {
                return std::unexpected(std::move(skipped.error()));
            }
            if (at_end()) {
                return fail("unterminated object");
            }
            if (peek() == '}') {
                if (node) {
                    node->close = offset_;
                }
                ++offset_;
                return result;
            }
            if (peek() != '"') {
                return fail(
                    std::format("unexpected {}, expected a property name", describe_next()));
            }
            const std::size_t name_begin = offset_;
            auto key = parse_string();
            if (!key) {
                return std::unexpected(std::move(key.error()));
            }
            JsoncMember* member = nullptr;
            if (node) {
                member = &node->members.emplace_back();
                member->name = *key;
                member->name_begin = name_begin;
                member->name_end = offset_;
            }
            if (auto skipped = skip_trivia(); !skipped) {
                return std::unexpected(std::move(skipped.error()));
            }
            if (at_end() || peek() != ':') {
                return fail(std::format("unexpected {}, expected ':'", describe_next()));
            }
            ++offset_;
            if (auto skipped = skip_trivia(); !skipped) {
                return std::unexpected(std::move(skipped.error()));
            }
            if (at_end()) {
                return fail("expected a value");
            }
            auto value = parse_value(depth + 1, member ? &member->value : nullptr);
            if (!value) {
                return value;
            }
            result[*key] = std::move(*value);
            if (auto skipped = skip_trivia(); !skipped) {
                return std::unexpected(std::move(skipped.error()));
            }
            if (at_end()) {
                return fail("unterminated object");
            }
            if (peek() == ',') {
                if (member) {
                    member->comma = offset_;
                }
                ++offset_;
            } else if (peek() != '}') {
                return fail(std::format("unexpected {}, expected ',' or '}}'", describe_next()));
            }
        }
    }

    std::string_view text_;
    std::size_t offset_ = 0;
    std::size_t start_ = 0;
};

} // namespace

std::expected<json, Error> parse_jsonc(std::string_view text) {
    Parser parser(text);
    auto result = parser.parse_document();
    if (!result) {
        return std::unexpected(parser.to_error(result.error()));
    }
    return std::move(*result);
}

std::expected<JsoncNode, Error> parse_jsonc_tree(std::string_view text) {
    Parser parser(text);
    JsoncNode root;
    auto result = parser.parse_document(&root);
    if (!result) {
        return std::unexpected(parser.to_error(result.error()));
    }
    return root;
}

bool is_blank_jsonc(std::string_view text) {
    Parser parser(text);
    const auto blank = parser.is_blank();
    return blank && *blank;
}

} // namespace anvil::core
