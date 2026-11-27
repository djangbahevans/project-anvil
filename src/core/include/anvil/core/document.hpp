#pragma once

#include "anvil/core/error.hpp"
#include "anvil/core/selection.hpp"
#include "anvil/core/text.hpp"
#include "anvil/core/text_buffer.hpp"
#include "anvil/core/text_file.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace anvil::core {

// Replaces the bytes [start, end) with `text`. A pure insertion has start == end.
struct Edit {
    std::size_t start = 0;
    std::size_t end = 0;
    std::string text;
};

struct EditOptions {
    // The selections after the edit. When unset, the current selections move with the edit.
    std::optional<SelectionSet> selections_after{};
    // Makes the edit part of the previous undo step's group, so undo reverts both together, as
    // for consecutive typing. Ignored when there is no previous step or the document was saved
    // since.
    bool join_previous = false;
};

enum class ChangeCause {
    edit,
    undo,
    redo,
};

// What a mutation did to the text. `changes` are in the order to apply them one after another:
// each is described against the text as left by the ones before it, and the changes of one
// batch run from the end of the text to the start so that offsets stay valid.
struct DocumentChange {
    std::uint64_t version = 0;
    ChangeCause cause = ChangeCause::edit;
    std::vector<TextChange> changes;
};

// A text document: the text, its selections, undo history, and the file format details needed to
// save it back the way it was loaded.
class Document {
public:
    // The number of undo groups kept unless set_undo_limit() says otherwise.
    static constexpr std::size_t default_undo_limit = 1000;

    explicit Document(Text text = {}, LineEnding line_ending = LineEnding::lf, bool bom = false);

    // Fails with ErrorCode::invalid_encoding when `bytes` is not valid UTF-8.
    static std::expected<Document, Error> from_file_bytes(std::string_view bytes);
    std::string to_file_bytes() const;

    // Remembers what the document's file holds, as it was read or written. from_file_bytes does
    // this for the bytes it reads.
    void record_file_content(std::string_view bytes);
    // Whether the file's bytes are the ones remembered. False when nothing is remembered.
    bool matches_file(std::string_view bytes) const;
    // Whether writing the document over a file that holds `bytes` would lose what another writer
    // put there: the bytes are neither the remembered ones nor the ones the document would write.
    // False when nothing is remembered.
    bool file_conflicts_with(std::string_view bytes) const;

    const Text& text() const {
        return buffer_.snapshot();
    }
    LineEnding line_ending() const {
        return line_ending_;
    }
    bool has_bom() const {
        return bom_;
    }
    // Takes the format of a file whose content the document is given. Not part of the undo history.
    void set_file_format(LineEnding line_ending, bool bom) {
        line_ending_ = line_ending;
        bom_ = bom;
    }

    // Starts at 1 and grows with every change, including undo and redo.
    std::uint64_t version() const {
        return version_;
    }

    // True when the text differs from the text at the last mark_saved() (or from loading).
    // Undoing back to the saved state makes the document clean again. A saved state whose undo
    // step was dropped because of the undo limit cannot be undone back to, and the document stays
    // dirty until the next save.
    bool is_dirty() const;
    void mark_saved();

    const SelectionSet& selections() const {
        return selections_;
    }
    // Fails with ErrorCode::invalid_argument when a selection is out of range or not on character
    // boundaries.
    std::expected<void, Error> set_selections(SelectionSet selections);

    // Checks that apply_edits would accept the edits, without applying them.
    std::expected<void, Error> validate_edits(std::span<const Edit> edits) const;

    // Applies the edits as one undo step. The edits are given in coordinates of the current
    // text, in any order, and must not overlap; edits at the same offset keep the order given.
    // Nothing changes when an edit is invalid. An empty list changes nothing and returns no
    // changes.
    std::expected<DocumentChange, Error> apply_edits(std::span<const Edit> edits,
                                                     EditOptions options = {});

    // The most undo groups the history keeps. When an edit makes it longer, whole groups are
    // dropped from the oldest end, so an undo never reverts half of a typing run. A limit of zero
    // keeps no history. Redo history is not affected, and a group that returns from it after the
    // limit was lowered is dropped again when it would exceed the limit. Lowering the limit
    // drops groups at once; raising it does not bring any back.
    std::size_t undo_limit() const {
        return undo_limit_;
    }
    void set_undo_limit(std::size_t groups);

    bool can_undo() const {
        return !undo_.empty();
    }
    bool can_redo() const {
        return !redo_.empty();
    }
    // Reverts the latest undo group. Returns std::nullopt when there is nothing to undo or redo.
    std::expected<std::optional<DocumentChange>, Error> undo();
    std::expected<std::optional<DocumentChange>, Error> redo();

    // The UTF-16 offset of byte `offset` as extensions count it: with each line break as wide
    // as the document's line ending.
    std::size_t extension_offset(std::size_t offset) const;

private:
    struct Replacement {
        std::size_t start = 0;
        std::size_t end = 0;
        Text text;
    };
    struct Step {
        std::uint64_t id = 0;
        std::uint64_t group = 0;
        std::vector<Replacement> forward;
        std::vector<Replacement> inverse;
        SelectionSet selections_before;
        SelectionSet selections_after;
    };

    // The edits as replacements sorted by position, or the reason they are invalid.
    std::expected<std::vector<Replacement>, Error>
    to_replacements(std::span<const Edit> edits) const;
    std::expected<std::vector<TextChange>, Error>
    apply_replacements(const std::vector<Replacement>& replacements);
    std::expected<std::optional<DocumentChange>, Error> replay(bool undoing);
    std::uint64_t current_step_id() const;
    void drop_groups_over_the_limit();

    // The size and a hash stand for the bytes, which would otherwise be kept twice.
    struct FileContent {
        std::size_t size = 0;
        std::uint64_t hash = 0;

        bool operator==(const FileContent&) const = default;
    };
    static FileContent describe_file(std::string_view bytes);

    TextBuffer buffer_;
    std::optional<FileContent> file_content_;
    SelectionSet selections_;
    LineEnding line_ending_;
    bool bom_;
    std::uint64_t version_ = 1;
    // Dropping the oldest steps is constant time. Steps of one group are next to each other.
    std::deque<Step> undo_;
    std::deque<Step> redo_;
    std::size_t undo_groups_ = 0;
    std::size_t undo_limit_ = default_undo_limit;
    // The identity of the last step dropped from the front of undo_ (0 when none was): the state
    // the text is in when every kept step has been undone. Without it that state would be named
    // like the initial one, and a document saved at its start would look clean again.
    std::uint64_t dropped_step_id_ = 0;
    std::uint64_t next_step_id_ = 1;
    std::uint64_t next_group_id_ = 1;
    std::uint64_t saved_step_id_ = 0;
    bool sealed_ = false;
};

} // namespace anvil::core
