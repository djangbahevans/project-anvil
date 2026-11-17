#include "anvil/core/document.hpp"

#include <algorithm>
#include <format>
#include <iterator>

namespace anvil::core {
namespace {

enum class Affinity {
    before,
    after,
};

using Signed = std::ptrdiff_t;

Error invalid(std::string message) {
    return Error{ErrorCode::invalid_argument, std::move(message)};
}

Signed to_signed(std::size_t value) {
    return static_cast<Signed>(value);
}

// Maps offsets of the text before a batch of edits to offsets of the text after it. The edits
// must be sorted and must not overlap.
class OffsetMapper {
public:
    template <class Replacement> explicit OffsetMapper(const std::vector<Replacement>& edits) {
        starts_.reserve(edits.size());
        ends_.reserve(edits.size());
        inserted_.reserve(edits.size());
        prefix_shift_.reserve(edits.size() + 1);
        Signed shift = 0;
        prefix_shift_.push_back(0);
        for (const auto& edit : edits) {
            starts_.push_back(edit.start);
            ends_.push_back(edit.end);
            inserted_.push_back(edit.text.size());
            shift += to_signed(edit.text.size()) - to_signed(edit.end - edit.start);
            prefix_shift_.push_back(shift);
        }
    }

    // An offset inside replaced text moves to the end of the new text. An offset at the edge of
    // an insertion stays before it with Affinity::before and moves after it with Affinity::after.
    std::size_t map(std::size_t offset, Affinity affinity) const {
        std::size_t i = static_cast<std::size_t>(
            std::lower_bound(ends_.begin(), ends_.end(), offset) - ends_.begin());
        Signed shift = prefix_shift_[i];
        for (; i < starts_.size() && starts_[i] <= offset; ++i) {
            const bool insertion = starts_[i] == ends_[i];
            const Signed delta = to_signed(inserted_[i]) - to_signed(ends_[i] - starts_[i]);
            if (!insertion && ends_[i] == offset) {
                shift += delta;
                continue;
            }
            if (insertion) {
                if (affinity == Affinity::before) {
                    break;
                }
                shift += delta;
                continue;
            }
            const std::size_t base = static_cast<std::size_t>(to_signed(starts_[i]) + shift);
            if (offset == starts_[i] && affinity == Affinity::before) {
                return base;
            }
            return base + inserted_[i];
        }
        return static_cast<std::size_t>(to_signed(offset) + shift);
    }

private:
    std::vector<std::size_t> starts_;
    std::vector<std::size_t> ends_;
    std::vector<std::size_t> inserted_;
    std::vector<Signed> prefix_shift_;
};

// A selection grows when text is typed at either of its edges and a caret follows what is
// inserted at it.
SelectionSet map_selections(const SelectionSet& selections, const OffsetMapper& mapper) {
    std::vector<Selection> mapped;
    mapped.reserve(selections.all().size());
    for (const Selection& selection : selections.all()) {
        std::size_t start =
            mapper.map(selection.start(), selection.empty() ? Affinity::after : Affinity::before);
        std::size_t end = mapper.map(selection.end(), Affinity::after);
        mapped.push_back(selection.reversed() ? Selection{end, start} : Selection{start, end});
    }
    return SelectionSet::from(std::move(mapped), selections.primary_index());
}

std::optional<Error> check_selections(const Text& text, const SelectionSet& selections) {
    for (const Selection& selection : selections.all()) {
        for (const std::size_t offset : {selection.anchor, selection.active}) {
            if (offset > text.size() || !text.is_char_boundary(offset)) {
                return invalid(std::format(
                    "selection offset {} is outside the text or inside a character", offset));
            }
        }
    }
    return std::nullopt;
}

} // namespace

Document::Document(Text text, LineEnding line_ending, bool bom)
    : buffer_(std::move(text)), line_ending_(line_ending), bom_(bom) {}

std::expected<Document, Error> Document::from_file_bytes(std::string_view bytes) {
    auto file = decode_file(bytes);
    if (!file) {
        return std::unexpected(std::move(file.error()));
    }
    return Document(std::move(file->text), file->line_ending, file->has_bom);
}

std::string Document::to_file_bytes() const {
    return encode_file(text(), line_ending_, bom_);
}

std::uint64_t Document::current_step_id() const {
    return undo_.empty() ? 0 : undo_.back().id;
}

bool Document::is_dirty() const {
    return current_step_id() != saved_step_id_;
}

void Document::mark_saved() {
    saved_step_id_ = current_step_id();
    sealed_ = true;
}

std::expected<void, Error> Document::set_selections(SelectionSet selections) {
    if (auto error = check_selections(text(), selections)) {
        return std::unexpected(std::move(*error));
    }
    selections_ = std::move(selections);
    return {};
}

std::size_t Document::extension_offset(std::size_t offset) const {
    offset = std::min(offset, text().size());
    const std::size_t line = text().position_at(offset).line;
    return text().utf16_offset(offset) + line * (line_ending_width(line_ending_) - 1);
}

std::expected<std::vector<TextChange>, Error>
Document::apply_replacements(const std::vector<Replacement>& replacements) {
    const Text before = buffer_.snapshot();
    std::vector<TextChange> changes;
    changes.reserve(replacements.size());
    for (auto it = replacements.rbegin(); it != replacements.rend(); ++it) {
        auto change = buffer_.replace(it->start, it->end, it->text);
        if (!change) {
            buffer_.reset(before);
            return std::unexpected(std::move(change.error()));
        }
        changes.push_back(std::move(*change));
    }
    return changes;
}

std::expected<std::vector<Document::Replacement>, Error>
Document::to_replacements(std::span<const Edit> edits) const {
    std::vector<Replacement> forward;
    forward.reserve(edits.size());
    for (const Edit& edit : edits) {
        if (edit.start > edit.end || edit.end > text().size()) {
            return std::unexpected(
                invalid(std::format("edit range [{}, {}) is invalid for text of {} bytes",
                                    edit.start, edit.end, text().size())));
        }
        if (!text().is_char_boundary(edit.start) || !text().is_char_boundary(edit.end)) {
            return std::unexpected(invalid("edit boundaries must fall between characters"));
        }
        auto replacement = Text::from_utf8(edit.text);
        if (!replacement) {
            return std::unexpected(std::move(replacement.error()));
        }
        forward.push_back({edit.start, edit.end, std::move(*replacement)});
    }
    std::stable_sort(forward.begin(), forward.end(),
                     [](const Replacement& a, const Replacement& b) { return a.start < b.start; });
    for (std::size_t i = 1; i < forward.size(); ++i) {
        if (forward[i - 1].end > forward[i].start) {
            return std::unexpected(invalid("edits must not overlap"));
        }
    }
    return forward;
}

std::expected<void, Error> Document::validate_edits(std::span<const Edit> edits) const {
    auto replacements = to_replacements(edits);
    if (!replacements) {
        return std::unexpected(std::move(replacements.error()));
    }
    return {};
}

std::expected<DocumentChange, Error> Document::apply_edits(std::span<const Edit> edits,
                                                           EditOptions options) {
    auto converted = to_replacements(edits);
    if (!converted) {
        return std::unexpected(std::move(converted.error()));
    }
    std::vector<Replacement> forward = std::move(*converted);
    if (forward.empty()) {
        return DocumentChange{version_, ChangeCause::edit, {}};
    }

    // The inverse edits are described against the text after the edit: each replaces the text
    // that was inserted with the text that was removed.
    std::vector<Replacement> inverse;
    inverse.reserve(forward.size());
    Signed shift = 0;
    for (const Replacement& r : forward) {
        const std::size_t start = static_cast<std::size_t>(to_signed(r.start) + shift);
        inverse.push_back({start, start + r.text.size(), text().substr(r.start, r.end - r.start)});
        shift += to_signed(r.text.size()) - to_signed(r.end - r.start);
    }

    const SelectionSet selections_before = selections_;
    const OffsetMapper mapper(forward);
    auto changes = apply_replacements(forward);
    if (!changes) {
        return std::unexpected(std::move(changes.error()));
    }
    SelectionSet selections_after = map_selections(selections_, mapper);
    if (options.selections_after) {
        if (auto error = check_selections(text(), *options.selections_after)) {
            // Roll back so that a rejected edit leaves the document as it was.
            std::vector<Replacement> revert = inverse;
            (void)apply_replacements(revert);
            return std::unexpected(std::move(*error));
        }
        selections_after = *options.selections_after;
    }
    selections_ = selections_after;

    const bool join = options.join_previous && !undo_.empty() && !sealed_;
    Step step;
    step.id = next_step_id_++;
    step.group = join ? undo_.back().group : next_group_id_++;
    step.forward = std::move(forward);
    step.inverse = std::move(inverse);
    step.selections_before = selections_before;
    step.selections_after = selections_;
    undo_.push_back(std::move(step));
    redo_.clear();
    sealed_ = false;

    ++version_;
    return DocumentChange{version_, ChangeCause::edit, std::move(*changes)};
}

std::expected<std::optional<DocumentChange>, Error> Document::undo() {
    return replay(true);
}

std::expected<std::optional<DocumentChange>, Error> Document::redo() {
    return replay(false);
}

std::expected<std::optional<DocumentChange>, Error> Document::replay(bool undoing) {
    std::vector<Step>& source = undoing ? undo_ : redo_;
    std::vector<Step>& destination = undoing ? redo_ : undo_;
    if (source.empty()) {
        return std::nullopt;
    }

    // Undoing walks a group from its latest step back to its first; redoing walks it forward.
    // Either way the steps pass to the other stack in the order they were replayed, which puts
    // the step to replay next on top.
    const std::uint64_t group = source.back().group;
    DocumentChange result;
    result.cause = undoing ? ChangeCause::undo : ChangeCause::redo;
    std::optional<SelectionSet> final_selections;
    while (!source.empty() && source.back().group == group) {
        Step step = std::move(source.back());
        source.pop_back();
        auto changes = apply_replacements(undoing ? step.inverse : step.forward);
        if (!changes) {
            source.push_back(std::move(step));
            return std::unexpected(std::move(changes.error()));
        }
        std::move(changes->begin(), changes->end(), std::back_inserter(result.changes));
        final_selections = undoing ? step.selections_before : step.selections_after;
        destination.push_back(std::move(step));
    }
    selections_ = *final_selections;
    ++version_;
    result.version = version_;
    return result;
}

} // namespace anvil::core
