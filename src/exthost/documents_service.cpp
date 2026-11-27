#include "anvil/exthost/documents_service.hpp"

#include "anvil/exthost/vscode_types.hpp"
#include "anvil/exthost/workspace_file_edit.hpp"
#include "anvil/platform/file.hpp"
#include "anvil/platform/file_system.hpp"

#include <algorithm>
#include <set>

namespace anvil::exthost {
namespace {

using nlohmann::json;

constexpr const char* default_language = "plaintext";

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

DocumentsService::DocumentsService(Session& session, RequestRouter& router,
                                   ActivationService& activation)
    : session_(session), activation_(activation) {
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

void DocumentsService::activate_language(const std::string& language_id) {
    if (!activated_languages_.insert(language_id).second) {
        return;
    }
    activation_.fire("onLanguage:" + language_id, ActivationKind::normal, [](CallResult) {});
    activation_.fire("onLanguage", ActivationKind::normal, [](CallResult) {});
}

void DocumentsService::send_delta(const json& delta) {
    if (synchronized_) {
        send(ProxyId::ExtHostDocumentsAndEditors, "$acceptDocumentsAndEditorsDelta",
             json::array({delta}));
    }
}

void DocumentsService::set_editor_options(EditorOptions options) {
    if (options == editor_options_) {
        return;
    }
    editor_options_ = options;
    if (!synchronized_) {
        return;
    }
    for (const Editor& editor : editors_) {
        send(ProxyId::ExtHostEditors, "$acceptEditorPropertiesChanged",
             json::array({editor.id,
                          {{"options", editor_options_json()},
                           {"selections", nullptr},
                           {"visibleRanges", nullptr}}}));
    }
}

json DocumentsService::editor_options_json() const {
    return {{"tabSize", editor_options_.tab_size},
            {"indentSize", editor_options_.tab_size},
            {"originalIndentSize", "tabSize"},
            {"insertSpaces", editor_options_.insert_spaces},
            {"cursorStyle", 1},
            {"lineNumbers", 1}};
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
        {"options", editor_options_json()},
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
    const bool fixed = requested_language.has_value();
    std::string language = fixed                ? std::move(*requested_language)
                           : language_resolver_ ? language_resolver_(uri, document->text().line(0))
                                                : default_language;
    const std::string key = uri.to_string();
    if (entries_.contains(key)) {
        return std::unexpected(failure("the document is already open: " + key));
    }
    if (document->text().size() > max_shared_bytes) {
        return std::unexpected(
            failure("the document is too large to share with extensions: " + key));
    }
    const auto inserted = entries_.emplace(
        key, Entry{std::move(uri), std::move(document), std::move(language), fixed});
    send_delta({{"addedDocuments", json::array({model_added_data(inserted.first->second)})}});
    activate_language(inserted.first->second.language_id);
    return {};
}

std::optional<std::string> DocumentsService::language_of(const core::Uri& uri) const {
    const auto found = entries_.find(uri.to_string());
    return found == entries_.end() ? std::nullopt : std::optional(found->second.language_id);
}

bool DocumentsService::language_is_fixed(const core::Uri& uri) const {
    const auto found = entries_.find(uri.to_string());
    return found != entries_.end() && found->second.language_fixed;
}

void DocumentsService::language_changed(const core::Uri& uri, std::string language_id) {
    const auto found = entries_.find(uri.to_string());
    if (found == entries_.end()) {
        return;
    }
    found->second.language_fixed = false;
    if (found->second.language_id == language_id) {
        return;
    }
    found->second.language_id = std::move(language_id);
    if (synchronized_) {
        send(ProxyId::ExtHostDocuments, "$acceptModelLanguageChanged",
             json::array({uri_to_json(uri), found->second.language_id}));
    }
    activate_language(found->second.language_id);
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

std::expected<bool, Error> file_changed_under(const core::Document& document,
                                              const std::filesystem::path& path) {
    const auto bytes = platform::read_file(path);
    if (bytes) {
        return document.file_conflicts_with(*bytes);
    }
    const ErrorCode code = bytes.error().code;
    if (code == ErrorCode::not_found || code == ErrorCode::not_a_directory) {
        return false;
    }
    return std::unexpected(bytes.error());
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
    if (!overwrite_changed_files_) {
        const auto changed = file_changed_under(*entry->document, *path);
        if (!changed) {
            return failed(changed.error());
        }
        if (*changed) {
            return boolean(false);
        }
    }
    const std::string bytes = entry->document->to_file_bytes();
    auto written = path->has_parent_path() ? platform::make_directory(path->parent_path())
                                           : std::expected<void, Error>();
    if (written) {
        written = platform::write_file(*path, bytes);
    }
    if (!written) {
        return failed(written.error());
    }
    entry->document->record_file_content(bytes);
    entry->document->mark_saved();
    document_saved(entry->uri);
    if (observer_.saved) {
        observer_.saved(entry->uri);
    }
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

std::vector<std::string> DocumentsService::keys_within(const std::filesystem::path& root) const {
    std::vector<std::string> keys;
    for (const auto& [key, entry] : entries_) {
        if (const auto path = entry.uri.file_path(); path && is_path_within(*path, root)) {
            keys.push_back(key);
        }
    }
    return keys;
}

bool DocumentsService::has_dirty_document_within(const std::filesystem::path& root) const {
    return std::ranges::any_of(keys_within(root), [&](const std::string& key) {
        return entries_.at(key).document->is_dirty();
    });
}

void DocumentsService::files_deleted(const std::filesystem::path& root) {
    for (const std::string& key : keys_within(root)) {
        const Entry& entry = entries_.at(key);
        if (entry.document->is_dirty()) {
            continue;
        }
        const core::Uri uri = entry.uri;
        remove_document(uri);
        if (observer_.deleted) {
            observer_.deleted(uri);
        }
    }
}

bool DocumentsService::rekey(const core::Uri& from, const core::Uri& to) {
    const Entry* entry = entry_for(from);
    if (entry == nullptr || entries_.contains(to.to_string())) {
        return false;
    }
    const std::shared_ptr<core::Document> document = entry->document;
    const std::optional<std::string> language =
        entry->language_fixed ? std::optional(entry->language_id) : std::nullopt;
    const std::optional<std::string> editor = editor_for(from);
    const bool was_active = editor && *editor == active_editor_;
    remove_document(from);
    if (!add_document(to, document, language)) {
        return false;
    }
    if (editor) {
        if (const auto added = add_editor(to); added && was_active) {
            set_active_editor(*added);
        }
    }
    return true;
}

std::vector<std::pair<core::Uri, core::Uri>>
DocumentsService::files_renamed(const std::filesystem::path& from,
                                const std::filesystem::path& to) {
    files_deleted(to);
    std::vector<std::pair<core::Uri, core::Uri>> renamed;
    for (const std::string& key : keys_within(from)) {
        const core::Uri old_uri = entries_.at(key).uri;
        const core::Uri new_uri = core::Uri::file(path_moved(*old_uri.file_path(), from, to));
        if (rekey(old_uri, new_uri)) {
            renamed.emplace_back(old_uri, new_uri);
            if (observer_.renamed) {
                observer_.renamed(old_uri, new_uri);
            }
        }
    }
    return renamed;
}

void DocumentsService::undo_document(const core::Uri& uri) {
    Entry* entry = entry_for(uri);
    if (entry == nullptr) {
        return;
    }
    const core::SelectionSet selections_before = entry->document->selections();
    auto undone = entry->document->undo();
    if (!undone || !*undone) {
        return;
    }
    document_changed(uri, **undone);
    if (!(entry->document->selections() == selections_before)) {
        selections_changed(uri);
    }
    if (observer_.edited) {
        observer_.edited(uri, **undone);
    }
}

std::optional<HandlerResult> DocumentsService::try_apply_workspace_edit(const json& args) {
    struct DocumentEdits {
        core::Uri uri;
        std::vector<std::uint64_t> versions;
        json edits = json::array();
    };
    // A step is either a file operation or the text edits that lie between two of them.
    struct Step {
        std::optional<FileOperation> operation;
        std::vector<DocumentEdits> documents;
    };
    std::vector<Step> steps;

    for (const json& item : args.at(0).at("edits")) {
        if (item.contains("textEdit")) {
            auto uri = uri_from_json(item.at("resource"));
            if (!uri) {
                return failed(uri.error());
            }
            if (steps.empty() || steps.back().operation) {
                steps.emplace_back();
            }
            auto& documents = steps.back().documents;
            auto found = std::ranges::find(documents, *uri, &DocumentEdits::uri);
            if (found == documents.end()) {
                documents.push_back(DocumentEdits{*uri, {}, json::array()});
                found = documents.end() - 1;
            }
            if (item.contains("versionId") && item["versionId"].is_number_integer()) {
                found->versions.push_back(item["versionId"].get<std::uint64_t>());
            }
            found->edits.push_back(item["textEdit"]);
        } else if (item.contains("oldResource") || item.contains("newResource")) {
            auto operation = parse_file_operation(item);
            if (!operation) {
                return boolean(false);
            }
            steps.push_back(Step{std::move(*operation), {}});
        } else {
            return boolean(false);
        }
    }

    const auto converted_edits =
        [](const Entry& entry,
           const DocumentEdits& edits) -> std::expected<std::vector<core::Edit>, Error> {
        if (!std::ranges::all_of(edits.versions, [&](std::uint64_t version) {
                return version == entry.document->version();
            })) {
            return std::unexpected(failure("the document changed"));
        }
        auto converted = to_core_edits(entry.document->text(), edits.edits);
        if (!converted) {
            return std::unexpected(converted.error());
        }
        if (auto valid = entry.document->validate_edits(*converted); !valid) {
            return std::unexpected(valid.error());
        }
        return converted;
    };

    // Check everything that can be checked before anything changes. A document that no earlier
    // step of the edit touches is complete here; the others are checked when their step runs.
    FileOperationPlanner planner;
    std::vector<std::filesystem::path> touched;
    std::set<std::string> seen;
    for (const Step& step : steps) {
        if (step.operation) {
            const FileOperation& operation = *step.operation;
            if (operation.kind != FileOperation::Kind::remove &&
                has_dirty_document_within(operation.destination())) {
                return boolean(false);
            }
            if (!planner.check(operation)) {
                return boolean(false);
            }
            touched.push_back(operation.path);
            if (operation.kind == FileOperation::Kind::rename) {
                touched.push_back(operation.target);
            }
            continue;
        }
        for (const DocumentEdits& edits : step.documents) {
            const auto path = edits.uri.file_path();
            const bool settled =
                seen.insert(edits.uri.to_string()).second &&
                (!path || std::ranges::none_of(touched, [&](const std::filesystem::path& other) {
                    return is_path_within(*path, other) || is_path_within(other, *path);
                }));
            if (!settled) {
                continue;
            }
            auto entry = open_or_load(edits.uri);
            if (!entry || !converted_edits(**entry, edits)) {
                return boolean(false);
            }
        }
    }

    std::vector<std::function<void()>> undo;
    const auto roll_back = [&] {
        for (auto step = undo.rbegin(); step != undo.rend(); ++step) {
            (*step)();
        }
        return boolean(false);
    };
    for (const Step& step : steps) {
        if (step.operation) {
            const FileOperation& operation = *step.operation;
            auto performed = perform(operation);
            if (!performed) {
                return roll_back();
            }
            if (!performed->ran) {
                continue;
            }
            std::vector<std::pair<core::Uri, core::Uri>> renamed;
            if (operation.kind == FileOperation::Kind::rename) {
                renamed = files_renamed(operation.path, operation.target);
            } else {
                files_deleted(operation.path);
            }
            undo.push_back([this, undo_file = std::move(performed->undo),
                            renamed = std::move(renamed), kind = operation.kind,
                            path = operation.path] {
                for (auto pair = renamed.rbegin(); pair != renamed.rend(); ++pair) {
                    if (rekey(pair->second, pair->first) && observer_.renamed) {
                        observer_.renamed(pair->second, pair->first);
                    }
                }
                if (undo_file) {
                    undo_file();
                }
                if (kind == FileOperation::Kind::create) {
                    files_deleted(path);
                }
            });
            continue;
        }
        for (const DocumentEdits& edits : step.documents) {
            auto entry = open_or_load(edits.uri);
            if (!entry) {
                return roll_back();
            }
            auto converted = converted_edits(**entry, edits);
            if (!converted || !apply_protocol_edits(**entry, edits.edits, false)) {
                return roll_back();
            }
            undo.push_back([this, uri = edits.uri] { undo_document(uri); });
        }
    }
    return boolean(true);
}

} // namespace anvil::exthost
