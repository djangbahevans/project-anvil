#include "anvil/core/text.hpp"

#include "anvil/core/utf.hpp"
#include "rope.hpp"

#include <algorithm>
#include <format>

namespace anvil::core {

std::string normalize_line_endings(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            result.push_back('\n');
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                ++i;
            }
        } else {
            result.push_back(text[i]);
        }
    }
    return result;
}

Text::Text() = default;
Text::Text(std::shared_ptr<const rope::Node> root) : root_(std::move(root)) {}

std::expected<Text, Error> Text::from_utf8(std::string_view text) {
    if (auto valid = core::utf16_length(text); !valid) {
        return std::unexpected(valid.error());
    }
    const bool has_carriage_return = text.find('\r') != std::string_view::npos;
    return Text(
        rope::build(has_carriage_return ? std::string_view(normalize_line_endings(text)) : text));
}

std::size_t Text::size() const {
    return rope::byte_size(root_);
}

std::size_t Text::utf16_length() const {
    return root_ ? root_->summary.utf16 : 0;
}

std::size_t Text::line_count() const {
    return (root_ ? root_->summary.newlines : 0) + 1;
}

std::string Text::to_string() const {
    return slice(0, size());
}

std::string Text::slice(std::size_t offset, std::size_t length) const {
    std::string result;
    result.reserve(std::min(length, size()));
    rope::append_range(root_, offset, length, result);
    return result;
}

std::size_t Text::line_start(std::size_t line) const {
    if (line >= line_count()) {
        return size();
    }
    return rope::offset_of_line(root_, line);
}

std::size_t Text::line_end(std::size_t line) const {
    if (line + 1 < line_count()) {
        return rope::offset_of_line(root_, line + 1) - 1;
    }
    return size();
}

std::string Text::line(std::size_t line) const {
    if (line >= line_count()) {
        return {};
    }
    const std::size_t start = line_start(line);
    return slice(start, line_end(line) - start);
}

bool Text::is_char_boundary(std::size_t offset) const {
    return rope::is_char_boundary(root_, offset);
}

std::size_t Text::utf16_offset(std::size_t offset) const {
    return rope::utf16_at_offset(root_, std::min(offset, size()));
}

Position Text::position_at(std::size_t offset) const {
    offset = std::min(offset, size());
    const std::size_t line = rope::line_at_offset(root_, offset);
    const std::size_t line_offset = rope::offset_of_line(root_, line);
    return {line, rope::utf16_at_offset(root_, offset) - rope::utf16_at_offset(root_, line_offset)};
}

std::size_t Text::offset_at(Position position) const {
    if (position.line >= line_count()) {
        return size();
    }
    const std::size_t start = rope::offset_of_line(root_, position.line);
    const std::size_t start_units = rope::utf16_at_offset(root_, start);
    const std::size_t end_units = rope::utf16_at_offset(root_, line_end(position.line));
    const std::size_t column = std::min(position.column, end_units - start_units);
    return rope::offset_at_utf16(root_, start_units + column);
}

std::expected<Text, Error> Text::replaced(std::size_t start, std::size_t end,
                                          std::string_view replacement) const {
    if (start > end || end > size()) {
        return std::unexpected(Error{
            ErrorCode::invalid_argument,
            std::format("range [{}, {}) is invalid for text of {} bytes", start, end, size()),
        });
    }
    if (!is_char_boundary(start) || !is_char_boundary(end)) {
        return std::unexpected(
            Error{ErrorCode::invalid_argument, "range boundaries must fall between characters"});
    }
    if (auto valid = core::utf16_length(replacement); !valid) {
        return std::unexpected(valid.error());
    }

    const std::string normalized = normalize_line_endings(replacement);
    auto [head, rest] = rope::split(root_, end);
    auto [left, removed] = rope::split(head, start);
    (void)removed;

    // Merge the chunks next to the edit with the new text so repeated small edits do not leave a
    // trail of tiny chunks.
    std::string middle;
    if (left) {
        auto [remaining, chunk] = rope::pop_last_leaf(left);
        left = std::move(remaining);
        middle = std::move(chunk);
    }
    middle += normalized;
    if (rest) {
        auto [remaining, chunk] = rope::pop_first_leaf(rest);
        rest = std::move(remaining);
        middle += chunk;
    }
    return Text(rope::concat(rope::concat(left, rope::build(middle)), rest));
}

std::optional<std::string> Text::check_invariants() const {
    return rope::check_invariants(root_);
}

std::size_t Text::chunk_count() const {
    return rope::chunk_count(root_);
}

} // namespace anvil::core
