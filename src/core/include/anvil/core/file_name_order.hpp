#pragma once

#include <compare>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace anvil::core {

// Orders two file names as the explorer of VS Code does (`compareFileNamesDefault`, which is an
// `Intl.Collator` with `numeric: true` followed by a comparison of the lengths). The collation is
// the root one of ICU, reproduced for the characters that file names mostly hold:
//   - Digit runs compare as numbers, ignoring leading zeros.
//   - Whitespace and punctuation sort before digits, digits before letters, and ASCII letters
//     ignore case and accents at first. A name that differs only by accents then sorts by them
//     (acute before grave before breve, ...), and one that differs only by case sorts the lower
//     case first.
//   - Latin letters of Latin-1 with a diacritic sort next to their base letter; `ø` sorts as `o`
//     with a stroke, `æ` as `ae` and `ß` as `ss`. Any other character sorts after the letters by
//     code point, which is not the order of ICU for other scripts.
//   - Control characters are ignored.
// Names that the collator treats as equal (`a1` and `a01`) sort the shorter, measured in UTF-16
// units as JavaScript does, first. Any remaining tie is broken by the bytes, so that only
// identical names compare equal. A byte that is not part of valid UTF-8 sorts after every
// character.
std::strong_ordering compare_file_names(std::string_view a, std::string_view b);

// The collation keys of one name, computed once so that sorting many names does not decode them on
// every comparison. `FileNameKey(a) <=> FileNameKey(b)` equals `compare_file_names(a, b)`.
class FileNameKey {
public:
    explicit FileNameKey(std::string_view name);

    std::strong_ordering operator<=>(const FileNameKey& other) const;
    bool operator==(const FileNameKey& other) const {
        return (*this <=> other) == 0;
    }

private:
    enum class Kind : std::uint8_t { punctuation, digits, letter, other };

    struct Element {
        Kind kind = Kind::other;
        // Punctuation: the rank among the ASCII punctuation. Letter: `a` is 0. Other: the code
        // point.
        std::uint32_t value = 0;
        // The digits of a number without leading zeros.
        std::string digits;
        // The weight of the accent in the order of the root collation; zero is none.
        std::uint8_t accent = 0;
        bool upper = false;
    };

    std::vector<Element> elements_;
    std::size_t utf16_length_ = 0;
    std::string bytes_;
};

} // namespace anvil::core
