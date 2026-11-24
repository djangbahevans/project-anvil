#pragma once

#include "anvil/core/text.hpp"
#include "anvil/core/uri.hpp"
#include "anvil/workbench/editor_layout.hpp"
#include "anvil/workbench/navigation_history.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace anvil::workbench {

// An editor that was closed: the document, where its tab was, and what was selected in it.
struct ClosedEditor {
    GroupId group{};
    // The position of the tab in its group.
    std::size_t index = 0;
    core::Uri uri;
    std::vector<NavigationSelection> selections;
    // Which of the selections is the primary one.
    std::size_t primary = 0;
    // The text of an untitled document, which is gone once its editor is closed; a file is read
    // again.
    std::optional<core::Text> text;
    // The editors that one operation closed, such as Close Others, have the same batch.
    std::size_t batch = 0;
};

// The editors closed in this session, newest last, for Reopen Closed Editor, with the rules of
// VS Code's history service: at most `max_entries`, the oldest forgotten first; an editor that is
// closed again replaces its earlier entry; the editors that one operation closed come back
// together.
class ClosedEditors {
public:
    static constexpr std::size_t max_entries = 20;

    // Starts a batch: editors that are recorded from now on come back together, until the next
    // batch starts.
    void begin_batch() {
        ++batch_;
    }
    // Records a closed editor in the current batch.
    void push(ClosedEditor editor);
    bool empty() const {
        return editors_.empty();
    }
    std::size_t size() const {
        return editors_.size();
    }
    const std::vector<ClosedEditor>& editors() const {
        return editors_;
    }
    // Removes the editors of the newest batch and returns them in the order they were closed.
    std::vector<ClosedEditor> take_last_batch();
    // Forgets the editors of a document.
    void remove_uri(const core::Uri& uri);

private:
    std::vector<ClosedEditor> editors_;
    std::size_t batch_ = 0;
};

} // namespace anvil::workbench
