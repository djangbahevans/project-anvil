#include "cursor_commands.hpp"

#include "anvil/core/editing.hpp"

#include <utility>

namespace anvil::core::cursor_commands {
namespace {

struct PlacedEdit {
    Edit edit;
    std::size_t command = 0;
    std::size_t op = 0;
};

class Placement {
public:
    explicit Placement(std::vector<PlacedEdit> edits) : edits_(std::move(edits)) {
        std::ranges::stable_sort(edits_, [](const PlacedEdit& a, const PlacedEdit& b) {
            return std::pair(a.edit.start, a.edit.end) < std::pair(b.edit.start, b.edit.end);
        });
    }

    const std::vector<PlacedEdit>& edits() const {
        return edits_;
    }

    // Where a position of the old text is in the new one, as VS Code's text model moves a tracked
    // range: the edits are made from the last one to the first, and a position inside a replaced
    // range stays as far into the new text as it was, or goes to its end when that is shorter. At
    // an insertion, and at the end of a replaced range, the position moves after the new text when
    // `moves`.
    std::size_t map(std::size_t offset, bool moves) const {
        std::size_t position = offset;
        for (auto edit = edits_.rbegin(); edit != edits_.rend(); ++edit) {
            const Edit& e = edit->edit;
            if (e.start == e.end) {
                if (position > e.start || (position == e.start && moves)) {
                    position += e.text.size();
                }
            } else if (position > e.end || (position == e.end && moves)) {
                position = position - (e.end - e.start) + e.text.size();
            } else if (position > e.start) {
                position = std::min(position, e.start + e.text.size());
            }
        }
        return position;
    }

    // Where the text that the edit `index` (in the order of edits()) puts in is in the new text.
    std::size_t placed_start(std::size_t index) const {
        Signed delta = 0;
        for (std::size_t i = 0; i < index; ++i) {
            const Edit& e = edits_[i].edit;
            delta += static_cast<Signed>(e.text.size()) - static_cast<Signed>(e.end - e.start);
        }
        return static_cast<std::size_t>(static_cast<Signed>(edits_[index].edit.start) + delta);
    }

private:
    std::vector<PlacedEdit> edits_;
};

// The cursors whose edits overlap those of a cursor with a lower number lose, as in VS Code: the
// edits of a loser are not made and its selection is dropped.
std::vector<bool> losers_of(const std::vector<Command>& commands) {
    struct Raw {
        std::size_t major;
        LineRange range;
    };
    std::vector<Raw> ops;
    for (std::size_t c = 0; c < commands.size(); ++c) {
        for (const Op& op : commands[c].ops) {
            if (!(op.range.empty() && op.text.empty())) {
                ops.push_back({c, op.range});
            }
        }
    }
    // The last range first: by end, then by start, descending.
    std::ranges::stable_sort(ops, [](const Raw& a, const Raw& b) {
        return std::pair(b.range.end, b.range.start) < std::pair(a.range.end, a.range.start);
    });
    std::vector<bool> loser(commands.size(), false);
    for (Signed i = 1; i < static_cast<Signed>(ops.size()); ++i) {
        const Raw previous = ops[static_cast<std::size_t>(i) - 1];
        const Raw current = ops[static_cast<std::size_t>(i)];
        if (previous.range.start < current.range.end) {
            const std::size_t major =
                previous.major > current.major ? previous.major : current.major;
            loser[major] = true;
            for (Signed j = 0; j < static_cast<Signed>(ops.size()); ++j) {
                if (ops[static_cast<std::size_t>(j)].major == major) {
                    ops.erase(ops.begin() + j);
                    if (j < i) {
                        --i;
                    }
                    --j;
                }
            }
            if (i > 0) {
                --i;
            }
        }
    }
    return loser;
}

// VS Code's merging of cursors that overlap, or that touch when one of them is a caret: the union
// of the two takes the place of the one with the lower number, in the direction of that one, or of
// the other one when it is the last cursor that was added.
void merge_cursors(std::vector<Selection>& cursors) {
    struct Sorted {
        std::size_t index;
        Selection selection;
    };
    std::vector<Sorted> sorted;
    for (std::size_t i = 0; i < cursors.size(); ++i) {
        sorted.push_back({i, cursors[i]});
    }
    std::ranges::stable_sort(sorted, [](const Sorted& a, const Sorted& b) {
        return std::pair(a.selection.start(), a.selection.end()) <
               std::pair(b.selection.start(), b.selection.end());
    });
    std::size_t last_added = cursors.empty() ? 0 : cursors.size() - 1;
    for (std::size_t at = 0; at + 1 < sorted.size(); ++at) {
        const Sorted current = sorted[at];
        const Sorted next = sorted[at + 1];
        const bool merge = current.selection.empty() || next.selection.empty()
                               ? next.selection.start() <= current.selection.end()
                               : next.selection.start() < current.selection.end();
        if (!merge) {
            continue;
        }
        const bool current_wins = current.index < next.index;
        const std::size_t winner_at = current_wins ? at : at + 1;
        const std::size_t looser_at = current_wins ? at + 1 : at;
        const std::size_t winner_index = sorted[winner_at].index;
        const std::size_t looser_index = sorted[looser_at].index;
        const Selection looser = sorted[looser_at].selection;
        const Selection winner = sorted[winner_at].selection;
        if (!(looser == winner)) {
            const std::size_t start = std::min(looser.start(), winner.start());
            const std::size_t end = std::max(looser.end(), winner.end());
            bool forward = !winner.reversed();
            if (looser_index == last_added) {
                forward = !looser.reversed();
                last_added = winner_index;
            }
            const Selection merged = forward ? Selection{start, end} : Selection{end, start};
            sorted[winner_at].selection = merged;
            cursors[winner_index] = merged;
        }
        for (Sorted& other : sorted) {
            if (other.index > looser_index) {
                --other.index;
            }
        }
        cursors.erase(cursors.begin() + static_cast<Signed>(looser_index));
        sorted.erase(sorted.begin() + static_cast<Signed>(looser_at));
        if (last_added >= looser_index && last_added > 0) {
            --last_added;
        }
        --at;
    }
}

} // namespace

// The edits of all the commands, as one batch, and the selection each command leaves.
Finished finish(const Model& model, const std::vector<Command>& commands,
                const std::vector<Sel>& cursors_before) {
    Finished finished;
    const bool any = std::ranges::any_of(commands, [](const Command& command) {
        return std::ranges::any_of(
            command.ops, [](const Op& op) { return !(op.range.empty() && op.text.empty()); });
    });
    if (!any) {
        finished.cancelled = true;
        return finished;
    }
    const std::vector<bool> loser = losers_of(commands);

    std::vector<PlacedEdit> placed;
    for (std::size_t c = 0; c < commands.size(); ++c) {
        if (loser[c]) {
            continue;
        }
        for (std::size_t k = 0; k < commands[c].ops.size(); ++k) {
            const Op& op = commands[c].ops[k];
            if (op.applies && !(op.range.empty() && op.text.empty())) {
                placed.push_back(
                    {Edit{model.offset(op.range.start), model.offset(op.range.end), op.text}, c,
                     k});
            }
        }
    }
    const Placement placement(std::move(placed));
    const auto edit_index = [&](std::size_t command, std::size_t op) {
        const auto& edits = placement.edits();
        for (std::size_t i = 0; i < edits.size(); ++i) {
            if (edits[i].command == command && edits[i].op == op) {
                return i;
            }
        }
        return edits.size();
    };

    for (std::size_t c = 0; c < commands.size(); ++c) {
        const Command& command = commands[c];
        if (loser[c]) {
            continue;
        }
        const bool has_ops = std::ranges::any_of(
            command.ops, [](const Op& op) { return !(op.range.empty() && op.text.empty()); });
        if (!has_ops) {
            // A command that edits nothing leaves the cursor with the same number as the command
            // where it was, at the same line and column, also when other commands changed the text
            // before it on the line or moved the line.
            const auto retained = [&](Pos position) {
                const std::size_t line_start =
                    placement.map(model.line_start_offset(position.line), false);
                const std::size_t line_end =
                    placement.map(model.line_end_offset(position.line), true);
                return std::min(line_start + position.column - 1, line_end);
            };
            finished.selections.push_back(
                Selection{retained(cursors_before[c].anchor), retained(cursors_before[c].active)});
            continue;
        }
        if (command.inverse) {
            const std::size_t first = edit_index(c, 0);
            const std::size_t first_end =
                placement.placed_start(first) + placement.edits()[first].edit.text.size();
            if (command.ops.size() == 2) {
                const std::size_t second = edit_index(c, 1);
                finished.selections.push_back(Selection{first_end, placement.placed_start(second)});
            } else {
                const std::size_t caret =
                    first_end - (command.used_end_token ? command.used_end_token_size + 1 : 0);
                finished.selections.push_back(Selection::caret(caret));
            }
            continue;
        }

        const Sel& tracked = command.tracked;
        bool start_moves = true;
        bool end_moves = false;
        if (tracked.empty()) {
            start_moves = end_moves =
                tracked.start().column != model.max_column(tracked.start().line);
        }
        std::size_t start = placement.map(model.offset(tracked.start()), start_moves);
        std::size_t end = std::max(start, placement.map(model.offset(tracked.end()), end_moves));
        if (command.keep_start_column) {
            const std::size_t line_start =
                placement.map(model.line_start_offset(tracked.start().line), false);
            start = std::min(start, line_start + tracked.start().column - 1);
        }
        // A range that the edits emptied has no direction left.
        const bool reversed = tracked.reversed() && start != end;
        if (command.move_end_down) {
            end = placement.map(model.line_start_offset(command.selection.end().line), false);
        }
        // A column that the shift pushes past the end of its line is the end of the line.
        const auto shifted = [&](std::size_t offset, std::size_t line) {
            return std::min(offset + command.delta_column,
                            placement.map(model.line_end_offset(line), true));
        };
        const std::size_t start_line = tracked.start().line;
        const std::size_t end_line =
            command.move_end_down ? command.selection.end().line : tracked.end().line;
        const std::size_t anchor = reversed ? shifted(end, end_line) : shifted(start, start_line);
        const std::size_t active = reversed ? shifted(start, start_line) : shifted(end, end_line);
        finished.selections.push_back(Selection{anchor, active});
    }
    for (const PlacedEdit& edit : placement.edits()) {
        finished.edits.push_back(edit.edit);
    }
    merge_cursors(finished.selections);
    return finished;
}

std::vector<Sel> cursors_of(const Model& model, const SelectionSet& selections) {
    const std::vector<Selection>& all = selections.all();
    const std::size_t primary = selections.primary_index();
    std::vector<Sel> cursors{model.selection(all[primary])};
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (i != primary) {
            cursors.push_back(model.selection(all[i]));
        }
    }
    return cursors;
}

} // namespace anvil::core::cursor_commands

namespace anvil::core {

EditOutcome EditController::run_commands(const cursor_commands::Model& model,
                                         const std::vector<cursor_commands::Command>& commands,
                                         const std::vector<cursor_commands::Sel>& cursors) {
    cursor_commands::Finished finished = cursor_commands::finish(model, commands, cursors);
    if (finished.cancelled) {
        return {};
    }
    return apply_line_edit(std::move(finished.edits), std::move(finished.selections), 0);
}

} // namespace anvil::core
