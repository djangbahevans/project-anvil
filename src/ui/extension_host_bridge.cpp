#include "anvil/ui/extension_host_bridge.hpp"

namespace anvil::ui {

ExtensionHostBridge::ExtensionHostBridge(QObject* parent) : QObject(parent), timer_(this) {
    timer_.setInterval(10);
    connect(&timer_, &QTimer::timeout, this, [this] { tick(); });
}

ExtensionHostBridge::~ExtensionHostBridge() = default;

std::expected<void, Error> ExtensionHostBridge::start(const Config& config,
                                                      std::shared_ptr<core::Logger> logger,
                                                      Callbacks callbacks) {
    callbacks_ = std::move(callbacks);
    core::NullLogger discarded;
    exthost::Contributions contributions =
        exthost::read_contributions(config.extension_dirs, logger ? *logger : discarded);
    auto started = exthost::ExtensionHost::start(exthost::ExtensionHostOptions{
        .node_executable = config.node_executable,
        .vscode_dir = config.vscode_dir,
        .extension_dirs = config.extension_dirs,
        .storage_dir = config.storage_dir,
        .logger = std::move(logger),
    });
    if (!started) {
        return std::unexpected(std::move(started.error()));
    }
    host_ = std::move(*started);
    contributions_ = std::move(contributions);

    exthost::DocumentsService::Observer observer;
    observer.edited = [this](const core::Uri& uri, const core::DocumentChange& change) {
        if (callbacks_.edited) {
            callbacks_.edited(uri, change);
        }
    };
    observer.show_requested = [this](const core::Uri& uri) {
        if (callbacks_.show_requested) {
            callbacks_.show_requested(uri);
        }
    };
    observer.saved = [this](const core::Uri& uri) {
        if (callbacks_.saved) {
            callbacks_.saved(uri);
        }
    };
    host_->documents().set_observer(std::move(observer));
    timer_.start();
    return {};
}

void ExtensionHostBridge::tick() {
    const auto pumped = host_->pump(std::chrono::milliseconds(0));
    if (pumped && *pumped) {
        return;
    }
    timer_.stop();
    std::string reason = pumped ? "the extension host exited" : pumped.error().message;
    if (host_->state() == exthost::HostState::failed) {
        reason = host_->failure();
    }
    host_.reset();
    if (callbacks_.stopped) {
        callbacks_.stopped(reason);
    }
}

std::shared_ptr<core::Document> ExtensionHostBridge::find(const core::Uri& uri) const {
    return host_ ? host_->documents().find(uri) : nullptr;
}

void ExtensionHostBridge::add_document(const core::Uri& uri,
                                       std::shared_ptr<core::Document> document) {
    if (host_) {
        (void)host_->documents().add_document(uri, std::move(document));
    }
}

void ExtensionHostBridge::remove_document(const core::Uri& uri) {
    if (host_) {
        host_->documents().remove_document(uri);
    }
}

void ExtensionHostBridge::show(const core::Uri& uri) {
    if (!host_) {
        return;
    }
    auto& documents = host_->documents();
    // An editor per document: reuse the one that exists.
    if (const auto editor = documents.editor_for(uri)) {
        documents.set_active_editor(*editor);
    } else if (auto added = documents.add_editor(uri)) {
        documents.set_active_editor(*added);
    }
}

void ExtensionHostBridge::set_editor_options(exthost::DocumentsService::EditorOptions options) {
    if (host_) {
        host_->documents().set_editor_options(options);
    }
}

void ExtensionHostBridge::text_changed(const core::Uri& uri, const core::DocumentChange& change) {
    if (host_) {
        host_->documents().document_changed(uri, change);
    }
}

void ExtensionHostBridge::selections_changed(const core::Uri& uri) {
    if (host_) {
        host_->documents().selections_changed(uri);
    }
}

void ExtensionHostBridge::saved(const core::Uri& uri) {
    if (host_) {
        host_->documents().document_saved(uri);
    }
}

} // namespace anvil::ui
