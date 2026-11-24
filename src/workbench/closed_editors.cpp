#include "anvil/workbench/closed_editors.hpp"

#include <algorithm>

namespace anvil::workbench {

void ClosedEditors::push(ClosedEditor editor) {
    remove_uri(editor.uri);
    editor.batch = batch_;
    editors_.push_back(std::move(editor));
    if (editors_.size() > max_entries) {
        editors_.erase(editors_.begin());
    }
}

std::vector<ClosedEditor> ClosedEditors::take_last_batch() {
    std::vector<ClosedEditor> batch;
    if (editors_.empty()) {
        return batch;
    }
    const std::size_t id = editors_.back().batch;
    while (!editors_.empty() && editors_.back().batch == id) {
        batch.push_back(std::move(editors_.back()));
        editors_.pop_back();
    }
    std::ranges::reverse(batch);
    return batch;
}

void ClosedEditors::remove_uri(const core::Uri& uri) {
    std::erase_if(editors_, [&](const ClosedEditor& editor) { return editor.uri == uri; });
}

} // namespace anvil::workbench
