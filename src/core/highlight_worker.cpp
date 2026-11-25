#include "anvil/core/highlight_worker.hpp"

#include "anvil/core/tree_sitter_highlighter.hpp"

namespace anvil::core {

std::expected<std::unique_ptr<HighlightWorker>, Error>
HighlightWorker::create(std::string_view language_id, Text text, std::uint64_t version,
                        Deliver deliver) {
    if (!TreeSitterHighlighter::supports(language_id)) {
        return std::unexpected(Error{ErrorCode::unsupported,
                                     "no tree-sitter grammar for " + std::string(language_id)});
    }
    if (text.size() > max_highlighted_bytes) {
        return std::unexpected(Error{ErrorCode::limit_exceeded, "the text is too large"});
    }
    std::unique_ptr<HighlightWorker> worker(
        new HighlightWorker(std::string(language_id), std::move(deliver)));
    worker->queued_version_ = version;
    Work first{true, true, {}, std::move(text), version};
    worker->thread_ =
        std::jthread([raw = worker.get(), first = std::move(first)](std::stop_token stop) mutable {
            raw->run(stop, std::move(first));
        });
    return worker;
}

HighlightWorker::HighlightWorker(std::string language_id, Deliver deliver)
    : language_id_(std::move(language_id)), deliver_(std::move(deliver)) {}

HighlightWorker::~HighlightWorker() {
    thread_.request_stop();
    wake_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void HighlightWorker::edit(const DocumentChange& change, const Text& text_after) {
    {
        const std::lock_guard lock(mutex_);
        if (change.changes.empty() && change.version == queued_version_) {
            return;
        }
        const bool follows = change.version == queued_version_ + 1;
        queued_version_ = change.version;
        if (!pending_.present) {
            pending_ = Work{};
            pending_.present = true;
        }
        if (!follows || pending_.replace) {
            pending_.replace = true;
            pending_.changes.clear();
        } else {
            pending_.changes.insert(pending_.changes.end(), change.changes.begin(),
                                    change.changes.end());
        }
        pending_.text = text_after;
        pending_.version = change.version;
    }
    wake_.notify_one();
}

void HighlightWorker::replace(const Text& text, std::uint64_t version) {
    {
        const std::lock_guard lock(mutex_);
        queued_version_ = version;
        pending_ = Work{true, true, {}, text, version};
    }
    wake_.notify_one();
}

std::size_t HighlightWorker::parses() const {
    const std::lock_guard lock(mutex_);
    return parses_;
}

std::size_t HighlightWorker::full_parses() const {
    const std::lock_guard lock(mutex_);
    return full_parses_;
}

void HighlightWorker::run(std::stop_token stop, Work work) {
    std::unique_ptr<TreeSitterHighlighter> highlighter;
    for (;;) {
        const auto count = [&](bool full) {
            const std::lock_guard lock(mutex_);
            ++parses_;
            full_parses_ += full ? 1 : 0;
        };
        if (!work.replace && highlighter &&
            highlighter->apply(work.changes, work.text, stop).has_value()) {
            count(false);
        } else {
            if (stop.stop_requested()) {
                return;
            }
            auto made = TreeSitterHighlighter::create(language_id_, work.text, stop);
            highlighter = made ? std::move(*made) : nullptr;
            count(true);
        }
        if (stop.stop_requested()) {
            return;
        }

        bool newer = false;
        {
            const std::lock_guard lock(mutex_);
            newer = pending_.present;
        }
        if (highlighter && !newer) {
            deliver_(work.version, highlighter->snapshot());
        }

        std::unique_lock lock(mutex_);
        if (!wake_.wait(lock, stop, [this] { return pending_.present; })) {
            return;
        }
        work = std::move(pending_);
        pending_ = Work{};
    }
}

} // namespace anvil::core
