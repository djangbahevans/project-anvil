#include "anvil/core/live_highlighter.hpp"

#include <algorithm>

namespace anvil::core {

LiveHighlighter::LiveHighlighter(std::string root_scope, std::uint64_t document_version)
    : root_scope_(std::move(root_scope)), document_version_(document_version) {}

void LiveHighlighter::edited(const DocumentChange& change) {
    document_version_ = change.version;
    if (parsed_) {
        for (const TextChange& text_change : change.changes) {
            pending_.push_back(
                {text_change.range.start.line,
                 text_change.range.end.line - text_change.range.start.line + 1,
                 static_cast<std::size_t>(std::ranges::count(text_change.text, '\n')) + 1});
        }
        if (pending_.size() > max_pending_edits) {
            parsed_.reset();
            pending_.clear();
        }
    }
    ++version_;
}

bool LiveHighlighter::deliver(std::uint64_t version, std::shared_ptr<const Highlighter> parsed) {
    if (version != document_version_) {
        return false;
    }
    parsed_ = std::move(parsed);
    parsed_version_ = version;
    pending_.clear();
    ++version_;
    return true;
}

std::vector<HighlightSpan> LiveHighlighter::spans(std::size_t line) const {
    if (!parsed_) {
        return {};
    }
    for (auto shift = pending_.rbegin(); shift != pending_.rend(); ++shift) {
        if (line < shift->first_row) {
            continue;
        }
        if (line >= shift->first_row + shift->new_rows) {
            line = line - shift->new_rows + shift->old_rows;
        } else {
            line = shift->first_row + std::min(line - shift->first_row, shift->old_rows - 1);
        }
    }
    return parsed_->spans(line);
}

} // namespace anvil::core
