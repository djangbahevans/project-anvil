#pragma once

#include "anvil/core/document.hpp"
#include "anvil/core/highlighter.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace anvil::core {

// The highlighter that a view holds for a document that is edited while a worker parses it on
// another thread. It is read and changed on the thread of the view.
//
// Spans come from the last result of the worker, which describes the text as of some version of
// the document. Until a result for the current version arrives, the edits made since are used to
// ask the result about the line that a line was before them, so that text below an inserted line
// keeps its colors and an edited line keeps the colors it had, instead of the view flickering
// between plain and colored text.
class LiveHighlighter final : public Highlighter {
public:
    // `document_version` is the version of the document that the first result will describe.
    LiveHighlighter(std::string root_scope, std::uint64_t document_version);

    // The document changed. Its version becomes `change.version`.
    void edited(const DocumentChange& change);

    // A result of the worker for `version` of the document. It is used when that is the
    // document's current version, and dropped, with `false`, when edits have been made since:
    // the worker is already parsing a later version.
    bool deliver(std::uint64_t version, std::shared_ptr<const Highlighter> parsed);

    // The most edits remembered while no result for the current version arrives. Past it the
    // older result is forgotten and the document has no spans until the next one.
    static constexpr std::size_t max_pending_edits = 4096;

    std::vector<HighlightSpan> spans(std::size_t line) const override;
    std::string_view root_scope() const override {
        return root_scope_;
    }
    std::uint64_t version() const override {
        return version_;
    }

    // The version of the text that the shown spans describe; zero before the first result.
    std::uint64_t parsed_version() const {
        return parsed_version_;
    }

private:
    // One edit: `old_rows` lines starting at `first_row` became `new_rows` lines.
    struct Shift {
        std::size_t first_row = 0;
        std::size_t old_rows = 0;
        std::size_t new_rows = 0;
    };

    std::string root_scope_;
    std::shared_ptr<const Highlighter> parsed_;
    std::uint64_t parsed_version_ = 0;
    std::uint64_t document_version_ = 0;
    std::uint64_t version_ = 1;
    std::vector<Shift> pending_;
};

} // namespace anvil::core
