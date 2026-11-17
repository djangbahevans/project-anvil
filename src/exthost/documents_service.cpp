#include "anvil/exthost/documents_service.hpp"

#include "anvil/exthost/vscode_types.hpp"
#include "anvil/platform/file.hpp"

#include <algorithm>

namespace anvil::exthost {
namespace {

using nlohmann::json;

constexpr const char* default_language = "plaintext";

json default_editor_options() {
    return {{"tabSize", 4},         {"indentSize", 4},  {"originalIndentSize", "tabSize"},
            {"insertSpaces", true}, {"cursorStyle", 1}, {"lineNumbers", 1}};
}

// The extension host models documents with "\n" or "\r\n" only; a file with lone "\r" breaks is
// presented with "\n", which has the same width.
core::LineEnding protocol_line_ending(const core::Document& document) {
    return document.line_ending() == core::LineEnding::crlf ? core::LineEnding::crlf
                                                            : core::LineEnding::lf;
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (true) {
        const std::size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            lines.push_back(text.substr(start));
            return lines;
        }
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
}

json content_change_event(const core::Document& document, const core::DocumentChange& change) {
    const core::LineEnding ending = protocol_line_ending(document);
    const std::size_t extra_width = core::line_ending_width(ending) - 1;
    json changes = json::array();
    for (const core::TextChange& c : change.changes) {
        const std::size_t lines_spanned = c.range.end.line - c.range.start.line;
        changes.push_back({
            {"range", range_to_json(c.range)},
            {"rangeOffset", c.utf16_offset + c.range.start.line * extra_width},
            {"rangeLength", c.removed_utf16 + lines_spanned * extra_width},
            {"text", core::apply_line_ending(c.text, ending)},
        });
    }
    return {
        {"changes", std::move(changes)},
        {"eol", std::string(core::line_ending_text(ending))},
        {"versionId", change.version},
        {"isUndoing", change.cause == core::ChangeCause::undo},
        {"isRedoing", change.cause == core::ChangeCause::redo},
        {"isFlush", false},
        {"isEolChange", false},
    };
}

Error failure(std::string message) {
    return Error{ErrorCode::invalid_argument, std::move(message)};
}

HandlerResult failed(const Error& error) {
    return Failed{error_json(error.message)};
}

HandlerResult boolean(bool value) {
    return OkJson{value ? "true" : "false"};
}

} // namespace

DocumentsService::DocumentsService(Session& session, RequestRouter& router) : session_(session) {
    using Args = const json&;
    auto on = [&](ProxyId service, const char* method,
                  std::optional<HandlerResult> (DocumentsService::*handler)(Args)) {
        router.on(service, method,
                  [this, handler](const Request&, Args args) { return (this->*handler)(args); });
    };
    on(ProxyId::MainThreadDocuments, "$tryOpenDocument", &DocumentsService::try_open_document);
    on(ProxyId::MainThreadDocuments, "$tryCreateDocument", &DocumentsService::try_create_document);
    on(ProxyId::MainThreadDocuments, "$trySaveDocument", &DocumentsService::try_save_document);
    on(ProxyId::MainThreadTextEditors, "$tryApplyEdits", &DocumentsService::try_apply_edits);
    on(ProxyId::MainThreadTextEditors, "$trySetSelections", &DocumentsService::try_set_selections);
    on(ProxyId::MainThreadTextEditors, "$tryShowTextDocument",
       &DocumentsService::try_show_text_document);
    on(ProxyId::MainThreadBulkEdits, "$tryApplyWorkspaceEdit",
       &DocumentsService::try_apply_workspace_edit);
}

DocumentsService::Entry* DocumentsService::entry_for(const core::Uri& uri) {
    const auto found = entries_.find(uri.to_string());
    return found == entries_.end() ? nullptr : &found->second;
}

DocumentsService::Editor* DocumentsService::editor_with_id(const std::string& id) {
    const auto found = std::find_if(editors_.begin(), editors_.end(),
                                    [&](const Editor& editor) { return editor.id == id; });
    return found == editors_.end() ? nullptr : &*found;
}

std::shared_ptr<core::Document> DocumentsService::find(const core::Uri& uri) const {
    const auto found = entries_.find(uri.to_string());
    return found == entries_.end() ? nullptr : found->second.document;
}

void DocumentsService::send(ProxyId target, const char* method, const json& args) {
    session_.call(target, method, args.dump(), [](CallResult) {});
}

void DocumentsService::send_delta(const json& delta) {
    if (synchronized_) {
        send(ProxyId::ExtHostDocumentsAndEditors, "$acceptDocumentsAndEditorsDelta",
             json::array({delta}));
    }
}

json DocumentsService::model_added_data(const Entry& entry) const {
    const core::Document& document = *entry.document;
    return {
        {"uri", uri_to_json(entry.uri)},
        {"versionId", document.version()},
        {"lines", split_lines(document.text().to_string())},
        {"EOL", std::string(core::line_ending_text(protocol_line_ending(document)))},
        {"languageId", entry.language_id},
        {"isDirty", document.is_dirty()},
        {"encoding", "utf8"},
    };
}

json DocumentsService::editor_added_data(const Editor& editor, const Entry& entry) const {
    const core::Document& document = *entry.document;
    json selections = json::array();
    for (const core::Selection& selection : document.selections().all()) {
        selections.push_back(selection_to_json(selection, document.text()));
    }
    return {
        {"id", editor.id},
        {"documentUri", uri_to_json(entry.uri)},
        {"options", default_editor_options()},
        {"selections", std::move(selections)},
        {"visibleRanges", json::array({range_to_json(core::Range{{0, 0}, {0, 0}})})},
        {"editorPosition", 1},
    };
}

std::string DocumentsService::initial_delta_args() {
    json documents = json::array();
    for (const auto& [key, entry] : entries_) {
        documents.push_back(model_added_data(entry));
    }
    json editors = json::array();
    for (const Editor& editor : editors_) {
        editors.push_back(editor_added_data(editor, entries_.at(editor.uri_key)));
    }
    json delta = {{"addedDocuments", std::move(documents)}, {"addedEditors", std::move(editors)}};
    if (!active_editor_.empty()) {
        delta["newActiveEditor"] = active_editor_;
    }
    synchronized_ = true;
    return json::array({delta}).dump();
}

std::expected<void, Error>
DocumentsService::add_document(core::Uri uri, std::shared_ptr<core::Document> document,
                               std::optional<std::string> requested_language) {
    std::string language = requested_language   ? std::move(*requested_language)
                           : language_resolver_ ? language_resolver_(uri)
                                                : default_language;
    const std::string key = uri.to_string();
    if (entries_.contains(key)) {
        return std::unexpected(failure("the document is already open: " + key));
    }
    if (document->text().size() > max_shared_bytes) {
        return std::unexpected(
            failure("the document is too large to share with extensions: " + key));
    }
    const auto inserted =
        entries_.emplace(key, Entry{std::move(uri), std::move(document), std::move(language)});
    send_delta({{"addedDocuments", json::array({model_added_data(inserted.first->second)})}});
    return {};
}

void DocumentsService::remove_document(const core::Uri& uri) {
    const std::string key = uri.to_string();
    const auto found = entries_.find(key);
    if (found == entries_.end()) {
        return;
    }
    json removed_editors = json::array();
    bool active_removed = false;
    for (auto it = editors_.begin(); it != editors_.end();) {
        if (it->uri_key == key) {
            removed_editors.push_back(it->id);
            active_removed = active_removed || it->id == active_editor_;
            it = editors_.erase(it);
        } else {
            ++it;
        }
    }
    json delta = {{"removedDocuments", json::array({uri_to_json(found->second.uri)})},
                  {"removedEditors", std::move(removed_editors)}};
    if (active_removed) {
        active_editor_.clear();
        delta["newActiveEditor"] = nullptr;
    }
    entries_.erase(found);
    send_delta(delta);
}

std::expected<std::string, Error> DocumentsService::add_editor(const core::Uri& uri) {
    Entry* entry = entry_for(uri);
    if (entry == nullptr) {
        return std::unexpected(failure("the document is not open: " + uri.to_string()));
    }
    Editor editor{"anvil-editor-" + std::to_string(next_editor_++), uri.to_string()};
    editors_.push_back(editor);
    send_delta({{"addedEditors", json::array({editor_added_data(editor, *entry)})}});
    return editor.id;
}

std::optional<std::string> DocumentsService::editor_for(const core::Uri& uri) const {
    const std::string key = uri.to_string();
    const auto found = std::find_if(editors_.begin(), editors_.end(),
                                    [&](const Editor& editor) { return editor.uri_key == key; });
    return found == editors_.end() ? std::nullopt : std::optional<std::string>(found->id);
}

void DocumentsService::remove_editor(const std::string& id) {
    const auto found = std::find_if(editors_.begin(), editors_.end(),
                                    [&](const Editor& editor) { return editor.id == id; });
    if (found == editors_.end()) {
        return;
    }
    editors_.erase(found);
    json delta = {{"removedEditors", json::array({id})}};
    if (active_editor_ == id) {
        active_editor_.clear();
        delta["newActiveEditor"] = nullptr;
    }
    send_delta(delta);
}

void DocumentsService::set_active_editor(const std::string& id) {
    if (id == active_editor_) {
        return;
    }
    active_editor_ = id;
    send_delta({{"newActiveEditor", id.empty() ? json(nullptr) : json(id)}});
}

void DocumentsService::document_changed(const core::Uri& uri, const core::DocumentChange& change) {
    if (Entry* entry = entry_for(uri); entry != nullptr && synchronized_) {
        send(ProxyId::ExtHostDocuments, "$acceptModelChanged",
             json::array({uri_to_json(uri), content_change_event(*entry->document, change),
                          entry->document->is_dirty()}));
    }
}

void DocumentsService::document_saved(const core::Uri& uri) {
    if (entry_for(uri) != nullptr && synchronized_) {
        send(ProxyId::ExtHostDocuments, "$acceptModelSaved", json::array({uri_to_json(uri)}));
    }
}

void DocumentsService::selections_changed(const core::Uri& uri) {
    const Entry* entry = entry_for(uri);
    if (entry == nullptr || !synchronized_) {
        return;
    }
    json selections = json::array();
    for (const core::Selection& selection : entry->document->selections().all()) {
        selections.push_back(selection_to_json(selection, entry->document->text()));
    }
    for (const Editor& editor : editors_) {
        if (editor.uri_key == uri.to_string()) {
            send(ProxyId::ExtHostEditors, "$acceptEditorPropertiesChanged",
                 json::array({editor.id,
                              {{"options", nullptr},
                               {"selections", {{"selections", selections}}},
                               {"visibleRanges", nullptr}}}));
        }
    }
}

std::expected<DocumentsService::Entry*, Error>
DocumentsService::open_or_load(const core::Uri& uri) {
    if (Entry* entry = entry_for(uri)) {
        return entry;
    }
    const auto path = uri.file_path();
    if (!path) {
        return std::unexpected(failure("cannot open " + uri.to_string()));
    }
    auto bytes = platform::read_file(*path);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    auto document = core::Document::from_file_bytes(*bytes);
    if (!document) {
        return std::unexpected(failure(path->string() + " is not a UTF-8 text file"));
    }
    if (auto added = add_document(uri, std::make_shared<core::Document>(std::move(*document)));
        !added) {
        return std::unexpected(added.error());
    }
    return entry_for(uri);
}

std::optional<HandlerResult> DocumentsService::try_open_document(const json& args) {
    auto uri = uri_from_json(args.at(0));
    if (!uri) {
        return failed(uri.error());
    }
    auto entry = open_or_load(*uri);
    if (!entry) {
        return failed(entry.error());
    }
    return OkJson{uri_to_json((*entry)->uri).dump()};
}

std::optional<HandlerResult> DocumentsService::try_create_document(const json& args) {
    std::string content;
    std::string language = default_language;
    if (!args.empty() && args[0].is_object()) {
        content = args[0].value("content", "");
        language = args[0].value("language", language);
    }
    auto text = core::Text::from_utf8(content);
    if (!text) {
        return failed(text.error());
    }
    core::Uri uri{"untitled", "", "Untitled-" + std::to_string(next_untitled_++), "", ""};
    auto added = add_document(uri, std::make_shared<core::Document>(std::move(*text)), language);
    if (!added) {
        return failed(added.error());
    }
    return OkJson{uri_to_json(uri).dump()};
}

std::optional<HandlerResult> DocumentsService::try_save_document(const json& args) {
    auto uri = uri_from_json(args.at(0));
    if (!uri) {
        return failed(uri.error());
    }
    Entry* entry = entry_for(*uri);
    const auto path = uri->file_path();
    if (entry == nullptr || !path) {
        return boolean(false);
    }
    if (auto written = platform::write_file(*path, entry->document->to_file_bytes()); !written) {
        return failed(written.error());
    }
    entry->document->mark_saved();
    document_saved(entry->uri);
    return boolean(true);
}

namespace {

std::expected<std::vector<core::Edit>, Error> to_core_edits(const core::Text& text,
                                                            const json& edits) {
    std::vector<core::Edit> result;
    for (const json& edit : edits) {
        auto range = range_from_json(edit.at("range"));
        if (!range) {
            return std::unexpected(range.error());
        }
        const bool has_text = edit.contains("text") && edit["text"].is_string();
        result.push_back({text.offset_at(range->start), text.offset_at(range->end),
                          has_text ? edit["text"].get<std::string>() : std::string()});
    }
    return result;
}

} // namespace

std::expected<core::DocumentChange, Error>
DocumentsService::apply_protocol_edits(Entry& entry, const json& edits, bool join_previous) {
    auto converted = to_core_edits(entry.document->text(), edits);
    if (!converted) {
        return std::unexpected(converted.error());
    }
    const core::SelectionSet selections_before = entry.document->selections();
    auto change =
        entry.document->apply_edits(*converted, core::EditOptions{.join_previous = join_previous});
    if (!change) {
        return std::unexpected(change.error());
    }
    document_changed(entry.uri, *change);
    if (!(entry.document->selections() == selections_before)) {
        selections_changed(entry.uri);
    }
    if (observer_.edited) {
        observer_.edited(entry.uri, *change);
    }
    return change;
}

std::optional<HandlerResult> DocumentsService::try_apply_edits(const json& args) {
    const std::string id = args.at(0).get<std::string>();
    const Editor* editor = editor_with_id(id);
    if (editor == nullptr) {
        return failed(failure("TextEditor(" + id + ") is not open"));
    }
    Entry& entry = entries_.at(editor->uri_key);
    if (args.at(1).get<std::uint64_t>() != entry.document->version()) {
        return boolean(false);
    }
    // Offsets and positions in the edits refer to the document as the extension saw it, which the
    // version check has just confirmed.
    const bool undo_stop_before = args.size() > 3 ? args[3].value("undoStopBefore", true) : true;
    return boolean(apply_protocol_edits(entry, args.at(2), !undo_stop_before).has_value());
}

std::optional<HandlerResult> DocumentsService::try_set_selections(const json& args) {
    const std::string id = args.at(0).get<std::string>();
    const Editor* editor = editor_with_id(id);
    if (editor == nullptr) {
        return failed(failure("TextEditor(" + id + ") is not open"));
    }
    core::Document& document = *entries_.at(editor->uri_key).document;
    std::vector<core::Selection> selections;
    for (const json& item : args.at(1)) {
        auto selection = selection_from_json(item, document.text());
        if (!selection) {
            return failed(selection.error());
        }
        selections.push_back(*selection);
    }
    if (auto set = document.set_selections(core::SelectionSet::from(std::move(selections))); !set) {
        return failed(set.error());
    }
    return Ok{};
}

std::optional<HandlerResult> DocumentsService::try_show_text_document(const json& args) {
    auto uri = uri_from_json(args.at(0));
    if (!uri) {
        return failed(uri.error());
    }
    auto entry = open_or_load(*uri);
    if (!entry) {
        return failed(entry.error());
    }
    const std::string key = uri->to_string();
    const auto existing = std::find_if(editors_.begin(), editors_.end(),
                                       [&](const Editor& editor) { return editor.uri_key == key; });
    std::string id;
    if (existing != editors_.end()) {
        id = existing->id;
    } else if (auto added = add_editor(*uri)) {
        id = *added;
    } else {
        return failed(added.error());
    }
    set_active_editor(id);

    if (args.size() > 1 && args[1].is_object() && args[1].contains("selection")) {
        core::Document& document = *(*entry)->document;
        if (auto range = range_from_json(args[1]["selection"])) {
            const core::Selection selection{document.text().offset_at(range->start),
                                            document.text().offset_at(range->end)};
            if (document.set_selections(core::SelectionSet::from({selection}))) {
                selections_changed(*uri);
            }
        }
    }
    if (observer_.show_requested) {
        observer_.show_requested(*uri);
    }
    return OkJson{json(id).dump()};
}

std::optional<HandlerResult> DocumentsService::try_apply_workspace_edit(const json& args) {
    struct Batch {
        Entry* entry = nullptr;
        json edits = json::array();
        std::vector<core::Edit> converted;
    };
    std::vector<Batch> batches;
    std::map<std::string, std::size_t> batch_index;

    for (const json& item : args.at(0).at("edits")) {
        if (!item.contains("textEdit")) {
            return boolean(false); // file operations are not supported
        }
        auto uri = uri_from_json(item.at("resource"));
        if (!uri) {
            return failed(uri.error());
        }
        auto entry = open_or_load(*uri);
        if (!entry) {
            return boolean(false);
        }
        const json& version = item.contains("versionId") ? item["versionId"] : json();
        if (version.is_number_integer() &&
            version.get<std::uint64_t>() != (*entry)->document->version()) {
            return boolean(false);
        }
        const auto [slot, inserted] = batch_index.emplace(uri->to_string(), batches.size());
        if (inserted) {
            batches.push_back(Batch{*entry, json::array(), {}});
        }
        batches[slot->second].edits.push_back(item["textEdit"]);
    }

    // Check every document before changing any, so the edit applies completely or not at all.
    for (Batch& batch : batches) {
        auto converted = to_core_edits(batch.entry->document->text(), batch.edits);
        if (!converted || !batch.entry->document->validate_edits(*converted)) {
            return boolean(false);
        }
    }
    for (const Batch& batch : batches) {
        if (!apply_protocol_edits(*batch.entry, batch.edits, false)) {
            return boolean(false);
        }
    }
    return boolean(true);
}

} // namespace anvil::exthost
