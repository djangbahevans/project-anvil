#pragma once

#ifdef ANVIL_TREE_SITTER

#include "anvil/core/document.hpp"
#include "anvil/core/error.hpp"
#include "anvil/core/highlighter.hpp"
#include "anvil/core/text.hpp"

#include <condition_variable>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace anvil::core {

// Parses one document on a thread of its own, so that typing never waits for a parser. It holds
// snapshots of the text and the changes that led to them, never the `Document`.
//
// `edit` queues what an edit did; the thread parses incrementally against the latest text,
// and hands over a highlighter for the text as of the document's version at that time. Edits that
// arrive while a parse runs are coalesced into one parse of the latest text, so a burst of typing
// costs one parse after it, not one per key. The first parse runs on the thread too.
//
// Only available with `ANVIL_TREE_SITTER` (`TreeSitterHighlighter`).
class HighlightWorker {
public:
    // Called on the worker's thread for each result, with the version of the document that it
    // describes. A result is not given when a later edit has arrived, because a newer one is on
    // its way. The highlighter can be read on any one thread.
    using Deliver = std::function<void(std::uint64_t version, std::shared_ptr<const Highlighter>)>;

    // Starts the thread, which parses `text`, the document as of `version`. Fails with
    // ErrorCode::unsupported for a language without a grammar and with ErrorCode::limit_exceeded
    // for a text of more than `max_highlighted_bytes`.
    static std::expected<std::unique_ptr<HighlightWorker>, Error>
    create(std::string_view language_id, Text text, std::uint64_t version, Deliver deliver);

    // Stops the thread, which ends a parse in progress, and waits for it. `deliver` is not called
    // after this returns.
    ~HighlightWorker();
    HighlightWorker(const HighlightWorker&) = delete;
    HighlightWorker& operator=(const HighlightWorker&) = delete;

    // The document changed to `text_after`; `change.version` is its new version. A change that
    // does not follow the previous version by one, which means an edit that was not reported, is
    // handled by parsing `text_after` from scratch.
    void edit(const DocumentChange& change, const Text& text_after);

    // The document's text was replaced by `text` at `version` without a description of the change.
    void replace(const Text& text, std::uint64_t version);

    // Diagnostics for tests: how many parses ran, and how many of them started from scratch.
    std::size_t parses() const;
    std::size_t full_parses() const;

private:
    struct Work {
        bool present = false;
        bool replace = false;
        std::vector<TextChange> changes;
        Text text;
        std::uint64_t version = 0;
    };

    HighlightWorker(std::string language_id, Deliver deliver);
    void run(std::stop_token stop, Work first);

    std::string language_id_;
    Deliver deliver_;
    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    Work pending_;
    std::uint64_t queued_version_ = 0;
    std::size_t parses_ = 0;
    std::size_t full_parses_ = 0;
    std::jthread thread_;
};

} // namespace anvil::core

#endif
