#pragma once

#include "anvil/core/document.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace anvil::core {

enum class Motion {
    left,
    right,
    up,
    down,
    // Column 1 of the line.
    line_start,
    // VS Code's Home: the first character that is not a space or a tab, or column 1 when the caret
    // is already there or the line has nothing else.
    home,
    line_end,
    document_start,
    document_end,
    word_left,
    word_right,
};

// What an editing command did.
struct EditOutcome {
    // Set when the text changed, including by undo and redo.
    std::optional<DocumentChange> change;
    // True when the selections differ from before the command.
    bool selections_changed = false;

    bool changed_anything() const {
        return change.has_value() || selections_changed;
    }
};

// The word that a caret at `offset` is in or next to: the run of word characters (see classify)
// around it on its line. A caret between two words belongs to the one after it. Nothing when the
// characters on both sides of the caret are not word characters. An offset inside a character moves
// back to its start, and one past the end is the end.
std::optional<Selection> word_at(const Text& text, std::size_t offset);

// What copy puts on the clipboard.
struct CopiedText {
    std::string text;
    // True when `text` stands for the line of a lone caret: it is that line followed by a line
    // break, and pasting it into a lone caret inserts it above that caret's line.
    bool whole_line = false;
};

// The editing commands a user triggers with the keyboard and mouse, applied to a document: typing,
// deleting, moving and selecting, and undo. They work on every selection at once, so multiple
// cursors need no special handling.
//
// Characters are Unicode code points; a base character with combining marks counts as several.
//
// The controller keeps the state that is not part of the document: the column that vertical
// movement tries to return to, and whether the last edit may be joined by the next one into a
// single undo step. The document must outlive the controller.
class EditController {
public:
    explicit EditController(Document& document) : document_(document) {}

    // --- editing ---

    // Replaces every selection with `text`, leaving a caret after it. Typing that follows typing
    // without other changes in between undoes as one step; text with a line break never joins.
    EditOutcome type(std::string_view text);
    // Like type(), but never joins an undo step. With `whole_line`, text that is exactly one line
    // ending in a line break (copied_text() with whole_line set) is inserted at the start of the
    // line of each caret, which moves down with the text, provided every selection is an empty
    // caret. Anything else is pasted like ordinary text.
    EditOutcome paste(std::string_view text, bool whole_line = false);
    // Replaces the ranges with their texts as one undo step that never joins typing. The edits are
    // in coordinates of the current text, and must be valid as for Document::apply_edits. The
    // selections move with the edits unless `selection_after` is given, which then becomes the only
    // selection.
    EditOutcome replace_ranges(std::vector<Edit> edits,
                               std::optional<Selection> selection_after = std::nullopt);
    // Replaces every selection with a line break followed by the indentation of its line.
    EditOutcome insert_newline();
    // Deletes the selections, or the character before each caret.
    EditOutcome delete_backward();
    // Deletes the selections, or the character after each caret.
    EditOutcome delete_forward();
    // Deletes the selections, for cut.
    EditOutcome delete_selection();
    // Deletes the line of a lone caret with its line break, for cutting a line: the break before
    // it for the last line of several, so that no empty line is left behind. The caret goes to
    // where the line was. Does nothing for a selection or several carets.
    EditOutcome delete_caret_line();

    EditOutcome undo();
    EditOutcome redo();

    // --- selecting ---

    // Moves every active end; the anchors move too unless `extend` is set. Without `extend`, left
    // and right collapse a selection to its start or end instead of moving.
    EditOutcome move(Motion motion, bool extend = false);
    EditOutcome select_all();
    // Makes `range` the only selection, with its active end at `range.active`. Offsets move back
    // to a character boundary.
    EditOutcome select(Selection range);
    // Places a single caret, or with `extend` moves the active end of the primary selection.
    EditOutcome set_caret(std::size_t offset, bool extend = false);
    // Drops every selection but the primary one, which keeps its extent.
    EditOutcome keep_primary_selection();
    // Adds a caret without removing the others.
    EditOutcome add_caret(std::size_t offset);
    // Selects the word at `offset`, or the run of punctuation or spaces there.
    EditOutcome select_word_at(std::size_t offset);
    // Selects the whole line containing `offset`, including its line break.
    EditOutcome select_line_at(std::size_t offset);

    // The text of the selections, joined by line breaks when there are several; empty when only
    // carets are present.
    std::string selected_text() const;
    // The text for the clipboard: the selected text, or, for a lone caret, its line and a line
    // break (also on a last line that has none). Empty for several carets.
    CopiedText copied_text() const;

private:
    enum class EditKind {
        none,
        typing,
        deleting_backward,
        deleting_forward,
    };

    EditOutcome apply(std::vector<Edit> edits, std::vector<Selection> selections_after,
                      EditKind kind, bool may_join);
    EditOutcome replace_selections(std::string_view text, EditKind kind, bool may_join);
    enum class DeleteMode {
        backward,
        forward,
        selections_only,
    };
    EditOutcome delete_with(DeleteMode mode);
    EditOutcome adopt(SelectionSet selections);
    EditOutcome after_history(std::expected<std::optional<DocumentChange>, Error> result);

    Document& document_;
    std::optional<std::size_t> goal_column_;
    EditKind last_kind_ = EditKind::none;
    // The document version and selections right after the last joinable edit.
    std::uint64_t last_version_ = 0;
    SelectionSet last_selections_;
};

} // namespace anvil::core
