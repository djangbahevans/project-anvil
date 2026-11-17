#include "anvil/core/text_buffer.hpp"

#include <utility>

namespace anvil::core {

std::expected<TextChange, Error> TextBuffer::replace(std::size_t start, std::size_t end,
                                                     std::string_view replacement) {
    auto updated = text_.replaced(start, end, replacement);
    if (!updated) {
        return std::unexpected(std::move(updated.error()));
    }

    TextChange change;
    change.offset = start;
    change.removed_bytes = end - start;
    change.range = {text_.position_at(start), text_.position_at(end)};
    change.utf16_offset = text_.utf16_offset(start);
    change.removed_utf16 = text_.utf16_offset(end) - change.utf16_offset;
    change.text = normalize_line_endings(replacement);
    text_ = std::move(*updated);
    return change;
}

std::expected<TextChange, Error> TextBuffer::replace(std::size_t start, std::size_t end,
                                                     const Text& replacement) {
    auto updated = text_.replaced(start, end, replacement);
    if (!updated) {
        return std::unexpected(std::move(updated.error()));
    }

    TextChange change;
    change.offset = start;
    change.removed_bytes = end - start;
    change.range = {text_.position_at(start), text_.position_at(end)};
    change.utf16_offset = text_.utf16_offset(start);
    change.removed_utf16 = text_.utf16_offset(end) - change.utf16_offset;
    change.text = replacement.to_string();
    text_ = std::move(*updated);
    return change;
}

std::expected<TextChange, Error> TextBuffer::replace(Range range, std::string_view replacement) {
    return replace(text_.offset_at(range.start), text_.offset_at(range.end), replacement);
}

} // namespace anvil::core
