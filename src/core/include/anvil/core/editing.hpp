#pragma once

#include "anvil/core/document.hpp"
#include "anvil/core/language_comments.hpp"
#include "anvil/core/search.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

// Which of VS Code's line comment commands runs: Toggle Line Comment, Add Line Comment or Remove
// Line Comment.
enum class LineCommentType { toggle, add, remove };

// What the comment commands need to know besides the text and the selections.
struct CommentOptions {
    // The tokens of the language of the document.
    LanguageComments tokens;
    // `editor.tabSize`: the width of a tab, which aligns the comment tokens of indented lines.
    std::size_t indent_size = 4;
    // `editor.comments.insertSpace`: a space follows a line token and surrounds a block comment.
    bool insert_space = true;
    // `editor.comments.ignoreEmptyLines`: a line with nothing on it gets no line token, unless the
    // lines of a toggle are all like that.
    bool ignore_empty_lines = true;
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

// What "add selection to next find match" searches for, as VS Code starts it from the primary
// selection.
struct OccurrenceQuery {
    // A literal query for the text of the selection, or for the word under a lone caret.
    Query query;
    // The word under a caret, which the first command selects instead of looking for a match.
    std::optional<Selection> first;
};

// What copy puts on the clipboard.
struct CopiedText {
    // The pieces joined by line breaks.
    std::string text;
    // True when `text` stands for the line of a lone caret: it is that line followed by a line
    // break, and pasting it into a lone caret inserts it above that caret's line.
    bool whole_line = false;
    // The separate pieces when there are several, one per selection, and one per line for the
    // carets: pasting them into as many cursors gives each its own piece. Empty otherwise.
    std::vector<std::string> pieces;
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
    //
    // With several cursors, `pieces` (copied_text().pieces of the copy that `text` came from) are
    // given to the cursors one each when there are as many as cursors. Otherwise a `text` that
    // is not a whole line, and has as many lines as there are cursors, ignoring one line break at
    // its end, goes one line to each cursor. Everything else is pasted at every cursor. The
    // selections are replaced and the carets inserted at, in document order, as one undo step.
    EditOutcome paste(std::string_view text, bool whole_line = false,
                      const std::vector<std::string>& pieces = {});
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
    // Deletes what copied_text() copies, for cut: the selections, and the lines of the carets with
    // their line breaks (the break before the last line of several, so that no empty line is left
    // behind, unless the line before is cut as well). Carets on the same line cut it once. Ranges
    // that overlap are deleted as one. Every cursor ends as a caret where its text was.
    EditOutcome delete_cut();

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
    // Makes `ranges` the selections, with the one at index `primary` primary. Offsets move back to
    // a character boundary, and ranges that overlap are merged.
    EditOutcome select_ranges(std::vector<Selection> ranges, std::size_t primary = 0);
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

    // --- cursors ---

    // Adds, for every selection, a copy of it one line up or down: each end of the copy goes to
    // the column that end wants on the neighboring line, which is the column it had, or the
    // column it would have had on a longer line (the width that a short line took away is
    // remembered while the cursors are added again and again). At the first line (up) and the
    // last line (down) the copy is the selection itself, so nothing is added.
    EditOutcome add_cursors_vertically(bool down);
    // Replaces the selections with a caret at the end of every line they cover: of each line of a
    // selection except the last, and at the end of the last unless the selection ends at the
    // start of it. Selections that are carets give nothing; when no selection gives a caret,
    // nothing changes.
    EditOutcome add_cursors_to_line_ends();
    // Replaces every selection with a caret where Go to Bracket takes the start of it (see
    // jump_to_bracket).
    EditOutcome jump_to_bracket();
    // Grows every selection to the next range around it, as VS Code's Expand Selection does (see
    // selection_ranges): each selection steps through the ranges of its own caret, the end that
    // is active, that contain it. The steps are remembered while the selections stay as the
    // command left them, so that shrink_selection() goes back through the same ranges. The
    // results are not reversed selections.
    EditOutcome expand_selection();
    // Goes back one range of those that expand_selection() went through. After any other change
    // of the text or the selections it starts from the selections as they are, which have no
    // range to go back to, so it does nothing.
    EditOutcome shrink_selection();

    // --- lines ---

    // Moves the lines that each selection touches one line up or down, over the line above or
    // below them. A selection that ends at the start of a line does not touch that line. The
    // selections move with their text, and the text of the block above or below goes the other
    // way. A selection at the first line (up) or on the last line (down) stays where it is. When
    // the lines that two selections would swap overlap, the one later in the document is dropped.
    EditOutcome move_lines(bool down);
    // Copies the lines that each selection touches above or below themselves. The selections stay
    // with the text where they were: the copy is under them when going up, and above them when
    // going down, so the selections end up on the lower copy then. Selections that touch the same
    // line copy it once, for the first of them.
    EditOutcome copy_lines(bool down);
    // Copies the text of each selection after itself and selects the copy; a caret copies its
    // line below it, like copy_lines(true).
    EditOutcome duplicate_selection();

    // --- comments ---

    // Toggle, add or remove the line comment token of the language on the lines of every selection
    // as VS Code's `LineCommentCommand` does: a toggle removes the tokens when every line has one
    // and adds them otherwise, at the smallest indentation of the lines; a selection that ends at
    // the start of a line leaves that line out; selections that share a line comment it once.
    // A language with only a block comment gets that around the text instead. One undo step.
    // As in VS Code, a cursor whose edits overlap those of an earlier cursor is dropped, and the
    // cursors that end up overlapping are merged. The caller decides what to do for a language that
    // has no tokens, which VS Code treats like any other.
    EditOutcome comment_lines(LineCommentType type, const CommentOptions& options);
    // Wraps the text of every selection in the block comment of the language, or unwraps it when it
    // is wrapped already, as VS Code's `BlockCommentCommand` does. Nothing happens when the
    // language has no block comment.
    EditOutcome toggle_block_comment(const CommentOptions& options);

    // --- occurrences ---

    // What to search for from the primary selection. A lone caret takes the word at it, which
    // matches whole words only and respects case whatever the options say; any other caret takes
    // the word at it, and a selection takes its text, both with `match_case` and `whole_word`.
    // Nothing for a caret that is not in a word.
    std::optional<OccurrenceQuery> occurrence_query(bool match_case, bool whole_word) const;
    // Whether the selections after the first all have the text of the first, ignoring case unless
    // `match_case`. False when one of them is a caret.
    bool selections_share_text(bool match_case) const;
    // Replaces every caret with the word at it, when it has one.
    EditOutcome expand_carets_to_words();
    // Adds `selection`, which becomes the primary one. A selection that overlaps another is
    // merged with it.
    EditOutcome add_selection(Selection selection);
    // Adds the first match of `search` that begins at or after the end of the primary selection,
    // wrapping around the text and skipping matches that overlap a selection, as the primary
    // selection. Does nothing when every match is taken.
    EditOutcome add_next_occurrence(const Search& search);
    // The mirror of add_next_occurrence: the last match that begins before the primary selection,
    // wrapping around from the end of the text.
    EditOutcome add_previous_occurrence(const Search& search);

    struct AllOccurrences {
        EditOutcome outcome;
        // More matches exist than `limit`, and only the first `limit` were selected.
        bool truncated = false;
    };
    // Replaces the selections with every match of `search`, up to `limit`. The match that meets
    // the primary selection becomes the primary one. Does nothing when there is no match.
    AllOccurrences select_all_occurrences(const Search& search, std::size_t limit);

    // The text of the selections, joined by line breaks when there are several; empty when only
    // carets are present.
    std::string selected_text() const;
    // The text for the clipboard, as VS Code puts it there: the selected text of each selection and
    // the line of each caret, which is its text and a line break (also on a last line that has
    // none), in document order, with the carets on one line giving it once. Several pieces are
    // joined by line breaks. Empty when the document has no selection and no caret to copy from.
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
    // Replaces the selections, in document order, with one text each.
    EditOutcome replace_each_selection(const std::vector<std::string>& texts);
    enum class DeleteMode {
        backward,
        forward,
        selections_only,
    };
    EditOutcome delete_with(DeleteMode mode);
    EditOutcome adopt(SelectionSet selections);
    // Applies the edits as one undo step that never joins typing, and makes `selections` the
    // selections with the one at index `primary` primary. Without edits only the selections change.
    EditOutcome apply_line_edit(std::vector<Edit> edits, std::vector<Selection> selections,
                                std::size_t primary);
    EditOutcome after_history(std::expected<std::optional<DocumentChange>, Error> result);

    EditOutcome step_selection(bool expand);

    Document& document_;
    std::optional<std::size_t> goal_column_;
    EditKind last_kind_ = EditKind::none;
    // The document version and selections right after the last joinable edit.
    std::uint64_t last_version_ = 0;
    SelectionSet last_selections_;
    // How far past the end of a short line each end of each selection wants to be, for the
    // selections in `leftovers_for_`; add_cursors_vertically keeps it so that a cursor added over a
    // short line goes back to its column on the next long one.
    struct Leftover {
        std::size_t anchor = 0;
        std::size_t active = 0;
    };
    SelectionSet leftovers_for_;
    std::vector<Leftover> leftovers_;
    // What expand_selection() and shrink_selection() step through: for each selection of the start,
    // its ranges (the selection first) and where it is among them. Valid while the document is at
    // `version` with `selections`, which the last step left.
    struct SmartSelectStep {
        std::vector<Selection> ranges;
        std::size_t index = 0;
    };
    struct SmartSelectState {
        std::uint64_t version = 0;
        SelectionSet selections;
        std::size_t primary = 0;
        std::vector<SmartSelectStep> steps;
    };
    std::optional<SmartSelectState> smart_select_;
};

} // namespace anvil::core
