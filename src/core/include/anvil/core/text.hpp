#pragma once

#include "anvil/core/error.hpp"

#include <compare>
#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace anvil::core::rope {
struct Node;
}

namespace anvil::core {

// A zero-based line and a column counted in UTF-16 code units, the unit that the extension API and
// the Language Server Protocol use.
struct Position {
    std::size_t line = 0;
    std::size_t column = 0;

    auto operator<=>(const Position&) const = default;
};

struct Range {
    Position start;
    Position end;

    bool operator==(const Range&) const = default;
};

// Replaces "\r\n" and lone "\r" with "\n".
std::string normalize_line_endings(std::string_view text);

// Immutable text. Copying is constant time and shares storage, so a copy is a snapshot that stays
// valid, and can be read from any thread, while the text it came from is edited.
//
// The text is valid UTF-8 whose only line break is "\n"; the loader and the saver convert the
// line ending style of files. Offsets are byte offsets on character boundaries. Columns are UTF-16
// code units, and a column that falls inside a surrogate pair maps to the start of the pair.
class Text {
public:
    Text();

    // Fails with ErrorCode::invalid_encoding for malformed UTF-8. Line breaks are normalized.
    static std::expected<Text, Error> from_utf8(std::string_view text);

    std::size_t size() const;
    bool empty() const {
        return size() == 0;
    }
    // Length in UTF-16 code units.
    std::size_t utf16_length() const;
    // At least one; a trailing line break starts an empty last line.
    std::size_t line_count() const;

    std::string to_string() const;
    // The bytes of [offset, offset + length), clamped to the end of the text.
    std::string slice(std::size_t offset, std::size_t length) const;
    // The same range as text that shares storage with this one instead of copying it. The
    // range's ends must fall on character boundaries.
    Text substr(std::size_t offset, std::size_t length) const;

    // Offset of the first byte of `line`; the size of the text for a line past the end.
    std::size_t line_start(std::size_t line) const;
    // Offset of the line break ending `line`, or the size of the text for the last line.
    std::size_t line_end(std::size_t line) const;
    // The text of `line` without its line break; empty for a line past the end.
    std::string line(std::size_t line) const;

    bool is_char_boundary(std::size_t offset) const;

    // The number of UTF-16 code units before `offset`, clamped to the end of the text.
    std::size_t utf16_offset(std::size_t offset) const;

    // The position of `offset`, clamped to the end of the text.
    Position position_at(std::size_t offset) const;
    // The offset of `position`. A line past the end maps to the end of the text and a column past
    // the end of its line maps to the end of that line.
    std::size_t offset_at(Position position) const;

    // The text with [start, end) replaced. Fails with ErrorCode::invalid_argument when the range
    // is reversed, out of bounds or splits a character, and with ErrorCode::invalid_encoding when
    // `replacement` is not valid UTF-8. Line breaks in `replacement` are normalized.
    std::expected<Text, Error> replaced(std::size_t start, std::size_t end,
                                        std::string_view replacement) const;

    // Like replaced(start, end, std::string_view), with a replacement that is already valid and
    // normalized. Large replacements are spliced in without copying.
    std::expected<Text, Error> replaced(std::size_t start, std::size_t end,
                                        const Text& replacement) const;

    // Diagnostics for tests and benchmarks: the first violated internal invariant, if any, and the
    // number of storage chunks.
    std::optional<std::string> check_invariants() const;
    std::size_t chunk_count() const;

private:
    explicit Text(std::shared_ptr<const rope::Node> root);

    std::shared_ptr<const rope::Node> root_;
};

} // namespace anvil::core
