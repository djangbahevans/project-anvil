#pragma once

#include "anvil/core/document.hpp"
#include "anvil/core/highlighter.hpp"
#include "anvil/core/uri.hpp"

#include <QObject>
#include <memory>
#include <string>

namespace anvil::ui {

// The syntax highlighting of the open documents: for each document in a language that has a
// grammar, a worker thread that parses it ([`core::HighlightWorker`]) and the highlighter that the
// views of the document read ([`core::LiveHighlighter`]), which takes the worker's results on the
// UI thread. A document in a language without a grammar, and one that is too large, has none and
// costs nothing.
//
// The window reports every change of a document's text with `edited`; without that a highlighter
// shows the text it last parsed. Everything here runs on the UI thread, except the parsing.
class Highlighting : public QObject {
    Q_OBJECT

public:
    explicit Highlighting(QObject* parent = nullptr);
    // Stops the workers, which ends a parse in progress.
    ~Highlighting() override;

    // The highlighter for the views of the document `uri` that has `document` as its text, whose
    // language is `language`: made, with its worker, when the document is first asked for. A
    // change of language gives a new one. Null when the language has no grammar, the text has
    // more than `core::max_highlighted_bytes`, or the build has no tree-sitter.
    std::shared_ptr<const core::Highlighter>
    highlighter_for(const core::Uri& uri, const std::shared_ptr<core::Document>& document,
                    const std::string& language);

    // The text of the document changed. A document that outgrows the limit loses its highlighter,
    // which `highlighter_for` then reports as null; the result says that it did, so that the views
    // can be given none.
    bool edited(const core::Uri& uri, const core::Document& document,
                const core::DocumentChange& change);

    // The document is gone, or is not highlighted any more.
    void forget(const core::Uri& uri);
    // The document is now known under another uri.
    void rename(const core::Uri& from, const core::Uri& to);

    // Whether `uri` has a highlighter now, and the number of workers: for tests.
    bool has_highlighter(const core::Uri& uri) const;
    std::size_t worker_count() const;

signals:
    // New spans arrived for `highlighter`, which a view that shows it repaints.
    void spans_arrived(const anvil::core::Highlighter* highlighter);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace anvil::ui
