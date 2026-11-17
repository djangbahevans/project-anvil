#pragma once

#include "anvil/core/error.hpp"
#include "anvil/core/text.hpp"

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

namespace anvil::core {

// One edit, described in terms of the text before it was applied.
struct TextChange {
    // Byte offset and length of the replaced range.
    std::size_t offset = 0;
    std::size_t removed_bytes = 0;
    // The replaced range with UTF-16 columns, its start as a UTF-16 offset from the start of the
    // text, and its length in UTF-16 code units.
    Range range;
    std::size_t utf16_offset = 0;
    std::size_t removed_utf16 = 0;
    // The inserted text, with normalized line breaks.
    std::string text;
};

// The current text of a document. Edits replace the text and report what changed; snapshot() hands
// out the text as of now, which later edits do not affect.
class TextBuffer {
public:
    TextBuffer() = default;
    explicit TextBuffer(Text text) : text_(std::move(text)) {}

    const Text& snapshot() const {
        return text_;
    }

    // Replaces [start, end) with `replacement`; see Text::replaced for the failure cases. The text
    // is unchanged when an edit fails.
    std::expected<TextChange, Error> replace(std::size_t start, std::size_t end,
                                             std::string_view replacement);
    std::expected<TextChange, Error> insert(std::size_t offset, std::string_view text) {
        return replace(offset, offset, text);
    }
    std::expected<TextChange, Error> erase(std::size_t start, std::size_t end) {
        return replace(start, end, std::string_view{});
    }
    // Replaces [start, end) with text that is already valid and normalized, without copying
    // large replacements.
    std::expected<TextChange, Error> replace(std::size_t start, std::size_t end,
                                             const Text& replacement);
    // Replaces the text in `range`, whose positions are clamped to the text.
    std::expected<TextChange, Error> replace(Range range, std::string_view replacement);

    // Replaces the whole text, for example to restore a snapshot.
    void reset(Text text) {
        text_ = std::move(text);
    }

private:
    Text text_;
};

} // namespace anvil::core
