#include "anvil/ui/highlighting.hpp"

#include "anvil/core/highlight_worker.hpp"
#include "anvil/core/live_highlighter.hpp"
#include "anvil/core/tree_sitter_highlighter.hpp"

#include <map>

namespace anvil::ui {

struct Highlighting::Impl {
#ifdef ANVIL_TREE_SITTER
    struct Entry {
        std::string language;
        std::shared_ptr<core::LiveHighlighter> live;
        std::unique_ptr<core::HighlightWorker> worker;
    };
    std::map<core::Uri, Entry> entries;
#endif
};

Highlighting::Highlighting(QObject* parent) : QObject(parent), impl_(std::make_unique<Impl>()) {}

Highlighting::~Highlighting() = default;

#ifdef ANVIL_TREE_SITTER

std::shared_ptr<const core::Highlighter>
Highlighting::highlighter_for(const core::Uri& uri, const std::shared_ptr<core::Document>& document,
                              const std::string& language) {
    auto found = impl_->entries.find(uri);
    if (found != impl_->entries.end()) {
        if (found->second.language == language &&
            document->text().size() <= core::max_highlighted_bytes) {
            return found->second.live;
        }
        impl_->entries.erase(found);
    }
    if (!core::TreeSitterHighlighter::supports(language)) {
        return nullptr;
    }

    const std::uint64_t version = document->version();
    auto live = std::make_shared<core::LiveHighlighter>(
        std::string(core::TreeSitterHighlighter::root_scope_of(language)), version);
    auto worker = core::HighlightWorker::create(
        language, document->text(), version,
        [this, weak = std::weak_ptr<core::LiveHighlighter>(live)](
            std::uint64_t parsed_version, std::shared_ptr<const core::Highlighter> parsed) {
            QMetaObject::invokeMethod(
                this,
                [this, weak, parsed_version, parsed = std::move(parsed)] {
                    if (const auto live = weak.lock();
                        live && live->deliver(parsed_version, parsed)) {
                        emit spans_arrived(live.get());
                    }
                },
                Qt::QueuedConnection);
        });
    if (!worker) {
        return nullptr;
    }
    impl_->entries.emplace(uri, Impl::Entry{language, live, std::move(*worker)});
    return live;
}

bool Highlighting::edited(const core::Uri& uri, const core::Document& document,
                          const core::DocumentChange& change) {
    const auto found = impl_->entries.find(uri);
    if (found == impl_->entries.end()) {
        return false;
    }
    if (document.text().size() > core::max_highlighted_bytes) {
        impl_->entries.erase(found);
        return true;
    }
    found->second.live->edited(change);
    found->second.worker->edit(change, document.text());
    return false;
}

void Highlighting::forget(const core::Uri& uri) {
    impl_->entries.erase(uri);
}

void Highlighting::rename(const core::Uri& from, const core::Uri& to) {
    if (auto node = impl_->entries.extract(from)) {
        node.key() = to;
        impl_->entries.insert(std::move(node));
    }
}

bool Highlighting::has_highlighter(const core::Uri& uri) const {
    return impl_->entries.contains(uri);
}

std::size_t Highlighting::worker_count() const {
    return impl_->entries.size();
}

#else

std::shared_ptr<const core::Highlighter>
Highlighting::highlighter_for(const core::Uri&, const std::shared_ptr<core::Document>&,
                              const std::string&) {
    return nullptr;
}

bool Highlighting::edited(const core::Uri&, const core::Document&, const core::DocumentChange&) {
    return false;
}

void Highlighting::forget(const core::Uri&) {}

void Highlighting::rename(const core::Uri&, const core::Uri&) {}

bool Highlighting::has_highlighter(const core::Uri&) const {
    return false;
}

std::size_t Highlighting::worker_count() const {
    return 0;
}

#endif

} // namespace anvil::ui
