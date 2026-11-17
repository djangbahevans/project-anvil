#pragma once

#include "anvil/core/document.hpp"
#include "anvil/core/log.hpp"
#include "anvil/core/uri.hpp"
#include "anvil/exthost/extension_host.hpp"

#include <QObject>
#include <QTimer>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace anvil::ui {

// Runs the extension host next to the user interface: starts it, services its connection from the
// event loop, and passes what the editor does to documents on to it. Without a running extension
// host every call does nothing, so the window works the same with or without one.
class ExtensionHostBridge : public QObject {
public:
    struct Config {
        std::filesystem::path node_executable;
        // The transpiled VS Code checkout (see scripts/fetch-vscode.sh).
        std::filesystem::path vscode_dir;
        std::vector<std::filesystem::path> extension_dirs;
        std::filesystem::path storage_dir;
    };

    struct Callbacks {
        // An extension edited a document.
        std::function<void(const core::Uri&, const core::DocumentChange&)> edited;
        // An extension asked to show a document.
        std::function<void(const core::Uri&)> show_requested;
        // The extension host stopped, with the reason.
        std::function<void(const std::string&)> stopped;
    };

    explicit ExtensionHostBridge(QObject* parent = nullptr);
    ~ExtensionHostBridge() override;

    // Starts the extension host and waits for it to connect. Messages go to `logger`.
    std::expected<void, Error> start(const Config& config, std::shared_ptr<core::Logger> logger,
                                     Callbacks callbacks);

    bool running() const {
        return host_ != nullptr;
    }
    exthost::ExtensionHost* host() {
        return host_.get();
    }

    // The document registered under `uri`, which extensions may also have opened; null when there
    // is none or no extension host is running.
    std::shared_ptr<core::Document> find(const core::Uri& uri) const;

    void add_document(const core::Uri& uri, std::shared_ptr<core::Document> document);
    void remove_document(const core::Uri& uri);
    // Shows the document in an editor and makes it the active one.
    void show(const core::Uri& uri);

    void text_changed(const core::Uri& uri, const core::DocumentChange& change);
    void selections_changed(const core::Uri& uri);
    void saved(const core::Uri& uri);

private:
    void tick();

    std::unique_ptr<exthost::ExtensionHost> host_;
    QTimer timer_;
    Callbacks callbacks_;
};

} // namespace anvil::ui
