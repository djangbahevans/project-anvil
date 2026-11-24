#include "anvil/core/editing.hpp"

#include <algorithm>

namespace anvil::core {
namespace {

// The lines a selection works on: those it touches, except that a selection of several lines that
// ends at the start of a line does not touch that line.
struct Block {
    std::size_t first = 0;
    std::size_t last = 0;
    // The line of the end of the selection, whether or not it is touched.
    std::size_t end_line = 0;
    // The offsets that move with the block: its lines, and the start of the line after it when the
    // selection ends there.
    std::size_t lo = 0;
    std::size_t hi = 0;
};

Block block_of(const Text& text, const Selection& selection) {
    Block block;
    block.first = text.position_at(selection.start()).line;
    block.end_line = text.position_at(selection.end()).line;
    block.last = block.end_line;
    if (block.first < block.end_line && selection.end() == text.line_start(block.end_line)) {
        --block.last;
    }
    block.lo = text.line_start(block.first);
    block.hi = block.last == block.end_line ? text.line_end(block.last) : selection.end();
    return block;
}

// The text of the lines first to last, without the line break after the last.
std::string lines_text(const Text& text, std::size_t first, std::size_t last) {
    return text.slice(text.line_start(first), text.line_end(last) - text.line_start(first));
}

} // namespace

EditOutcome EditController::apply_line_edit(std::vector<Edit> edits,
                                            std::vector<Selection> selections,
                                            std::size_t primary) {
    SelectionSet after = SelectionSet::from(std::move(selections), primary);
    if (edits.empty()) {
        return adopt(std::move(after));
    }
    const SelectionSet before = document_.selections();
    auto change = document_.apply_edits(edits, EditOptions{.selections_after = std::move(after)});
    if (!change) {
        return {};
    }
    last_kind_ = EditKind::none;
    goal_column_.reset();
    return {std::move(*change), !(document_.selections() == before)};
}

EditOutcome EditController::move_lines(bool down) {
    const Text& text = document_.text();
    const std::vector<Selection>& selections = document_.selections().all();
    const std::size_t count = selections.size();
    const std::size_t line_count = text.line_count();

    struct Move {
        Block block;
        bool moves = false;
        // The lines that are swapped, which no other selection may swap.
        std::size_t region_first = 0;
        std::size_t region_last = 0;
        bool dropped = false;
    };
    std::vector<Move> moves(count);
    for (std::size_t i = 0; i < count; ++i) {
        Move& move = moves[i];
        move.block = block_of(text, selections[i]);
        move.moves = down ? move.block.end_line + 1 < line_count : move.block.first > 0;
        move.region_first = down ? move.block.first : move.block.first - (move.moves ? 1 : 0);
        move.region_last = down ? move.block.last + 1 : move.block.last;
    }
    for (std::size_t i = 0; i < count; ++i) {
        for (std::size_t j = i + 1; j < count; ++j) {
            if (moves[i].moves && moves[j].moves && moves[i].region_first <= moves[j].region_last &&
                moves[j].region_first <= moves[i].region_last) {
                moves[j].dropped = true;
            }
        }
    }

    struct Replaced {
        std::size_t start;
        std::size_t end;
    };
    std::vector<Edit> edits;
    std::vector<Replaced> replaced;
    std::vector<std::ptrdiff_t> shifts(count, 0);
    for (std::size_t i = 0; i < count; ++i) {
        const Move& move = moves[i];
        if (!move.moves || move.dropped) {
            continue;
        }
        const Block& block = move.block;
        const std::size_t other = down ? block.last + 1 : block.first - 1;
        const std::string other_line = lines_text(text, other, other);
        const std::string block_text = lines_text(text, block.first, block.last);
        const std::size_t start = text.line_start(move.region_first);
        const std::size_t end = text.line_end(move.region_last);
        const std::string swapped =
            down ? other_line + "\n" + block_text : block_text + "\n" + other_line;
        shifts[i] = static_cast<std::ptrdiff_t>(other_line.size() + 1) * (down ? 1 : -1);
        if (text.slice(start, end - start) != swapped) {
            edits.push_back({start, end, swapped});
        }
        replaced.push_back({start, end});
    }

    std::vector<Selection> after;
    std::size_t primary = 0;
    const auto shifted = [](std::size_t offset, std::ptrdiff_t shift) {
        return static_cast<std::size_t>(static_cast<std::ptrdiff_t>(offset) + shift);
    };
    for (std::size_t i = 0; i < count; ++i) {
        if (i == document_.selections().primary_index()) {
            primary = after.size();
        }
        if (moves[i].dropped) {
            continue;
        }
        Selection selection = selections[i];
        if (moves[i].moves) {
            selection = {shifted(selection.anchor, shifts[i]),
                         shifted(selection.active, shifts[i])};
        } else {
            // A selection that stays but sits in lines that another selection swaps goes to the
            // start of what was swapped.
            for (const Replaced& region : replaced) {
                for (std::size_t* offset : {&selection.anchor, &selection.active}) {
                    if (region.start < *offset && *offset < region.end) {
                        *offset = region.start;
                    }
                }
            }
        }
        after.push_back(selection);
    }
    primary = std::min(primary, after.size() - 1);
    return apply_line_edit(std::move(edits), std::move(after), primary);
}

EditOutcome EditController::copy_lines(bool down) {
    const Text& text = document_.text();
    const std::vector<Selection>& selections = document_.selections().all();
    const std::size_t count = selections.size();

    std::vector<Block> blocks;
    for (const Selection& selection : selections) {
        blocks.push_back(block_of(text, selection));
    }
    // Selections that touch the same line copy it once, for the first of them.
    std::vector<bool> copies(count, true);
    std::size_t copying = 0;
    for (std::size_t i = 1; i < count; ++i) {
        if (blocks[copying].end_line == blocks[i].first) {
            copies[i] = false;
        } else {
            copying = i;
        }
    }

    struct Insertion {
        std::size_t at;
        std::size_t length;
        std::size_t lo;
        std::size_t hi;
    };
    std::vector<Edit> edits;
    std::vector<Insertion> insertions;
    for (std::size_t i = 0; i < count; ++i) {
        if (!copies[i]) {
            continue;
        }
        const Block& block = blocks[i];
        const std::string source = lines_text(text, block.first, block.last);
        const std::size_t at = down ? block.lo : text.line_end(block.last);
        edits.push_back({at, at, down ? source + "\n" : "\n" + source});
        insertions.push_back({at, source.size() + 1, block.lo, block.hi});
    }

    // Offsets in the lines that were copied stay with the original text: they move with the
    // copy that goes above when copying down, and stay when it goes below. The others move with
    // the text before them.
    const auto mapped = [&](std::size_t offset) {
        std::size_t result = offset;
        for (const Insertion& insertion : insertions) {
            const bool owned = insertion.lo <= offset && offset <= insertion.hi;
            if (owned ? down : insertion.at < offset) {
                result += insertion.length;
            }
        }
        return result;
    };
    std::vector<Selection> after;
    for (const Selection& selection : selections) {
        after.push_back({mapped(selection.anchor), mapped(selection.active)});
    }
    return apply_line_edit(std::move(edits), std::move(after),
                           document_.selections().primary_index());
}

EditOutcome EditController::duplicate_selection() {
    const Text& text = document_.text();
    const std::vector<Selection>& selections = document_.selections().all();
    const std::size_t count = selections.size();

    struct Insertion {
        std::size_t at;
        std::size_t length;
        // For a line that a caret copies: the offsets of the line, which move with it.
        std::size_t lo;
        std::size_t hi;
        bool line;
    };
    std::vector<Edit> edits;
    std::vector<Insertion> insertions;
    for (const Selection& selection : selections) {
        if (selection.empty()) {
            const Block block = block_of(text, selection);
            const std::string source = lines_text(text, block.first, block.last);
            edits.push_back({block.lo, block.lo, source + "\n"});
            insertions.push_back({block.lo, source.size() + 1, block.lo, block.hi, true});
        } else {
            const std::string copy =
                text.slice(selection.start(), selection.end() - selection.start());
            edits.push_back({selection.end(), selection.end(), copy});
            insertions.push_back({selection.end(), copy.size(), 0, 0, false});
        }
    }

    // An offset moves with the text inserted before it, and with the line that a caret on it copies
    // above it.
    const auto mapped = [&](std::size_t offset) {
        std::size_t result = offset;
        for (const Insertion& insertion : insertions) {
            const bool owned = insertion.line && insertion.lo <= offset && offset <= insertion.hi;
            if (owned || insertion.at < offset) {
                result += insertion.length;
            }
        }
        return result;
    };
    std::vector<Selection> after;
    for (std::size_t i = 0; i < count; ++i) {
        const Selection& selection = selections[i];
        if (selection.empty()) {
            after.push_back(Selection::caret(mapped(selection.active)));
        } else {
            // The copy is what is selected afterwards: it follows the insertions before it, and
            // those at its place that come first in the order of the selections.
            std::size_t start = insertions[i].at;
            for (std::size_t j = 0; j < count; ++j) {
                if (insertions[j].at < insertions[i].at ||
                    (insertions[j].at == insertions[i].at && j < i)) {
                    start += insertions[j].length;
                }
            }
            after.push_back({start, start + insertions[i].length});
        }
    }
    return apply_line_edit(std::move(edits), std::move(after),
                           document_.selections().primary_index());
}

} // namespace anvil::core
