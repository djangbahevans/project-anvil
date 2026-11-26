#pragma once

#include "anvil/core/document.hpp"
#include "anvil/core/log.hpp"
#include "anvil/core/uri.hpp"
#include "anvil/exthost/contributions.hpp"
#include "anvil/exthost/extension_host.hpp"

#include <QObject>
#include <QTimer>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
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
        // The folder extensions work on; they see no workspace when unset.
        std::optional<core::Workspace> workspace{};
        // What the searches that extensions ask for run with.
        exthost::SearchSettings search{};
    };

    struct Callbacks {
        // An extension edited a document.
        std::function<void(const core::Uri&, const core::DocumentChange&)> edited;
        // An extension asked to show a document.
        std::function<void(const core::Uri&)> show_requested;
        // An extension saved a document.
        std::function<void(const core::Uri&)> saved;
        // An extension renamed the file of a document, which is now registered under the new URI.
        std::function<void(const core::Uri& from, const core::Uri& to)> renamed;
        // An extension deleted the file of a document that had no unsaved changes, which is no
        // longer registered.
        std::function<void(const core::Uri&)> deleted;
        // The extension host stopped, with the reason.
        std::function<void(const std::string&)> stopped;
    };

    explicit ExtensionHostBridge(QObject* parent = nullptr);
    ~ExtensionHostBridge() override;

    // Starts the extension host and waits for it to connect. Messages go to `logger`. The
    // configuration is kept, so that restart() can start another host with it.
    std::expected<void, Error> start(const Config& config, std::shared_ptr<core::Logger> logger,
                                     Callbacks callbacks);

    // Stops the running host, if there is one, and starts another with the configuration of
    // start() and `workspace` as the workspace (none for no folder). Requests that the old host
    // has not answered fail with an error that says it restarted, and the contributions of the
    // extensions are read again. The new host knows no documents until the caller adds them; it
    // learns them in its first delta, which it sends once it has connected, so documents must be
    // added before the event loop runs. Also starts a host that stopped for another reason.
    // Fails when start() has not been called or the new host does not start; the bridge is then
    // not running.
    std::expected<void, Error> restart(std::optional<core::Workspace> workspace);
    // What the searches of the next host that starts run with.
    void set_search_settings(exthost::SearchSettings search) {
        config_.search = std::move(search);
    }

    bool running() const {
        return host_ != nullptr;
    }
    exthost::ExtensionHost* host() {
        return host_.get();
    }
    // The commands, key bindings and palette rules that the extensions of the configuration
    // declare in their manifests; empty until the host has started.
    const exthost::Contributions& contributions() const {
        return contributions_;
    }

    // The document registered under `uri`, which extensions may also have opened; null when there
    // is none or no extension host is running.
    std::shared_ptr<core::Document> find(const core::Uri& uri) const;

    // `language` is the language id the document has, where the resolver would choose one.
    void add_document(const core::Uri& uri, std::shared_ptr<core::Document> document,
                      std::optional<std::string> language = std::nullopt);
    void remove_document(const core::Uri& uri);
    // The editor renamed or deleted files itself: the documents of the files that were at and below
    // the path follow, as for an extension's workspace edit, and `callbacks.renamed` and
    // `callbacks.deleted` are called for each. Nothing happens without an extension host.
    void files_renamed(const std::filesystem::path& from, const std::filesystem::path& to);
    void files_deleted(const std::filesystem::path& path);

    // Chooses the language id of the documents that are registered without one, in this host and in
    // the ones that follow a restart.
    void set_language_resolver(exthost::DocumentsService::LanguageResolver resolver);
    // The language id that extensions see for the document; nothing when there is no extension host
    // or the document is not registered.
    std::optional<std::string> language_of(const core::Uri& uri) const;
    // Whether the language was given by an extension, which no resolver changes.
    bool language_is_fixed(const core::Uri& uri) const;
    void language_changed(const core::Uri& uri, std::string language_id);
    // Shows the document in an editor and makes it the active one.
    void show(const core::Uri& uri);

    // The tab size and indentation style that extensions see on every editor.
    void set_editor_options(exthost::DocumentsService::EditorOptions options);

    void text_changed(const core::Uri& uri, const core::DocumentChange& change);
    void selections_changed(const core::Uri& uri);
    void saved(const core::Uri& uri);

private:
    std::expected<void, Error> launch();
    void tick();

    Config config_;
    std::shared_ptr<core::Logger> logger_;
    std::unique_ptr<exthost::ExtensionHost> host_;
    exthost::Contributions contributions_;
    exthost::DocumentsService::LanguageResolver language_resolver_;
    QTimer timer_;
    Callbacks callbacks_;
};

} // namespace anvil::ui
