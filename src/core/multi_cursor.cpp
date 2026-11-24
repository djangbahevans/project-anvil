#include "anvil/core/brackets.hpp"
#include "anvil/core/editing.hpp"
#include "anvil/core/smart_select.hpp"

#include <algorithm>

namespace anvil::core {
namespace {

struct Moved {
    std::size_t offset = 0;
    std::size_t leftover = 0;
};

// Where an end of a selection goes on the line above or below its own. It wants the column it has
// plus what a short line took away from it before; on a shorter line it stops at the end of the
// line, and what is missing is what it will want again.
Moved translate(const Text& text, std::size_t offset, std::size_t leftover, bool down) {
    const Position position = text.position_at(offset);
    const std::size_t wanted = position.column + leftover;
    const bool beyond = down ? position.line + 1 >= text.line_count() : position.line == 0;
    if (beyond) {
        // The edge line has nothing to move to; the end stays, and a caret at the very edge of the
        // text forgets what it wanted.
        const bool at_edge = down ? offset == text.size() : offset == 0;
        return {offset, at_edge ? 0 : leftover};
    }
    const std::size_t line = down ? position.line + 1 : position.line - 1;
    const std::size_t target = text.offset_at({line, wanted});
    return {target, wanted - text.position_at(target).column};
}

} // namespace

EditOutcome EditController::add_cursors_vertically(bool down) {
    const Text& text = document_.text();
    const SelectionSet before = document_.selections();
    const std::vector<Selection>& selections = before.all();
    const bool known = leftovers_for_ == before && leftovers_.size() == selections.size();

    struct Entry {
        Selection selection;
        Leftover leftover;
    };
    std::vector<Entry> entries;
    for (std::size_t i = 0; i < selections.size(); ++i) {
        const Leftover current = known ? leftovers_[i] : Leftover{};
        entries.push_back({selections[i], current});
        const Moved anchor = translate(text, selections[i].anchor, current.anchor, down);
        const Moved active = translate(text, selections[i].active, current.active, down);
        entries.push_back({{anchor.offset, active.offset}, {anchor.leftover, active.leftover}});
    }
    std::vector<Selection> all;
    for (const Entry& entry : entries) {
        all.push_back(entry.selection);
    }
    SelectionSet after = SelectionSet::from(std::move(all), 2 * before.primary_index());

    // Each selection that remains keeps the leftover of the first entry that made it.
    std::vector<Leftover> kept;
    for (const Selection& selection : after.all()) {
        const auto found = std::ranges::find_if(
            entries, [&](const Entry& entry) { return entry.selection == selection; });
        kept.push_back(found == entries.end() ? Leftover{} : found->leftover);
    }
    EditOutcome outcome = adopt(after);
    leftovers_for_ = std::move(after);
    leftovers_ = std::move(kept);
    return outcome;
}

EditOutcome EditController::add_cursors_to_line_ends() {
    const Text& text = document_.text();
    std::vector<Selection> carets;
    for (const Selection& selection : document_.selections().all()) {
        if (selection.empty()) {
            continue;
        }
        const std::size_t first = text.position_at(selection.start()).line;
        const std::size_t last = text.position_at(selection.end()).line;
        for (std::size_t line = first; line < last; ++line) {
            carets.push_back(Selection::caret(text.line_end(line)));
        }
        if (selection.end() != text.line_start(last)) {
            carets.push_back(Selection::caret(selection.end()));
        }
    }
    if (carets.empty()) {
        return {};
    }
    return adopt(SelectionSet::from(std::move(carets), 0));
}

EditOutcome EditController::jump_to_bracket() {
    const std::string text = document_.text().to_string();
    std::vector<Selection> carets;
    for (const Selection& selection : document_.selections().all()) {
        carets.push_back(Selection::caret(anvil::core::jump_to_bracket(text, selection.start())));
    }
    return adopt(SelectionSet::from(std::move(carets), document_.selections().primary_index()));
}

EditOutcome EditController::expand_selection() {
    return step_selection(true);
}

EditOutcome EditController::shrink_selection() {
    return step_selection(false);
}

EditOutcome EditController::step_selection(bool expand) {
    const Text& text = document_.text();
    if (!smart_select_ || smart_select_->version != document_.version() ||
        !(smart_select_->selections == document_.selections())) {
        SmartSelectState state;
        state.primary = document_.selections().primary_index();
        for (const Selection& selection : document_.selections().all()) {
            SmartSelectStep step;
            step.ranges.push_back({selection.start(), selection.end()});
            for (const Selection& range : selection_ranges(text, selection.active)) {
                if (range.start() <= selection.start() && selection.end() <= range.end()) {
                    step.ranges.push_back(range);
                }
            }
            state.steps.push_back(std::move(step));
        }
        smart_select_ = std::move(state);
    }

    std::vector<Selection> stepped;
    for (SmartSelectStep& step : smart_select_->steps) {
        std::size_t index = step.index;
        while (expand ? index + 1 < step.ranges.size() : index > 0) {
            const std::size_t next = expand ? index + 1 : index - 1;
            const bool same = step.ranges[next] == step.ranges[index];
            index = next;
            if (!same) {
                break;
            }
        }
        step.index = index;
        stepped.push_back(step.ranges[index]);
    }
    const EditOutcome outcome =
        adopt(SelectionSet::from(std::move(stepped), smart_select_->primary));
    smart_select_->version = document_.version();
    smart_select_->selections = document_.selections();
    return outcome;
}

} // namespace anvil::core
