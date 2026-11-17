#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace anvil::core {

// A selected range of text, as byte offsets on character boundaries. `anchor` is where the
// selection started and `active` is the end that moves, so the selection is reversed when the
// active end comes first. An empty selection is a caret.
struct Selection {
    std::size_t anchor = 0;
    std::size_t active = 0;

    static Selection caret(std::size_t offset) {
        return {offset, offset};
    }

    std::size_t start() const {
        return std::min(anchor, active);
    }
    std::size_t end() const {
        return std::max(anchor, active);
    }
    bool empty() const {
        return anchor == active;
    }
    bool reversed() const {
        return active < anchor;
    }

    bool operator==(const Selection&) const = default;
};

// One or more selections, one of them primary. The selections are ordered by position and do not
// overlap: overlapping selections, and a caret touching a selection, are merged.
class SelectionSet {
public:
    // A single caret at the start of the text.
    SelectionSet() : selections_{Selection{}} {}

    // Normalizes `selections`; `primary` indexes the given list. An empty list becomes a single
    // caret at offset zero.
    static SelectionSet from(std::vector<Selection> selections, std::size_t primary = 0);

    const std::vector<Selection>& all() const {
        return selections_;
    }
    const Selection& primary() const {
        return selections_[primary_];
    }
    std::size_t primary_index() const {
        return primary_;
    }

    bool operator==(const SelectionSet&) const = default;

private:
    std::vector<Selection> selections_;
    std::size_t primary_ = 0;
};

} // namespace anvil::core
