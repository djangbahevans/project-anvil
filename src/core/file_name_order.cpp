#include "anvil/core/file_name_order.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace anvil::core {

namespace {

// Secondary weights, in the order of the root collation; zero is no accent.
namespace Accent {
inline constexpr std::uint8_t none = 0;
inline constexpr std::uint8_t acute = 1;
inline constexpr std::uint8_t grave = 2;
inline constexpr std::uint8_t circumflex = 3;
inline constexpr std::uint8_t ring = 4;
inline constexpr std::uint8_t diaeresis = 5;
inline constexpr std::uint8_t tilde = 6;
inline constexpr std::uint8_t cedilla = 7;
inline constexpr std::uint8_t stroke = 8;
inline constexpr std::uint8_t ligature = 9;
} // namespace Accent

// ASCII whitespace, punctuation and symbols in the order of the root collation.
constexpr std::string_view punctuation_order = " _-,;:!?.'\"()[]{}@*/\\&#%`^+<=>|~$";

// Symbols outside ASCII (punctuation, arrows, mathematical and other signs, emoji) sort after `~`
// and before `$`, by code point: the rank of an ASCII character is multiplied by the stride, and
// a symbol has the rank of `~` plus its code point, which is smaller than the stride.
constexpr std::uint32_t symbol_stride = 0x20000;
constexpr std::uint32_t symbols_rank = static_cast<std::uint32_t>(punctuation_order.size()) - 2;
static_assert(0x1FAFF < symbol_stride);

bool is_symbol(char32_t c) {
    return (c >= 0x2000 && c <= 0x2BFF) || (c >= 0x1F000 && c <= 0x1FAFF);
}

struct Decomposition {
    char32_t character;
    char base;
    std::uint8_t accent;
};

constexpr std::array decompositions = std::to_array<Decomposition>({
    {U'À', 'a', Accent::grave},      {U'Á', 'a', Accent::acute},
    {U'Â', 'a', Accent::circumflex}, {U'Ã', 'a', Accent::tilde},
    {U'Ä', 'a', Accent::diaeresis},  {U'Å', 'a', Accent::ring},
    {U'Ç', 'c', Accent::cedilla},    {U'È', 'e', Accent::grave},
    {U'É', 'e', Accent::acute},      {U'Ê', 'e', Accent::circumflex},
    {U'Ë', 'e', Accent::diaeresis},  {U'Ì', 'i', Accent::grave},
    {U'Í', 'i', Accent::acute},      {U'Î', 'i', Accent::circumflex},
    {U'Ï', 'i', Accent::diaeresis},  {U'Ñ', 'n', Accent::tilde},
    {U'Ò', 'o', Accent::grave},      {U'Ó', 'o', Accent::acute},
    {U'Ô', 'o', Accent::circumflex}, {U'Õ', 'o', Accent::tilde},
    {U'Ö', 'o', Accent::diaeresis},  {U'Ø', 'o', Accent::stroke},
    {U'Ù', 'u', Accent::grave},      {U'Ú', 'u', Accent::acute},
    {U'Û', 'u', Accent::circumflex}, {U'Ü', 'u', Accent::diaeresis},
    {U'Ý', 'y', Accent::acute},      {U'à', 'a', Accent::grave},
    {U'á', 'a', Accent::acute},      {U'â', 'a', Accent::circumflex},
    {U'ã', 'a', Accent::tilde},      {U'ä', 'a', Accent::diaeresis},
    {U'å', 'a', Accent::ring},       {U'ç', 'c', Accent::cedilla},
    {U'è', 'e', Accent::grave},      {U'é', 'e', Accent::acute},
    {U'ê', 'e', Accent::circumflex}, {U'ë', 'e', Accent::diaeresis},
    {U'ì', 'i', Accent::grave},      {U'í', 'i', Accent::acute},
    {U'î', 'i', Accent::circumflex}, {U'ï', 'i', Accent::diaeresis},
    {U'ñ', 'n', Accent::tilde},      {U'ò', 'o', Accent::grave},
    {U'ó', 'o', Accent::acute},      {U'ô', 'o', Accent::circumflex},
    {U'õ', 'o', Accent::tilde},      {U'ö', 'o', Accent::diaeresis},
    {U'ø', 'o', Accent::stroke},     {U'ù', 'u', Accent::grave},
    {U'ú', 'u', Accent::acute},      {U'û', 'u', Accent::circumflex},
    {U'ü', 'u', Accent::diaeresis},  {U'ý', 'y', Accent::acute},
    {U'ÿ', 'y', Accent::diaeresis},
});

constexpr char32_t invalid_byte_base = 0x110000;

std::vector<char32_t> decode(std::string_view text) {
    std::vector<char32_t> out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        std::size_t length = 0;
        char32_t code = 0;
        if (lead < 0x80) {
            length = 1;
            code = lead;
        } else if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
            code = lead & 0x1Fu;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3;
            code = lead & 0x0Fu;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4;
            code = lead & 0x07u;
        }
        bool valid = length != 0 && i + length <= text.size();
        for (std::size_t k = 1; valid && k < length; ++k) {
            const auto next = static_cast<unsigned char>(text[i + k]);
            valid = (next & 0xC0u) == 0x80u;
            code = (code << 6) | (next & 0x3Fu);
        }
        constexpr std::array<char32_t, 5> smallest = {0, 0, 0x80, 0x800, 0x10000};
        valid = valid && code >= smallest[length] && code <= 0x10FFFF &&
                !(code >= 0xD800 && code <= 0xDFFF);
        if (valid) {
            out.push_back(code);
            i += length;
        } else {
            out.push_back(invalid_byte_base + static_cast<unsigned char>(text[i]));
            ++i;
        }
    }
    return out;
}

bool is_ascii_digit(char32_t c) {
    return c >= U'0' && c <= U'9';
}

bool is_ignorable(char32_t c) {
    return c < 0x20 || c == 0x7F || (c >= 0x80 && c < 0xA0);
}

// The lower case of a Greek or Cyrillic capital and whether `c` was one; other characters are
// returned as they are.
std::pair<char32_t, bool> fold_case(char32_t c) {
    const bool greek = c >= 0x391 && c <= 0x3A9 && c != 0x3A2;
    const bool cyrillic = c >= 0x410 && c <= 0x42F;
    const bool cyrillic_extended = c >= 0x400 && c <= 0x40F;
    if (greek || cyrillic) {
        return {c + 0x20, true};
    }
    if (cyrillic_extended) {
        return {c + 0x50, true};
    }
    return {c, false};
}

std::size_t utf16_length_of(const std::vector<char32_t>& characters) {
    std::size_t length = 0;
    for (const char32_t c : characters) {
        length += (c >= 0x10000 && c < invalid_byte_base) ? 2 : 1;
    }
    return length;
}

} // namespace

FileNameKey::FileNameKey(std::string_view name) : bytes_(name) {
    const std::vector<char32_t> characters = decode(name);
    utf16_length_ = utf16_length_of(characters);
    elements_.reserve(characters.size());

    const auto push_letter = [this](char base, std::uint8_t accent, bool upper) {
        elements_.push_back(Element{.kind = Kind::letter,
                                    .value = static_cast<std::uint32_t>(base - 'a'),
                                    .digits = {},
                                    .accent = accent,
                                    .upper = upper});
    };
    const auto push_other = [this](Kind kind, std::uint32_t value) {
        elements_.push_back(
            Element{.kind = kind, .value = value, .digits = {}, .accent = 0, .upper = false});
    };

    for (std::size_t i = 0; i < characters.size();) {
        const char32_t c = characters[i];
        if (is_ignorable(c)) {
            ++i;
        } else if (is_ascii_digit(c)) {
            std::size_t end = i;
            while (end < characters.size() && is_ascii_digit(characters[end])) {
                ++end;
            }
            std::size_t first = i;
            while (first + 1 < end && characters[first] == U'0') {
                ++first;
            }
            Element number;
            number.kind = Kind::digits;
            for (std::size_t k = first; k < end; ++k) {
                number.digits.push_back(static_cast<char>(characters[k]));
            }
            elements_.push_back(std::move(number));
            i = end;
        } else if ((c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z')) {
            push_letter(static_cast<char>(c | 0x20), Accent::none, c <= U'Z');
            ++i;
        } else if (c == U'æ' || c == U'Æ') {
            push_letter('a', Accent::ligature, c == U'Æ');
            push_letter('e', Accent::ligature, c == U'Æ');
            ++i;
        } else if (c == U'ß') {
            push_letter('s', Accent::ligature, false);
            push_letter('s', Accent::ligature, false);
            ++i;
        } else if (const auto found =
                       std::ranges::find(decompositions, c, &Decomposition::character);
                   found != decompositions.end()) {
            push_letter(found->base, found->accent, c < U'ß');
            ++i;
        } else if (const auto rank = c < 0x80 ? punctuation_order.find(static_cast<char>(c))
                                              : std::string_view::npos;
                   rank != std::string_view::npos) {
            push_other(Kind::punctuation, static_cast<std::uint32_t>(rank) * symbol_stride);
            ++i;
        } else if (is_symbol(c)) {
            push_other(Kind::punctuation,
                       symbols_rank * symbol_stride + static_cast<std::uint32_t>(c));
            ++i;
        } else {
            const auto [lower, upper] = fold_case(c);
            elements_.push_back(Element{.kind = Kind::other,
                                        .value = static_cast<std::uint32_t>(lower),
                                        .digits = {},
                                        .accent = Accent::none,
                                        .upper = upper});
            ++i;
        }
    }
}

std::strong_ordering FileNameKey::operator<=>(const FileNameKey& other) const {
    const std::size_t shared = std::min(elements_.size(), other.elements_.size());
    for (std::size_t i = 0; i < shared; ++i) {
        const Element& a = elements_[i];
        const Element& b = other.elements_[i];
        if (a.kind != b.kind) {
            return a.kind <=> b.kind;
        }
        if (a.kind == Kind::digits) {
            if (a.digits.size() != b.digits.size()) {
                return a.digits.size() <=> b.digits.size();
            }
            if (const auto order = a.digits.compare(b.digits) <=> 0; order != 0) {
                return order;
            }
        } else if (const auto order = a.value <=> b.value; order != 0) {
            return order;
        }
    }
    if (elements_.size() != other.elements_.size()) {
        return elements_.size() <=> other.elements_.size();
    }
    for (std::size_t i = 0; i < shared; ++i) {
        if (const auto order = elements_[i].accent <=> other.elements_[i].accent; order != 0) {
            return order;
        }
    }
    for (std::size_t i = 0; i < shared; ++i) {
        if (const auto order = elements_[i].upper <=> other.elements_[i].upper; order != 0) {
            return order;
        }
    }
    if (utf16_length_ != other.utf16_length_) {
        return utf16_length_ <=> other.utf16_length_;
    }
    return bytes_.compare(other.bytes_) <=> 0;
}

std::strong_ordering compare_file_names(std::string_view a, std::string_view b) {
    return FileNameKey(a) <=> FileNameKey(b);
}

} // namespace anvil::core
