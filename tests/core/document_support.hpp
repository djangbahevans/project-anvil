#pragma once

#include "anvil/core/document.hpp"

#include <gtest/gtest.h>

namespace anvil::core::testing {

// Applies `changes` one after another to `before`, checking that each is described correctly
// against the text it applies to, and returns the result.
inline Text replay_changes(const Text& before, const std::vector<TextChange>& changes) {
    TextBuffer buffer(before);
    for (const TextChange& change : changes) {
        const Text& current = buffer.snapshot();
        EXPECT_EQ(change.range.start, current.position_at(change.offset));
        EXPECT_EQ(change.range.end, current.position_at(change.offset + change.removed_bytes));
        EXPECT_EQ(change.removed_utf16, current.utf16_offset(change.offset + change.removed_bytes) -
                                            current.utf16_offset(change.offset));
        auto applied =
            buffer.replace(change.offset, change.offset + change.removed_bytes, change.text);
        EXPECT_TRUE(applied.has_value());
    }
    return buffer.snapshot();
}

} // namespace anvil::core::testing
