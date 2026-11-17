#include "anvil/core/editing.hpp"

#include "anvil/core/char_class.hpp"

#include <algorithm>

namespace anvil::core {
namespace {

using Signed = std::ptrdiff_t;

std::size_t previous_boundary(const Text& text, std::size_t offset) {
    do {
        --offset;
    } while (offset > 0 && !text.is_char_boundary(offset));
    return offset;
}

std::size_t next_boundary(const Text& text, std::size_t offset) {
    do {
        ++offset;
    } while (offset < text.size() && !text.is_char_boundary(offset));
    return offset;
}

// The code point that starts at `offset`, which must be a character boundary before the end.
char32_t codepoint_at(const Text& text, std::size_t offset) {
    return decode_code_point(text.slice(offset, 4), 0);
}

std::size_t word_right(const Text& text, std::size_t offset) {
    if (offset >= text.size()) {
        return text.size();
    }
    if (codepoint_at(text, offset) == '\n') {
        return offset + 1;
    }
    while (offset < text.size() && codepoint_at(text, offset) != '\n' &&
           classify(codepoint_at(text, offset)) == CharClass::space) {
        offset = next_boundary(text, offset);
    }
    if (offset >= text.size() || codepoint_at(text, offset) == '\n') {
        return offset;
    }
    const CharClass run = classify(codepoint_at(text, offset));
    while (offset < text.size() && classify(codepoint_at(text, offset)) == run &&
           codepoint_at(text, offset) != '\n') {
        offset = next_boundary(text, offset);
    }
    return offset;
}

std::size_t word_left(const Text& text, std::size_t offset) {
    if (offset == 0) {
        return 0;
    }
    if (codepoint_at(text, previous_boundary(text, offset)) == '\n') {
        return offset - 1;
    }
    while (offset > 0) {
        const std::size_t before = previous_boundary(text, offset);
        const char32_t c = codepoint_at(text, before);
        if (c == '\n' || classify(c) != CharClass::space) {
            break;
        }
        offset = before;
    }
    if (offset == 0) {
        return 0;
    }
    const std::size_t first = previous_boundary(text, offset);
    if (codepoint_at(text, first) == '\n') {
        return offset;
    }
    const CharClass run = classify(codepoint_at(text, first));
    while (offset > 0) {
        const std::size_t before = previous_boundary(text, offset);
        const char32_t c = codepoint_at(text, before);
        if (c == '\n' || classify(c) != run) {
            break;
        }
        offset = before;
    }
    return offset;
}

std::size_t move_vertically(const Text& text, std::size_t offset, int direction,
                            std::optional<std::size_t> goal_column) {
    const Position position = text.position_at(offset);
    if (direction < 0) {
        if (position.line == 0) {
            return 0;
        }
        return text.offset_at({position.line - 1, goal_column.value_or(position.column)});
    }
    if (position.line + 1 >= text.line_count()) {
        return text.size();
    }
    return text.offset_at({position.line + 1, goal_column.value_or(position.column)});
}

std::size_t line_indentation_length(const Text& text, std::size_t line, std::size_t limit_bytes) {
    const std::string content = text.line(line);
    std::size_t length = 0;
    while (length < content.size() && (content[length] == ' ' || content[length] == '\t')) {
        ++length;
    }
    return std::min(length, limit_bytes);
}

} // namespace

EditOutcome EditController::apply(std::vector<Edit> edits, std::vector<Selection> selections_after,
                                  EditKind kind, bool may_join) {
    const bool join = may_join && kind != EditKind::none && kind == last_kind_ &&
                      document_.version() == last_version_ &&
                      document_.selections() == last_selections_;
    const SelectionSet before = document_.selections();
    auto change = document_.apply_edits(
        edits, EditOptions{.selections_after = SelectionSet::from(std::move(selections_after),
                                                                  before.primary_index()),
                           .join_previous = join});
    if (!change) {
        return {};
    }
    last_kind_ = kind;
    last_version_ = document_.version();
    last_selections_ = document_.selections();
    goal_column_.reset();
    return {std::move(*change), !(document_.selections() == before)};
}

EditOutcome EditController::replace_selections(std::string_view text, EditKind kind,
                                               bool may_join) {
    const std::string normalized = normalize_line_endings(text);
    std::vector<Edit> edits;
    std::vector<Selection> after;
    Signed shift = 0;
    for (const Selection& selection : document_.selections().all()) {
        edits.push_back({selection.start(), selection.end(), normalized});
        const std::size_t caret =
            static_cast<std::size_t>(static_cast<Signed>(selection.start()) + shift) +
            normalized.size();
        after.push_back(Selection::caret(caret));
        shift += static_cast<Signed>(normalized.size()) -
                 static_cast<Signed>(selection.end() - selection.start());
    }
    return apply(std::move(edits), std::move(after), kind, may_join);
}

EditOutcome EditController::type(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const bool has_line_break = text.find_first_of("\r\n") != std::string_view::npos;
    return replace_selections(text, EditKind::typing, !has_line_break);
}

EditOutcome EditController::paste(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    return replace_selections(text, EditKind::none, false);
}

EditOutcome EditController::replace_ranges(std::vector<Edit> edits,
                                           std::optional<Selection> selection_after) {
    if (edits.empty()) {
        return {};
    }
    const SelectionSet before = document_.selections();
    EditOptions options;
    if (selection_after) {
        options.selections_after = SelectionSet::from({*selection_after});
    }
    auto change = document_.apply_edits(edits, std::move(options));
    if (!change) {
        return {};
    }
    last_kind_ = EditKind::none;
    goal_column_.reset();
    return {std::move(*change), !(document_.selections() == before)};
}

EditOutcome EditController::insert_newline() {
    const Text& text = document_.text();
    std::vector<Edit> edits;
    std::vector<Selection> after;
    Signed shift = 0;
    for (const Selection& selection : document_.selections().all()) {
        const Position position = text.position_at(selection.start());
        const std::size_t before_caret = selection.start() - text.line_start(position.line);
        const std::string indentation =
            text.line(position.line)
                .substr(0, line_indentation_length(text, position.line, before_caret));
        const std::string inserted = "\n" + indentation;
        edits.push_back({selection.start(), selection.end(), inserted});
        after.push_back(Selection::caret(
            static_cast<std::size_t>(static_cast<Signed>(selection.start()) + shift) +
            inserted.size()));
        shift += static_cast<Signed>(inserted.size()) -
                 static_cast<Signed>(selection.end() - selection.start());
    }
    return apply(std::move(edits), std::move(after), EditKind::none, false);
}

EditOutcome EditController::delete_with(DeleteMode mode) {
    const Text& text = document_.text();
    std::vector<Edit> edits;
    std::vector<Selection> after;
    Signed shift = 0;
    bool any_selection = false;
    for (const Selection& selection : document_.selections().all()) {
        std::size_t start = selection.start();
        std::size_t end = selection.end();
        if (selection.empty()) {
            if (mode == DeleteMode::backward && start > 0) {
                start = previous_boundary(text, start);
            } else if (mode == DeleteMode::forward && end < text.size()) {
                end = next_boundary(text, end);
            }
        } else {
            any_selection = true;
        }
        const std::size_t caret = static_cast<std::size_t>(static_cast<Signed>(start) + shift);
        after.push_back(Selection::caret(caret));
        if (start < end) {
            edits.push_back({start, end, {}});
            shift -= static_cast<Signed>(end - start);
        }
    }
    if (edits.empty()) {
        return {};
    }
    const EditKind kind = mode == DeleteMode::forward    ? EditKind::deleting_forward
                          : mode == DeleteMode::backward ? EditKind::deleting_backward
                                                         : EditKind::none;
    return apply(std::move(edits), std::move(after), kind, !any_selection);
}

EditOutcome EditController::delete_backward() {
    return delete_with(DeleteMode::backward);
}

EditOutcome EditController::delete_forward() {
    return delete_with(DeleteMode::forward);
}

EditOutcome EditController::delete_selection() {
    return delete_with(DeleteMode::selections_only);
}

EditOutcome
EditController::after_history(std::expected<std::optional<DocumentChange>, Error> result) {
    if (!result || !*result) {
        return {};
    }
    last_kind_ = EditKind::none;
    goal_column_.reset();
    return {std::move(**result), true};
}

EditOutcome EditController::undo() {
    const SelectionSet before = document_.selections();
    EditOutcome outcome = after_history(document_.undo());
    outcome.selections_changed = outcome.change && !(document_.selections() == before);
    return outcome;
}

EditOutcome EditController::redo() {
    const SelectionSet before = document_.selections();
    EditOutcome outcome = after_history(document_.redo());
    outcome.selections_changed = outcome.change && !(document_.selections() == before);
    return outcome;
}

EditOutcome EditController::adopt(SelectionSet selections) {
    goal_column_.reset();
    if (selections == document_.selections()) {
        return {};
    }
    if (!document_.set_selections(std::move(selections))) {
        return {};
    }
    return {std::nullopt, true};
}

EditOutcome EditController::move(Motion motion, bool extend) {
    const Text& text = document_.text();
    const auto& current = document_.selections().all();
    const bool vertical = motion == Motion::up || motion == Motion::down;
    std::optional<std::size_t> goal;
    if (vertical && current.size() == 1) {
        goal = goal_column_
                   ? goal_column_
                   : std::optional<std::size_t>(text.position_at(current[0].active).column);
    }

    std::vector<Selection> moved;
    for (const Selection& selection : current) {
        std::size_t target = selection.active;
        switch (motion) {
        case Motion::left:
            if (!extend && !selection.empty()) {
                moved.push_back(Selection::caret(selection.start()));
                continue;
            }
            target = selection.active > 0 ? previous_boundary(text, selection.active) : 0;
            break;
        case Motion::right:
            if (!extend && !selection.empty()) {
                moved.push_back(Selection::caret(selection.end()));
                continue;
            }
            target = selection.active < text.size() ? next_boundary(text, selection.active)
                                                    : text.size();
            break;
        case Motion::up:
            target = move_vertically(text, selection.active, -1, goal);
            break;
        case Motion::down:
            target = move_vertically(text, selection.active, 1, goal);
            break;
        case Motion::line_start:
            target = text.line_start(text.position_at(selection.active).line);
            break;
        case Motion::line_end:
            target = text.line_end(text.position_at(selection.active).line);
            break;
        case Motion::document_start:
            target = 0;
            break;
        case Motion::document_end:
            target = text.size();
            break;
        case Motion::word_left:
            target = word_left(text, selection.active);
            break;
        case Motion::word_right:
            target = word_right(text, selection.active);
            break;
        }
        moved.push_back(extend ? Selection{selection.anchor, target} : Selection::caret(target));
    }

    EditOutcome outcome =
        adopt(SelectionSet::from(std::move(moved), document_.selections().primary_index()));
    // Vertical movement remembers the column it started from; any other movement forgets it.
    goal_column_ = vertical ? goal : std::nullopt;
    return outcome;
}

EditOutcome EditController::select_all() {
    return adopt(SelectionSet::from({Selection{0, document_.text().size()}}));
}

EditOutcome EditController::select(Selection range) {
    const Text& text = document_.text();
    const auto boundary = [&text](std::size_t offset) {
        offset = std::min(offset, text.size());
        while (!text.is_char_boundary(offset)) {
            --offset;
        }
        return offset;
    };
    return adopt(SelectionSet::from({Selection{boundary(range.anchor), boundary(range.active)}}));
}

EditOutcome EditController::set_caret(std::size_t offset, bool extend) {
    const Text& text = document_.text();
    offset = std::min(offset, text.size());
    while (!text.is_char_boundary(offset)) {
        --offset;
    }
    if (extend) {
        std::vector<Selection> selections = document_.selections().all();
        const std::size_t primary = document_.selections().primary_index();
        selections[primary].active = offset;
        return adopt(SelectionSet::from(std::move(selections), primary));
    }
    return adopt(SelectionSet::from({Selection::caret(offset)}));
}

EditOutcome EditController::keep_primary_selection() {
    return adopt(SelectionSet::from({document_.selections().primary()}));
}

EditOutcome EditController::add_caret(std::size_t offset) {
    const Text& text = document_.text();
    offset = std::min(offset, text.size());
    while (!text.is_char_boundary(offset)) {
        --offset;
    }
    std::vector<Selection> selections = document_.selections().all();
    selections.push_back(Selection::caret(offset));
    const std::size_t added = selections.size() - 1;
    return adopt(SelectionSet::from(std::move(selections), added));
}

EditOutcome EditController::select_word_at(std::size_t offset) {
    const Text& text = document_.text();
    offset = std::min(offset, text.size());
    while (!text.is_char_boundary(offset)) {
        --offset;
    }
    if (text.empty()) {
        return {};
    }
    std::size_t at = offset;
    if (at == text.size() || codepoint_at(text, at) == '\n') {
        if (at == 0 || codepoint_at(text, previous_boundary(text, at)) == '\n') {
            return adopt(SelectionSet::from({Selection::caret(offset)}));
        }
        at = previous_boundary(text, at);
    }
    const CharClass run = classify(codepoint_at(text, at));
    std::size_t start = at;
    while (start > 0) {
        const std::size_t before = previous_boundary(text, start);
        const char32_t c = codepoint_at(text, before);
        if (c == '\n' || classify(c) != run) {
            break;
        }
        start = before;
    }
    std::size_t end = next_boundary(text, at);
    while (end < text.size()) {
        const char32_t c = codepoint_at(text, end);
        if (c == '\n' || classify(c) != run) {
            break;
        }
        end = next_boundary(text, end);
    }
    return adopt(SelectionSet::from({Selection{start, end}}));
}

EditOutcome EditController::select_line_at(std::size_t offset) {
    const Text& text = document_.text();
    const std::size_t line = text.position_at(offset).line;
    const std::size_t start = text.line_start(line);
    const std::size_t end = line + 1 < text.line_count() ? text.line_start(line + 1) : text.size();
    return adopt(SelectionSet::from({Selection{start, end}}));
}

std::string EditController::selected_text() const {
    std::string result;
    bool first = true;
    for (const Selection& selection : document_.selections().all()) {
        if (selection.empty()) {
            continue;
        }
        if (!first) {
            result += '\n';
        }
        first = false;
        result += document_.text().slice(selection.start(), selection.end() - selection.start());
    }
    return result;
}

} // namespace anvil::core
