#pragma once

#include "anvil/core/document.hpp"
#include "anvil/core/uri.hpp"
#include "anvil/exthost/request_router.hpp"
#include "anvil/exthost/session.hpp"

#include <cstddef>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace anvil::exthost {

// The documents and text editors that extensions can see, kept in step with the extension host
// (VS Code's MainThreadDocuments, MainThreadTextEditors and MainThreadBulkEdits, plus the
// documents-and-editors delta). Documents are shared with whoever else uses them: the editor
// reports its own changes with document_changed(), and edits that extensions make are applied to
// the same Document objects and announced back.
//
// The session and router must outlive the service.
class DocumentsService {
public:
    // The language id of a document from its resource and the first line of its text.
    using LanguageResolver = std::function<std::string(const core::Uri&, std::string_view)>;

    // Documents above this size are not shared with extensions.
    static constexpr std::size_t max_shared_bytes = std::size_t{50} * 1024 * 1024;

    DocumentsService(Session& session, RequestRouter& router);

    // Callbacks for what extensions do to documents that the editor shows.
    struct Observer {
        // An extension edited the document. The service has applied the change and told the
        // extension host; the callback is for refreshing views.
        std::function<void(const core::Uri&, const core::DocumentChange&)> edited;
        // An extension asked to show the document in an editor. The service has made its editor
        // the active one.
        std::function<void(const core::Uri&)> show_requested;
        // An extension saved the document to its file. The service has written the file, marked
        // the document saved and told the extension host.
        std::function<void(const core::Uri&)> saved;
    };
    void set_observer(Observer observer) {
        observer_ = std::move(observer);
    }

    // The options that every text editor reports to extensions (`TextEditor.options`).
    struct EditorOptions {
        std::size_t tab_size = 4;
        bool insert_spaces = true;

        friend bool operator==(const EditorOptions&, const EditorOptions&) = default;
    };
    // Sets the options of all editors, existing and future. Extensions that already know of an
    // editor are told when the options differ from before.
    void set_editor_options(EditorOptions options);
    const EditorOptions& editor_options() const {
        return editor_options_;
    }

    // Picks the language id of a document that is registered without one; "plaintext" without a
    // resolver.
    void set_language_resolver(LanguageResolver resolver) {
        language_resolver_ = std::move(resolver);
    }

    // --- registering documents and editors ---

    // Makes `document` visible to extensions under `uri`, with `language` as its language id or
    // the one the resolver picks. Fails when the document is already registered or too large to
    // share.
    std::expected<void, Error> add_document(core::Uri uri, std::shared_ptr<core::Document> document,
                                            std::optional<std::string> language = std::nullopt);
    void remove_document(const core::Uri& uri);

    // The language id extensions see for the document, if it is registered.
    std::optional<std::string> language_of(const core::Uri& uri) const;
    // Whether the language was given when the document was registered, which is how an extension
    // that creates a document with a language asks for it; the resolver does not choose it again.
    bool language_is_fixed(const core::Uri& uri) const;
    // Gives the document another language id and tells the extension host, unless it has the id
    // already. The language is then the editor's to decide, and no longer one that was asked for
    // when the document was registered. Nothing happens when the document is not registered.
    void language_changed(const core::Uri& uri, std::string language_id);
    std::shared_ptr<core::Document> find(const core::Uri& uri) const;

    // Shows a registered document in an editor, whose selections are the document's selections.
    // Returns the editor id.
    std::expected<std::string, Error> add_editor(const core::Uri& uri);
    // The id of an editor showing the document, if there is one.
    std::optional<std::string> editor_for(const core::Uri& uri) const;
    void remove_editor(const std::string& id);
    // Makes the editor active; an empty id means no editor is active.
    void set_active_editor(const std::string& id);

    // --- reporting what the editor did ---

    // Reports an edit, undo or redo that was applied to the document.
    void document_changed(const core::Uri& uri, const core::DocumentChange& change);
    void document_saved(const core::Uri& uri);
    // Reports that the selections of the document's editors changed.
    void selections_changed(const core::Uri& uri);

    // The arguments of the first delta, which describes everything registered so far. The
    // extension host must receive it before it activates extensions; later registrations are
    // sent as they happen.
    std::string initial_delta_args();

private:
    struct Entry {
        core::Uri uri;
        std::shared_ptr<core::Document> document;
        std::string language_id;
        bool language_fixed = false;
    };
    struct Editor {
        std::string id;
        std::string uri_key;
    };

    nlohmann::json editor_options_json() const;
    nlohmann::json model_added_data(const Entry& entry) const;
    nlohmann::json editor_added_data(const Editor& editor, const Entry& entry) const;
    void send_delta(const nlohmann::json& delta);
    void send(ProxyId target, const char* method, const nlohmann::json& args);

    Entry* entry_for(const core::Uri& uri);
    Editor* editor_with_id(const std::string& id);
    std::expected<Entry*, Error> open_or_load(const core::Uri& uri);

    std::optional<HandlerResult> try_open_document(const nlohmann::json& args);
    std::optional<HandlerResult> try_create_document(const nlohmann::json& args);
    std::optional<HandlerResult> try_save_document(const nlohmann::json& args);
    std::optional<HandlerResult> try_apply_edits(const nlohmann::json& args);
    std::optional<HandlerResult> try_set_selections(const nlohmann::json& args);
    std::optional<HandlerResult> try_show_text_document(const nlohmann::json& args);
    std::optional<HandlerResult> try_apply_workspace_edit(const nlohmann::json& args);

    // Applies edits given as protocol ranges; returns the change.
    std::expected<core::DocumentChange, Error>
    apply_protocol_edits(Entry& entry, const nlohmann::json& edits, bool join_previous);

    Session& session_;
    LanguageResolver language_resolver_;
    Observer observer_;
    EditorOptions editor_options_;
    std::map<std::string, Entry> entries_;
    std::vector<Editor> editors_;
    std::string active_editor_;
    std::size_t next_editor_ = 1;
    std::size_t next_untitled_ = 1;
    bool synchronized_ = false;
};

} // namespace anvil::exthost
