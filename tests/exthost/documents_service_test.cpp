#include "anvil/exthost/documents_service.hpp"
#include "anvil/exthost/vscode_types.hpp"
#include "anvil/platform/file.hpp"
#include "fake_host.hpp"

#include <filesystem>
#include <gtest/gtest.h>
#include <unistd.h>

using namespace anvil::exthost;
using anvil::exthost::test::FakeHost;
using nlohmann::json;
namespace core = anvil::core;

namespace {

// What the service sent to the extension host: calls it made and replies it gave.
struct Sent {
    std::vector<RpcMessage> messages;

    // The calls, as "Service.method" with their parsed arguments.
    std::vector<std::pair<std::string, json>> calls() const {
        std::vector<std::pair<std::string, json>> result;
        for (const auto& message : messages) {
            if (const auto* request = std::get_if<Request>(&message)) {
                result.emplace_back(std::string(proxy_name(request->proxy_id)) + "." +
                                        request->method,
                                    *parse_arguments(*request));
            }
        }
        return result;
    }

    // The JSON of the reply to request `req`, or "<none>" / "<empty>" / "<error: ...>".
    std::string reply_to(std::uint32_t req) const {
        for (const auto& message : messages) {
            if (const auto* reply = std::get_if<JsonReply>(&message); reply && reply->req == req) {
                return reply->json;
            }
            if (const auto* reply = std::get_if<EmptyReply>(&message); reply && reply->req == req) {
                return "<empty>";
            }
            if (const auto* reply = std::get_if<ErrorReply>(&message); reply && reply->req == req) {
                return "<error: " + reply->json + ">";
            }
        }
        return "<none>";
    }

    // The position of the first message of the given kind, for ordering checks.
    std::size_t index_of_call(const std::string& name) const {
        std::size_t index = 0;
        for (const auto& message : messages) {
            if (const auto* request = std::get_if<Request>(&message);
                request &&
                std::string(proxy_name(request->proxy_id)) + "." + request->method == name) {
                return index;
            }
            ++index;
        }
        return messages.size();
    }
    std::size_t index_of_reply(std::uint32_t req) const {
        std::size_t index = 0;
        for (const auto& message : messages) {
            if (std::holds_alternative<JsonReply>(message) &&
                std::get<JsonReply>(message).req == req) {
                return index;
            }
            ++index;
        }
        return messages.size();
    }
};

// An edit that inserts `text` at the zero-based position, as an item of a workspace edit.
json text_item(const core::Uri& uri, int line, int column, std::string text,
               std::optional<int> version = std::nullopt) {
    json item = {{"resource", uri_to_json(uri)},
                 {"textEdit",
                  {{"range",
                    {{"startLineNumber", line + 1},
                     {"startColumn", column + 1},
                     {"endLineNumber", line + 1},
                     {"endColumn", column + 1}}},
                   {"text", std::move(text)}}}};
    if (version) {
        item["versionId"] = *version;
    }
    return item;
}

// An edit that replaces the columns [first, last) of a line.
json replace_item(const core::Uri& uri, int line, int first, int last, std::string text) {
    return {{"resource", uri_to_json(uri)},
            {"textEdit",
             {{"range",
               {{"startLineNumber", line + 1},
                {"startColumn", first + 1},
                {"endLineNumber", line + 1},
                {"endColumn", last + 1}}},
              {"text", std::move(text)}}}};
}

json create_item(const std::filesystem::path& path, json options = json::object(),
                 std::string_view base64_contents = "") {
    if (!base64_contents.empty()) {
        options["contents"] = {{"type", "base64"}, {"value", std::string(base64_contents)}};
    }
    return {{"newResource", uri_to_json(core::Uri::file(path))}, {"options", std::move(options)}};
}

json delete_item(const std::filesystem::path& path, json options = json::object()) {
    return {{"oldResource", uri_to_json(core::Uri::file(path))}, {"options", std::move(options)}};
}

json rename_item(const std::filesystem::path& from, const std::filesystem::path& to,
                 json options = json::object()) {
    return {{"oldResource", uri_to_json(core::Uri::file(from))},
            {"newResource", uri_to_json(core::Uri::file(to))},
            {"options", std::move(options)}};
}

std::string slurp(const std::filesystem::path& path) {
    const auto bytes = anvil::platform::read_file(path);
    return bytes ? *bytes : "<unreadable>";
}

bool is_on_disk(const std::filesystem::path& path) {
    return std::filesystem::exists(path);
}

class DocumentsServiceTest : public ::testing::Test {
protected:
    DocumentsServiceTest()
        : session_("{}"), host_(session_), activation_(session_),
          service_(session_, router_, activation_) {
        session_.set_request_handler([this](const Request& r) { return router_.dispatch(r); });
        host_.complete_handshake();
        scratch_ = std::filesystem::temp_directory_path() /
                   ("anvil-docs-test-" + std::to_string(::getpid()));
        std::filesystem::create_directories(scratch_);
    }
    ~DocumentsServiceTest() override {
        std::filesystem::remove_all(scratch_);
    }

    std::shared_ptr<core::Document> make_document(std::string_view bytes) {
        return std::make_shared<core::Document>(*core::Document::from_file_bytes(bytes));
    }

    core::Uri open(std::string_view name, std::string_view bytes) {
        const core::Uri uri = core::Uri::file(scratch_ / name);
        EXPECT_TRUE(service_.add_document(uri, make_document(bytes)).has_value());
        return uri;
    }

    // Marks the extension host as informed, discarding what was sent for the initial delta.
    json synchronize() {
        const json delta = json::parse(service_.initial_delta_args())[0];
        sent();
        return delta;
    }

    Sent sent() {
        return Sent{host_.drain_rpc()};
    }

    // The activation events among what was sent, in order.
    static std::vector<std::string> activations(const Sent& sent) {
        std::vector<std::string> events;
        for (const auto& [method, args] : sent.calls()) {
            if (method == "ExtHostExtensionService.$activateByEvent") {
                EXPECT_EQ(args[1], 0) << "the normal activation kind";
                events.push_back(args[0].get<std::string>());
            }
        }
        return events;
    }

    // Sends a request from the extension host and returns everything the service wrote back.
    Sent request(ProxyId service, std::string method, json args, std::uint32_t req = 1) {
        EXPECT_TRUE(host_
                        .send_rpc(Request{req, static_cast<std::uint8_t>(service),
                                          std::move(method), JsonArgs{args.dump()}})
                        .has_value());
        return sent();
    }

    // Applies a workspace edit made of `items` and returns what the service wrote back; the reply
    // is request number `req`.
    Sent apply_edit(std::vector<json> items, std::uint32_t req = 1) {
        return request(ProxyId::MainThreadBulkEdits, "$tryApplyWorkspaceEdit",
                       json::array({json{{"edits", std::move(items)}}}), req);
    }

    std::filesystem::path path(std::string_view name) const {
        return scratch_ / name;
    }
    std::filesystem::path write(std::string_view name, std::string_view bytes) {
        const auto file = path(name);
        std::filesystem::create_directories(file.parent_path());
        EXPECT_TRUE(anvil::platform::write_file(file, bytes).has_value());
        return file;
    }

    Session session_;
    FakeHost host_;
    RequestRouter router_;
    ActivationService activation_;
    DocumentsService service_;
    std::filesystem::path scratch_;
};

} // namespace

TEST_F(DocumentsServiceTest, NothingIsSentBeforeTheInitialDelta) {
    open("a.txt", "hello");
    EXPECT_TRUE(sent().calls().empty());
}

TEST_F(DocumentsServiceTest, InitialDeltaDescribesDocumentsEditorsAndTheActiveEditor) {
    const core::Uri uri = open("a.txt", "one\r\ntwo\r\n");
    const std::string editor = *service_.add_editor(uri);
    service_.set_active_editor(editor);
    ASSERT_TRUE(service_.find(uri)->set_selections(core::SelectionSet::from({{5, 1}})).has_value());

    const json delta = synchronize();
    ASSERT_EQ(delta["addedDocuments"].size(), 1u);
    const json& model = delta["addedDocuments"][0];
    EXPECT_EQ(model["uri"]["scheme"], "file");
    EXPECT_EQ(model["uri"]["path"], uri.path);
    EXPECT_EQ(model["lines"], json::parse(R"(["one","two",""])"));
    EXPECT_EQ(model["EOL"], "\r\n");
    EXPECT_EQ(model["versionId"], 1);
    EXPECT_EQ(model["languageId"], "plaintext");
    EXPECT_FALSE(model["isDirty"].get<bool>());
    EXPECT_EQ(model["encoding"], "utf8");

    ASSERT_EQ(delta["addedEditors"].size(), 1u);
    const json& added = delta["addedEditors"][0];
    EXPECT_EQ(added["id"], editor);
    EXPECT_EQ(added["documentUri"]["path"], uri.path);
    // text is "one\ntwo\n": offset 5 is line 2 column 2 (one-based), offset 1 is line 1 column 2
    EXPECT_EQ(
        added["selections"][0],
        json::parse(
            R"({"selectionStartLineNumber":2,"selectionStartColumn":2,"positionLineNumber":1,"positionColumn":2})"));
    EXPECT_EQ(delta["newActiveEditor"], editor);
}

TEST_F(DocumentsServiceTest, DocumentsAddedLaterAreSentAsTheyHappen) {
    synchronize();
    const core::Uri uri = open("late.txt", "x");
    const auto calls = sent().calls();
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].first, "ExtHostDocumentsAndEditors.$acceptDocumentsAndEditorsDelta");
    EXPECT_EQ(calls[0].second[0]["addedDocuments"][0]["uri"]["path"], uri.path);
}

TEST_F(DocumentsServiceTest, ALanguageResolverChoosesTheLanguageIdFromTheResourceAndTheFirstLine) {
    std::vector<std::string> first_lines;
    service_.set_language_resolver([&](const core::Uri& uri, std::string_view first_line) {
        first_lines.emplace_back(first_line);
        return uri.path.ends_with(".md") ? "markdown" : "plaintext";
    });
    open("readme.md", "# hi\nsecond");
    open("empty.txt", "");
    open("crlf.txt", "first\r\nsecond");
    EXPECT_EQ(first_lines, (std::vector<std::string>{"# hi", "", "first"}));
    std::map<std::string, std::string> languages;
    const json delta = synchronize();
    for (const json& document : delta["addedDocuments"]) {
        languages[document["uri"]["path"].get<std::string>()] = document["languageId"];
    }
    EXPECT_EQ(languages[core::Uri::file(scratch_ / "readme.md").path], "markdown");
    EXPECT_EQ(languages[core::Uri::file(scratch_ / "empty.txt").path], "plaintext");
}

TEST_F(DocumentsServiceTest, ALanguageGivenAtRegistrationIsKeptAndNotAskedOfTheResolver) {
    bool asked = false;
    service_.set_language_resolver([&](const core::Uri&, std::string_view) {
        asked = true;
        return "markdown";
    });
    const core::Uri given = core::Uri::file(scratch_ / "given.txt");
    ASSERT_TRUE(service_.add_document(given, make_document("x"), "python").has_value());
    EXPECT_FALSE(asked);
    EXPECT_EQ(service_.language_of(given), "python");
    EXPECT_TRUE(service_.language_is_fixed(given));

    service_.language_changed(given, "markdown");
    EXPECT_FALSE(service_.language_is_fixed(given))
        << "the editor decides the language from now on";
    service_.language_changed(given, "markdown");
    EXPECT_EQ(service_.language_of(given), "markdown");

    const core::Uri resolved = open("resolved.txt", "x");
    EXPECT_EQ(service_.language_of(resolved), "markdown");
    EXPECT_FALSE(service_.language_is_fixed(resolved));
    EXPECT_EQ(service_.language_of(core::Uri::file(scratch_ / "unknown.txt")), std::nullopt);
    EXPECT_FALSE(service_.language_is_fixed(core::Uri::file(scratch_ / "unknown.txt")));
}

TEST_F(DocumentsServiceTest, ALanguageChangeIsReportedToTheExtensionHostOnce) {
    const core::Uri uri = open("a.txt", "x");
    synchronize();
    service_.language_changed(uri, "python");
    const auto calls = sent().calls();
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].first, "ExtHostDocuments.$acceptModelLanguageChanged");
    EXPECT_EQ(calls[0].second[0]["path"], uri.path);
    EXPECT_EQ(calls[0].second[1], "python");
    EXPECT_EQ(service_.language_of(uri), "python");

    service_.language_changed(uri, "python");
    service_.language_changed(core::Uri::file(scratch_ / "unknown.txt"), "python");
    EXPECT_TRUE(sent().calls().empty());
}

TEST_F(DocumentsServiceTest, ALanguageChangeBeforeTheInitialDeltaIsInTheDelta) {
    const core::Uri uri = open("a.txt", "x");
    service_.language_changed(uri, "python");
    EXPECT_TRUE(sent().calls().empty());
    EXPECT_EQ(synchronize()["addedDocuments"][0]["languageId"], "python");
}

TEST_F(DocumentsServiceTest, TheFirstDocumentOfALanguageFiresItsActivationEvents) {
    activation_.set_ready();
    synchronize();
    const core::Uri first = core::Uri::file(scratch_ / "a.py");
    ASSERT_TRUE(service_.add_document(first, make_document("x"), "python").has_value());
    const Sent added = sent();
    EXPECT_EQ(activations(added), (std::vector<std::string>{"onLanguage:python", "onLanguage"}));
    EXPECT_LT(added.index_of_call("ExtHostDocumentsAndEditors.$acceptDocumentsAndEditorsDelta"),
              added.index_of_call("ExtHostExtensionService.$activateByEvent"))
        << "the extensions that start must see the document";

    ASSERT_TRUE(
        service_.add_document(core::Uri::file(scratch_ / "b.py"), make_document("y"), "python")
            .has_value());
    EXPECT_TRUE(activations(sent()).empty());
    service_.remove_document(first);
    ASSERT_TRUE(service_.add_document(first, make_document("x"), "python").has_value());
    EXPECT_TRUE(activations(sent()).empty());

    open("c.txt", "z");
    EXPECT_EQ(activations(sent()),
              (std::vector<std::string>{"onLanguage:plaintext", "onLanguage"}));
}

TEST_F(DocumentsServiceTest, ADocumentThatIsNotRegisteredFiresNothing) {
    activation_.set_ready();
    const core::Uri uri = open("a.txt", "x");
    sent();
    EXPECT_FALSE(service_.add_document(uri, make_document("y"), "python").has_value());
    service_.language_changed(core::Uri::file(scratch_ / "unknown.txt"), "python");
    auto huge = std::make_shared<core::Document>(
        *core::Text::from_utf8(std::string(DocumentsService::max_shared_bytes + 1, 'a')));
    EXPECT_FALSE(
        service_.add_document(core::Uri::file(scratch_ / "huge"), huge, "python").has_value());
    EXPECT_TRUE(activations(sent()).empty());
}

TEST_F(DocumentsServiceTest, ALanguageChangeFiresTheEventsOfANewLanguage) {
    activation_.set_ready();
    const core::Uri uri = open("a.txt", "x");
    synchronize();
    service_.language_changed(uri, "python");
    const Sent changed = sent();
    EXPECT_EQ(activations(changed), (std::vector<std::string>{"onLanguage:python", "onLanguage"}));
    EXPECT_LT(changed.index_of_call("ExtHostDocuments.$acceptModelLanguageChanged"),
              changed.index_of_call("ExtHostExtensionService.$activateByEvent"));

    service_.language_changed(uri, "plaintext");
    service_.language_changed(uri, "python");
    EXPECT_TRUE(activations(sent()).empty()) << "both languages have started their extensions";
}

TEST_F(DocumentsServiceTest, TheEventsOfDocumentsOpenBeforeTheHostIsReadyWaitForIt) {
    const core::Uri uri = open("a.txt", "x");
    service_.language_changed(uri, "python");
    EXPECT_TRUE(sent().calls().empty());
    synchronize();
    activation_.set_ready();
    EXPECT_EQ(activations(sent()), (std::vector<std::string>{"onLanguage:plaintext", "onLanguage",
                                                             "onLanguage:python", "onLanguage"}));
}

TEST_F(DocumentsServiceTest, ADocumentThatAnExtensionCreatesWithALanguageFiresItsEvents) {
    activation_.set_ready();
    synchronize();
    const Sent created = request(ProxyId::MainThreadDocuments, "$tryCreateDocument",
                                 json::array({json{{"language", "python"}, {"content", "x"}}}));
    EXPECT_EQ(activations(created), (std::vector<std::string>{"onLanguage:python", "onLanguage"}));
}

TEST_F(DocumentsServiceTest, RejectsDuplicateAndOversizedDocuments) {
    const core::Uri uri = open("a.txt", "x");
    EXPECT_FALSE(service_.add_document(uri, make_document("y")).has_value());
    const auto huge = std::make_shared<core::Document>(
        *core::Text::from_utf8(std::string(DocumentsService::max_shared_bytes + 1, 'a')));
    EXPECT_FALSE(service_.add_document(core::Uri::file(scratch_ / "huge.txt"), huge).has_value());
    EXPECT_EQ(service_.find(core::Uri::file(scratch_ / "huge.txt")), nullptr);
}

TEST_F(DocumentsServiceTest, RemovingADocumentRemovesItsEditorsAndActiveState) {
    const core::Uri uri = open("a.txt", "x");
    const std::string editor = *service_.add_editor(uri);
    service_.set_active_editor(editor);
    synchronize();

    service_.remove_document(uri);
    const auto calls = sent().calls();
    ASSERT_EQ(calls.size(), 1u);
    const json& delta = calls[0].second[0];
    EXPECT_EQ(delta["removedDocuments"][0]["path"], uri.path);
    EXPECT_EQ(delta["removedEditors"][0], editor);
    EXPECT_TRUE(delta["newActiveEditor"].is_null());
    EXPECT_EQ(service_.find(uri), nullptr);
}

// --- reporting changes ---

TEST_F(DocumentsServiceTest, DocumentChangesAreReportedWithOneBasedRangesAndOffsets) {
    const core::Uri uri = open("a.txt", "ab\ncd\nef");
    synchronize();
    auto document = service_.find(uri);
    const auto change = document->apply_edits(std::vector<core::Edit>{{4, 6, "X"}, {0, 0, "!"}});
    ASSERT_TRUE(change.has_value());
    service_.document_changed(uri, *change);

    const auto calls = sent().calls();
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].first, "ExtHostDocuments.$acceptModelChanged");
    const json& args = calls[0].second;
    EXPECT_EQ(args[0]["path"], uri.path);
    EXPECT_TRUE(args[2].get<bool>()); // dirty
    const json& event = args[1];
    EXPECT_EQ(event["versionId"], 2);
    EXPECT_EQ(event["eol"], "\n");
    EXPECT_FALSE(event["isUndoing"].get<bool>());
    ASSERT_EQ(event["changes"].size(), 2u);
    // ordered from the end of the text to the start
    EXPECT_EQ(
        event["changes"][0]["range"],
        json::parse(R"({"startLineNumber":2,"startColumn":2,"endLineNumber":3,"endColumn":1})"));
    EXPECT_EQ(event["changes"][0]["rangeOffset"], 4);
    EXPECT_EQ(event["changes"][0]["rangeLength"], 2);
    EXPECT_EQ(event["changes"][0]["text"], "X");
    EXPECT_EQ(
        event["changes"][1]["range"],
        json::parse(R"({"startLineNumber":1,"startColumn":1,"endLineNumber":1,"endColumn":1})"));
    EXPECT_EQ(event["changes"][1]["text"], "!");
}

TEST_F(DocumentsServiceTest, OffsetsAndTextFollowTheLineEndingOfCrlfDocuments) {
    const core::Uri uri = open("crlf.txt", "ab\r\ncd\r\nef");
    synchronize();
    auto document = service_.find(uri);
    // text "ab\ncd\nef"; replace "d\ne" (bytes 4..7) with "X\nY"
    const auto change = document->apply_edits(std::vector<core::Edit>{{4, 7, "X\nY"}});
    ASSERT_TRUE(change.has_value());
    service_.document_changed(uri, *change);

    const json event = sent().calls()[0].second[1];
    EXPECT_EQ(event["eol"], "\r\n");
    const json& c = event["changes"][0];
    // line 1 (zero-based) column 1: the file offset counts the first "\r\n" as two characters
    EXPECT_EQ(c["rangeOffset"], 5);
    // the removed "d\r\ne" is four characters in the file
    EXPECT_EQ(c["rangeLength"], 4);
    EXPECT_EQ(c["text"], "X\r\nY");
}

TEST_F(DocumentsServiceTest, UndoAndRedoAreFlaggedInTheEvent) {
    const core::Uri uri = open("a.txt", "abc");
    synchronize();
    auto document = service_.find(uri);
    ASSERT_TRUE(document->apply_edits(std::vector<core::Edit>{{3, 3, "d"}}).has_value());
    const auto undone = document->undo();
    service_.document_changed(uri, **undone);
    const auto redone = document->redo();
    service_.document_changed(uri, **redone);

    const auto calls = sent().calls();
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_TRUE(calls[0].second[1]["isUndoing"].get<bool>());
    EXPECT_FALSE(calls[0].second[1]["isRedoing"].get<bool>());
    EXPECT_TRUE(calls[1].second[1]["isRedoing"].get<bool>());
}

TEST_F(DocumentsServiceTest, SavingIsReported) {
    const core::Uri uri = open("a.txt", "abc");
    synchronize();
    service_.document_saved(uri);
    const auto calls = sent().calls();
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].first, "ExtHostDocuments.$acceptModelSaved");
}

TEST_F(DocumentsServiceTest, SelectionChangesAreReportedToTheDocumentsEditors) {
    const core::Uri uri = open("a.txt", "hello");
    const std::string editor = *service_.add_editor(uri);
    synchronize();
    ASSERT_TRUE(service_.find(uri)->set_selections(core::SelectionSet::from({{1, 3}})).has_value());
    service_.selections_changed(uri);

    const auto calls = sent().calls();
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].first, "ExtHostEditors.$acceptEditorPropertiesChanged");
    EXPECT_EQ(calls[0].second[0], editor);
    EXPECT_EQ(calls[0].second[1]["selections"]["selections"][0]["selectionStartColumn"], 2);
    EXPECT_EQ(calls[0].second[1]["selections"]["selections"][0]["positionColumn"], 4);
}

TEST_F(DocumentsServiceTest, EditorsReportTheConfiguredOptionsToExtensions) {
    service_.set_editor_options({.tab_size = 2, .insert_spaces = false});
    const core::Uri uri = open("a.txt", "x");
    const std::string editor = *service_.add_editor(uri);
    const json options = synchronize()["addedEditors"][0]["options"];
    EXPECT_EQ(options["tabSize"], 2);
    EXPECT_EQ(options["indentSize"], 2);
    EXPECT_EQ(options["originalIndentSize"], "tabSize");
    EXPECT_FALSE(options["insertSpaces"].get<bool>());
    EXPECT_EQ(service_.editor_options(), (DocumentsService::EditorOptions{2, false}));
}

TEST_F(DocumentsServiceTest, EditorsKeepTheDefaultOptionsUntilConfigured) {
    const core::Uri uri = open("a.txt", "x");
    service_.add_editor(uri);
    const json options = synchronize()["addedEditors"][0]["options"];
    EXPECT_EQ(options["tabSize"], 4);
    EXPECT_TRUE(options["insertSpaces"].get<bool>());
}

TEST_F(DocumentsServiceTest, ChangingTheOptionsTellsEveryKnownEditor) {
    const core::Uri first = open("a.txt", "x");
    const core::Uri second = open("b.txt", "y");
    const std::string first_editor = *service_.add_editor(first);
    const std::string second_editor = *service_.add_editor(second);
    synchronize();

    service_.set_editor_options({.tab_size = 8, .insert_spaces = false});
    const auto calls = sent().calls();
    ASSERT_EQ(calls.size(), 2u);
    for (const auto& [method, args] : calls) {
        EXPECT_EQ(method, "ExtHostEditors.$acceptEditorPropertiesChanged");
        EXPECT_EQ(args[1]["options"]["tabSize"], 8);
        EXPECT_EQ(args[1]["options"]["indentSize"], 8);
        EXPECT_FALSE(args[1]["options"]["insertSpaces"].get<bool>());
        EXPECT_TRUE(args[1]["selections"].is_null());
        EXPECT_TRUE(args[1]["visibleRanges"].is_null());
    }
    EXPECT_EQ(calls[0].second[0], first_editor);
    EXPECT_EQ(calls[1].second[0], second_editor);
}

TEST_F(DocumentsServiceTest, SettingTheSameOptionsAgainSendsNothing) {
    const core::Uri uri = open("a.txt", "x");
    service_.add_editor(uri);
    synchronize();
    service_.set_editor_options({});
    EXPECT_TRUE(sent().calls().empty());
}

TEST_F(DocumentsServiceTest, OptionsSetBeforeTheInitialDeltaAreSentInIt) {
    const core::Uri uri = open("a.txt", "x");
    service_.add_editor(uri);
    service_.set_editor_options({.tab_size = 3, .insert_spaces = true});
    EXPECT_TRUE(sent().calls().empty());
    EXPECT_EQ(synchronize()["addedEditors"][0]["options"]["tabSize"], 3);
}

// --- requests from extensions ---

TEST_F(DocumentsServiceTest, ApplyEditsChangesTheDocumentAndAnnouncesItBeforeReplying) {
    const core::Uri uri = open("a.txt", "hello world");
    const std::string editor = *service_.add_editor(uri);
    synchronize();

    const json edits = json::parse(
        R"([{"range":{"startLineNumber":1,"startColumn":7,"endLineNumber":1,"endColumn":12},"text":"there"}])");
    const Sent out = request(
        ProxyId::MainThreadTextEditors, "$tryApplyEdits",
        json::array({editor, 1, edits, {{"undoStopBefore", true}, {"undoStopAfter", true}}}));

    EXPECT_EQ(service_.find(uri)->text().to_string(), "hello there");
    EXPECT_EQ(out.reply_to(1), "true");
    // the extension host must see the new text before the reply resolves the edit promise
    EXPECT_LT(out.index_of_call("ExtHostDocuments.$acceptModelChanged"), out.index_of_reply(1));
}

TEST_F(DocumentsServiceTest, ApplyEditsRejectsAStaleVersion) {
    const core::Uri uri = open("a.txt", "hello");
    const std::string editor = *service_.add_editor(uri);
    synchronize();
    ASSERT_TRUE(service_.find(uri)->apply_edits(std::vector<core::Edit>{{5, 5, "!"}}).has_value());

    const json edits = json::parse(
        R"([{"range":{"startLineNumber":1,"startColumn":1,"endLineNumber":1,"endColumn":1},"text":"X"}])");
    const Sent out = request(ProxyId::MainThreadTextEditors, "$tryApplyEdits",
                             json::array({editor, 1, edits, json::object()}));
    EXPECT_EQ(out.reply_to(1), "false");
    EXPECT_EQ(service_.find(uri)->text().to_string(), "hello!");
}

TEST_F(DocumentsServiceTest, ApplyEditsSupportsDeletionAndMultipleEdits) {
    const core::Uri uri = open("a.txt", "abcdef");
    const std::string editor = *service_.add_editor(uri);
    synchronize();
    const json edits = json::parse(R"([
        {"range":{"startLineNumber":1,"startColumn":1,"endLineNumber":1,"endColumn":2},"text":null},
        {"range":{"startLineNumber":1,"startColumn":5,"endLineNumber":1,"endColumn":6},"text":"XY"}])");
    const Sent out = request(ProxyId::MainThreadTextEditors, "$tryApplyEdits",
                             json::array({editor, 1, edits, json::object()}));
    EXPECT_EQ(out.reply_to(1), "true");
    EXPECT_EQ(service_.find(uri)->text().to_string(), "bcdXYf");
}

TEST_F(DocumentsServiceTest, ApplyEditsMovesTheEditorsSelectionsAndReportsThem) {
    const core::Uri uri = open("a.txt", "abc");
    const std::string editor = *service_.add_editor(uri);
    ASSERT_TRUE(service_.find(uri)->set_selections(core::SelectionSet::from({{3, 3}})).has_value());
    synchronize();
    const json edits = json::parse(
        R"([{"range":{"startLineNumber":1,"startColumn":1,"endLineNumber":1,"endColumn":1},"text":"XX"}])");
    const Sent out = request(ProxyId::MainThreadTextEditors, "$tryApplyEdits",
                             json::array({editor, 1, edits, json::object()}));
    EXPECT_NE(out.index_of_call("ExtHostEditors.$acceptEditorPropertiesChanged"),
              out.messages.size());
}

TEST_F(DocumentsServiceTest, ApplyEditsFailsForUnknownEditorsAndInvalidEdits) {
    const core::Uri uri = open("a.txt", "abc");
    const std::string editor = *service_.add_editor(uri);
    synchronize();
    EXPECT_NE(request(ProxyId::MainThreadTextEditors, "$tryApplyEdits",
                      json::array({"nope", 1, json::array(), json::object()}))
                  .reply_to(1)
                  .find("<error"),
              std::string::npos);

    const json overlapping = json::parse(R"([
        {"range":{"startLineNumber":1,"startColumn":1,"endLineNumber":1,"endColumn":3},"text":"a"},
        {"range":{"startLineNumber":1,"startColumn":2,"endLineNumber":1,"endColumn":4},"text":"b"}])");
    EXPECT_EQ(request(ProxyId::MainThreadTextEditors, "$tryApplyEdits",
                      json::array({editor, 1, overlapping, json::object()}), 2)
                  .reply_to(2),
              "false");
    EXPECT_EQ(service_.find(uri)->text().to_string(), "abc");
}

TEST_F(DocumentsServiceTest, SetSelectionsUpdatesTheDocument) {
    const core::Uri uri = open("a.txt", "ab\ncd");
    const std::string editor = *service_.add_editor(uri);
    synchronize();
    const json selections = json::parse(
        R"([{"selectionStartLineNumber":1,"selectionStartColumn":2,"positionLineNumber":2,"positionColumn":2}])");
    const Sent out = request(ProxyId::MainThreadTextEditors, "$trySetSelections",
                             json::array({editor, selections}));
    EXPECT_EQ(out.reply_to(1), "<empty>");
    EXPECT_EQ(service_.find(uri)->selections().primary(), (core::Selection{1, 4}));
}

TEST_F(DocumentsServiceTest, OpenDocumentReturnsAlreadyOpenDocumentsWithoutLoading) {
    const core::Uri uri = open("a.txt", "x");
    synchronize();
    const Sent out =
        request(ProxyId::MainThreadDocuments, "$tryOpenDocument", json::array({uri_to_json(uri)}));
    EXPECT_EQ(json::parse(out.reply_to(1))["path"], uri.path);
    EXPECT_TRUE(out.calls().empty());
}

TEST_F(DocumentsServiceTest, OpenDocumentLoadsFilesAndAnnouncesThemBeforeReplying) {
    const auto path = scratch_ / "disk.txt";
    ASSERT_TRUE(anvil::platform::write_file(path, "from disk\r\nline two").has_value());
    synchronize();

    const core::Uri uri = core::Uri::file(path);
    const Sent out =
        request(ProxyId::MainThreadDocuments, "$tryOpenDocument", json::array({uri_to_json(uri)}));
    ASSERT_NE(service_.find(uri), nullptr);
    EXPECT_EQ(service_.find(uri)->text().to_string(), "from disk\nline two");
    EXPECT_EQ(service_.find(uri)->line_ending(), core::LineEnding::crlf);
    EXPECT_LT(out.index_of_call("ExtHostDocumentsAndEditors.$acceptDocumentsAndEditorsDelta"),
              out.index_of_reply(1));
    const json model = out.calls()[0].second[0]["addedDocuments"][0];
    EXPECT_EQ(model["lines"], json::parse(R"(["from disk","line two"])"));
    EXPECT_EQ(model["EOL"], "\r\n");
}

TEST_F(DocumentsServiceTest, OpenDocumentFailsForMissingFilesNonTextFilesAndOtherSchemes) {
    synchronize();
    const auto fails = [&](const core::Uri& uri) {
        return request(ProxyId::MainThreadDocuments, "$tryOpenDocument",
                       json::array({uri_to_json(uri)}))
                   .reply_to(1)
                   .find("<error") != std::string::npos;
    };
    EXPECT_TRUE(fails(core::Uri::file(scratch_ / "missing.txt")));
    ASSERT_TRUE(
        anvil::platform::write_file(scratch_ / "binary.bin", std::string("\xFF\xFE\x00\x01", 4))
            .has_value());
    EXPECT_TRUE(fails(core::Uri::file(scratch_ / "binary.bin")));
    EXPECT_TRUE(fails(core::Uri{"https", "example.com", "/a", "", ""}));
}

TEST_F(DocumentsServiceTest, SaveDocumentWritesTheFileMarksSavedAndAnnounces) {
    const auto path = scratch_ / "save.txt";
    ASSERT_TRUE(anvil::platform::write_file(path, "a\r\nb").has_value());
    const core::Uri uri = core::Uri::file(path);
    ASSERT_TRUE(service_.add_document(uri, make_document("a\r\nb")).has_value());
    synchronize();
    ASSERT_TRUE(service_.find(uri)->apply_edits(std::vector<core::Edit>{{1, 1, "!"}}).has_value());
    ASSERT_TRUE(service_.find(uri)->is_dirty());

    const Sent out =
        request(ProxyId::MainThreadDocuments, "$trySaveDocument", json::array({uri_to_json(uri)}));
    EXPECT_EQ(out.reply_to(1), "true");
    EXPECT_FALSE(service_.find(uri)->is_dirty());
    EXPECT_EQ(*anvil::platform::read_file(path), "a!\r\nb");
    EXPECT_NE(out.index_of_call("ExtHostDocuments.$acceptModelSaved"), out.messages.size());
}

TEST_F(DocumentsServiceTest, TheObserverLearnsOfExtensionSaves) {
    const auto path = scratch_ / "observed.txt";
    ASSERT_TRUE(anvil::platform::write_file(path, "a").has_value());
    const core::Uri uri = core::Uri::file(path);
    ASSERT_TRUE(service_.add_document(uri, make_document("a")).has_value());
    synchronize();
    std::vector<std::string> saved;
    DocumentsService::Observer observer;
    observer.saved = [&](const core::Uri& u) {
        saved.push_back(u.path);
        EXPECT_FALSE(service_.find(u)->is_dirty()); // already marked saved when observers run
    };
    service_.set_observer(observer);

    ASSERT_TRUE(service_.find(uri)->apply_edits(std::vector<core::Edit>{{1, 1, "!"}}).has_value());
    request(ProxyId::MainThreadDocuments, "$trySaveDocument", json::array({uri_to_json(uri)}));
    EXPECT_EQ(saved, (std::vector<std::string>{uri.path}));

    // a save the editor performs itself is not reported back to it, and neither is a failure
    service_.document_saved(uri);
    request(ProxyId::MainThreadDocuments, "$trySaveDocument",
            json::array({uri_to_json(core::Uri{"untitled", "", "Untitled-1", "", ""})}));
    EXPECT_EQ(saved.size(), 1u);
}

TEST_F(DocumentsServiceTest, SaveDocumentReportsFalseForUnsavableDocuments) {
    synchronize();
    EXPECT_EQ(request(ProxyId::MainThreadDocuments, "$trySaveDocument",
                      json::array({uri_to_json(core::Uri{"untitled", "", "Untitled-1", "", ""})}))
                  .reply_to(1),
              "false");
}

TEST_F(DocumentsServiceTest, CreateDocumentMakesAnUntitledDocument) {
    synchronize();
    const Sent out = request(ProxyId::MainThreadDocuments, "$tryCreateDocument",
                             json::array({{{"language", "python"}, {"content", "print(1)"}}}));
    const json uri = json::parse(out.reply_to(1));
    EXPECT_EQ(uri["scheme"], "untitled");
    EXPECT_EQ(uri["path"], "Untitled-1");
    const json model = out.calls()[0].second[0]["addedDocuments"][0];
    EXPECT_EQ(model["languageId"], "python");
    EXPECT_EQ(model["lines"], json::parse(R"j(["print(1)"])j"));
}

TEST_F(DocumentsServiceTest, ShowTextDocumentOpensAnEditorAndMakesItActive) {
    const core::Uri uri = open("a.txt", "hello\nworld");
    synchronize();
    const json options = {
        {"selection",
         {{"startLineNumber", 2}, {"startColumn", 1}, {"endLineNumber", 2}, {"endColumn", 3}}}};
    const Sent out = request(ProxyId::MainThreadTextEditors, "$tryShowTextDocument",
                             json::array({uri_to_json(uri), options}));
    const std::string id = json::parse(out.reply_to(1)).get<std::string>();
    EXPECT_FALSE(id.empty());
    EXPECT_EQ(service_.find(uri)->selections().primary(), (core::Selection{6, 8}));

    // showing the same document again reuses the editor
    const Sent again = request(ProxyId::MainThreadTextEditors, "$tryShowTextDocument",
                               json::array({uri_to_json(uri), json::object()}), 2);
    EXPECT_EQ(json::parse(again.reply_to(2)).get<std::string>(), id);
}

TEST_F(DocumentsServiceTest, TheObserverLearnsOfExtensionEditsAndShowRequests) {
    const core::Uri uri = open("a.txt", "hello");
    const std::string editor = *service_.add_editor(uri);
    synchronize();
    std::vector<std::string> events;
    DocumentsService::Observer observer;
    observer.edited = [&](const core::Uri& edited, const core::DocumentChange& change) {
        events.push_back("edited " + edited.path + " v" + std::to_string(change.version));
    };
    observer.show_requested = [&](const core::Uri& shown) {
        events.push_back("show " + shown.path);
    };
    service_.set_observer(observer);

    const json edits = json::parse(
        R"([{"range":{"startLineNumber":1,"startColumn":1,"endLineNumber":1,"endColumn":1},"text":"X"}])");
    request(ProxyId::MainThreadTextEditors, "$tryApplyEdits",
            json::array({editor, 1, edits, json::object()}));
    request(ProxyId::MainThreadTextEditors, "$tryShowTextDocument",
            json::array({uri_to_json(uri), json::object()}), 2);
    // edits the editor itself makes are not the observer's business
    const auto own = service_.find(uri)->apply_edits(std::vector<core::Edit>{{0, 0, "!"}});
    service_.document_changed(uri, *own);

    EXPECT_EQ(events, (std::vector<std::string>{"edited " + uri.path + " v2", "show " + uri.path}));
}

TEST_F(DocumentsServiceTest, WorkspaceEditsApplyToEveryDocument) {
    const core::Uri a = open("a.txt", "aaa");
    const core::Uri b = open("b.txt", "bbb");
    synchronize();
    const json dto = {{"edits", json::array({
                                    {{"resource", uri_to_json(a)},
                                     {"versionId", 1},
                                     {"textEdit",
                                      {{"range",
                                        {{"startLineNumber", 1},
                                         {"startColumn", 1},
                                         {"endLineNumber", 1},
                                         {"endColumn", 2}}},
                                       {"text", "X"}}}},
                                    {{"resource", uri_to_json(b)},
                                     {"textEdit",
                                      {{"range",
                                        {{"startLineNumber", 1},
                                         {"startColumn", 4},
                                         {"endLineNumber", 1},
                                         {"endColumn", 4}}},
                                       {"text", "!"}}}},
                                    {{"resource", uri_to_json(a)},
                                     {"textEdit",
                                      {{"range",
                                        {{"startLineNumber", 1},
                                         {"startColumn", 3},
                                         {"endLineNumber", 1},
                                         {"endColumn", 4}}},
                                       {"text", "Y"}}}},
                                })}};
    const Sent out =
        request(ProxyId::MainThreadBulkEdits, "$tryApplyWorkspaceEdit", json::array({dto}));
    EXPECT_EQ(out.reply_to(1), "true");
    EXPECT_EQ(service_.find(a)->text().to_string(), "XaY");
    EXPECT_EQ(service_.find(b)->text().to_string(), "bbb!");
}

TEST_F(DocumentsServiceTest, WorkspaceEditsAreAtomicAcrossDocuments) {
    const core::Uri a = open("a.txt", "aaa");
    const core::Uri b = open("b.txt", "bbb");
    synchronize();
    const json good = {
        {"resource", uri_to_json(a)},
        {"textEdit",
         {{"range",
           {{"startLineNumber", 1}, {"startColumn", 1}, {"endLineNumber", 1}, {"endColumn", 2}}},
          {"text", "X"}}}};
    const json overlapping_one = {
        {"resource", uri_to_json(b)},
        {"textEdit",
         {{"range",
           {{"startLineNumber", 1}, {"startColumn", 1}, {"endLineNumber", 1}, {"endColumn", 3}}},
          {"text", "p"}}}};
    const json overlapping_two = {
        {"resource", uri_to_json(b)},
        {"textEdit",
         {{"range",
           {{"startLineNumber", 1}, {"startColumn", 2}, {"endLineNumber", 1}, {"endColumn", 4}}},
          {"text", "q"}}}};
    const Sent out = request(
        ProxyId::MainThreadBulkEdits, "$tryApplyWorkspaceEdit",
        json::array({json{{"edits", json::array({good, overlapping_one, overlapping_two})}}}));
    EXPECT_EQ(out.reply_to(1), "false");
    EXPECT_EQ(service_.find(a)->text().to_string(), "aaa");
    EXPECT_EQ(service_.find(b)->text().to_string(), "bbb");
}

TEST_F(DocumentsServiceTest, WorkspaceEditsWithStaleVersionsAreRejected) {
    const core::Uri a = open("a.txt", "aaa");
    synchronize();
    const json stale = {{"edits", json::array({text_item(a, 0, 0, "X", 9)})}};
    EXPECT_EQ(request(ProxyId::MainThreadBulkEdits, "$tryApplyWorkspaceEdit", json::array({stale}))
                  .reply_to(1),
              "false");
    EXPECT_EQ(service_.find(a)->text().to_string(), "aaa");
}

TEST_F(DocumentsServiceTest, ACreateWritesTheContentsAndTheMissingParentDirectories) {
    synchronize();
    const auto file = path("deep/er/new.txt");
    EXPECT_EQ(apply_edit({create_item(file, json::object(), "aGVsbG8=")}).reply_to(1), "true");
    EXPECT_EQ(slurp(file), "hello");

    const auto empty = path("empty.txt");
    EXPECT_EQ(apply_edit({create_item(empty)}, 2).reply_to(2), "true");
    EXPECT_EQ(slurp(empty), "");

    const auto folder = path("folder/inner");
    EXPECT_EQ(apply_edit({create_item(folder, {{"folder", true}})}, 3).reply_to(3), "true");
    EXPECT_TRUE(std::filesystem::is_directory(folder));
}

TEST_F(DocumentsServiceTest, ACreateOfAnExistingFileFollowsTheOptions) {
    const auto file = write("a.txt", "old");
    synchronize();
    EXPECT_EQ(apply_edit({create_item(file)}).reply_to(1), "false");
    EXPECT_EQ(slurp(file), "old");

    EXPECT_EQ(apply_edit({create_item(file, {{"ignoreIfExists", true}})}, 2).reply_to(2), "true");
    EXPECT_EQ(slurp(file), "old");

    // An explicit `overwrite` decides, whatever `ignoreIfExists` says.
    EXPECT_EQ(apply_edit({create_item(file, {{"overwrite", false}, {"ignoreIfExists", true}})}, 3)
                  .reply_to(3),
              "false");
    EXPECT_EQ(slurp(file), "old");

    EXPECT_EQ(apply_edit({create_item(file, {{"overwrite", true}}, "bmV3")}, 4).reply_to(4),
              "true");
    EXPECT_EQ(slurp(file), "new");
}

TEST_F(DocumentsServiceTest, ACreateWhereAFileIsInTheWayOfADirectoryIsRefused) {
    const auto file = write("a.txt", "x");
    synchronize();
    EXPECT_EQ(apply_edit({create_item(file / "inner.txt")}).reply_to(1), "false");
    EXPECT_EQ(slurp(file), "x");
}

TEST_F(DocumentsServiceTest, ADeleteRemovesAFileOrWithRecursiveADirectory) {
    const auto file = write("a.txt", "x");
    const auto full = write("dir/inner/b.txt", "y");
    synchronize();
    EXPECT_EQ(apply_edit({delete_item(file)}).reply_to(1), "true");
    EXPECT_FALSE(is_on_disk(file));

    EXPECT_EQ(apply_edit({delete_item(path("dir"))}, 2).reply_to(2), "false");
    EXPECT_TRUE(is_on_disk(full));
    EXPECT_EQ(apply_edit({delete_item(path("dir"), {{"recursive", true}})}, 3).reply_to(3), "true");
    EXPECT_FALSE(is_on_disk(path("dir")));
}

TEST_F(DocumentsServiceTest, ADeleteOfAMissingFileFailsUnlessItIsToBeIgnored) {
    synchronize();
    EXPECT_EQ(apply_edit({delete_item(path("none.txt"))}).reply_to(1), "false");
    EXPECT_EQ(
        apply_edit({delete_item(path("none.txt"), {{"ignoreIfNotExists", true}})}, 2).reply_to(2),
        "true");
}

TEST_F(DocumentsServiceTest, ARenameMovesAFileOrADirectory) {
    const auto file = write("a.txt", "x");
    write("dir/inner.txt", "y");
    synchronize();
    EXPECT_EQ(apply_edit({rename_item(file, path("new/place/b.txt"))}).reply_to(1), "true");
    EXPECT_FALSE(is_on_disk(file));
    EXPECT_EQ(slurp(path("new/place/b.txt")), "x");

    EXPECT_EQ(apply_edit({rename_item(path("dir"), path("moved"))}, 2).reply_to(2), "true");
    EXPECT_FALSE(is_on_disk(path("dir")));
    EXPECT_EQ(slurp(path("moved/inner.txt")), "y");
}

TEST_F(DocumentsServiceTest, ARenameOntoAnExistingFileNeedsOverwrite) {
    const auto from = write("a.txt", "from");
    const auto to = write("b.txt", "to");
    synchronize();
    EXPECT_EQ(apply_edit({rename_item(from, to)}).reply_to(1), "false");
    EXPECT_EQ(slurp(to), "to");
    EXPECT_EQ(apply_edit({rename_item(from, to, {{"ignoreIfExists", true}})}, 2).reply_to(2),
              "true");
    EXPECT_TRUE(is_on_disk(from));
    EXPECT_EQ(slurp(to), "to");
    EXPECT_EQ(apply_edit({rename_item(from, to, {{"overwrite", true}})}, 3).reply_to(3), "true");
    EXPECT_FALSE(is_on_disk(from));
    EXPECT_EQ(slurp(to), "from");
}

TEST_F(DocumentsServiceTest, ARenameOfAMissingFileOrIntoItselfIsRefused) {
    const auto dir = write("dir/a.txt", "x");
    synchronize();
    EXPECT_EQ(apply_edit({rename_item(path("none.txt"), path("b.txt"))}).reply_to(1), "false");
    EXPECT_EQ(apply_edit({rename_item(path("dir"), path("dir/inside"))}, 2).reply_to(2), "false");
    EXPECT_EQ(apply_edit({rename_item(dir, path("dir"), {{"overwrite", true}})}, 3).reply_to(3),
              "false");
    EXPECT_EQ(slurp(dir), "x");
}

TEST_F(DocumentsServiceTest, OperationsLaterInAnEditSeeWhatEarlierOnesDid) {
    write("a.txt", "x");
    synchronize();
    EXPECT_EQ(apply_edit({create_item(path("n.txt")), rename_item(path("n.txt"), path("m.txt")),
                          rename_item(path("a.txt"), path("b.txt")), delete_item(path("b.txt"))})
                  .reply_to(1),
              "true");
    EXPECT_TRUE(is_on_disk(path("m.txt")));
    EXPECT_FALSE(is_on_disk(path("n.txt")));
    EXPECT_FALSE(is_on_disk(path("b.txt")));
}

TEST_F(DocumentsServiceTest, OperationsInsideARenamedDirectoryAreForecastAtTheirNewPlace) {
    write("dir/a.txt", "x");
    synchronize();
    EXPECT_EQ(
        apply_edit({rename_item(path("dir"), path("moved")), delete_item(path("moved/a.txt"))})
            .reply_to(1),
        "true");
    EXPECT_FALSE(is_on_disk(path("moved/a.txt")));
}

TEST_F(DocumentsServiceTest, AnEditWithAnotherSchemeOrAnUnsupportedOptionChangesNothing) {
    synchronize();
    const json foreign = {{"newResource", uri_to_json(core::Uri{"untitled", "", "x", "", ""})}};
    EXPECT_EQ(apply_edit({create_item(path("a.txt")), foreign}).reply_to(1), "false");
    EXPECT_FALSE(is_on_disk(path("a.txt")));

    write("b.txt", "x");
    EXPECT_EQ(
        apply_edit({rename_item(path("b.txt"), path("c.txt"), {{"copy", true}})}, 2).reply_to(2),
        "false");
    EXPECT_FALSE(is_on_disk(path("c.txt")));

    const json transfer =
        create_item(path("d.txt"), {{"contents", {{"type", "dataTransferItem"}, {"id", "1"}}}});
    EXPECT_EQ(apply_edit({transfer}, 3).reply_to(3), "false");
    EXPECT_FALSE(is_on_disk(path("d.txt")));
}

TEST_F(DocumentsServiceTest, NothingChangesWhenAnyPreconditionOfTheEditFails) {
    const auto keep = write("keep.txt", "k");
    synchronize();
    // The delete could not be taken back, so it must not run when a later operation cannot.
    EXPECT_EQ(apply_edit({delete_item(keep), create_item(path("new.txt")),
                          delete_item(path("missing.txt"))})
                  .reply_to(1),
              "false");
    EXPECT_TRUE(is_on_disk(keep));
    EXPECT_FALSE(is_on_disk(path("new.txt")));
}

TEST_F(DocumentsServiceTest, TextEditsApplyAfterTheCreateAndTheRenameBeforeThem) {
    const auto a = write("a.txt", "hello");
    const core::Uri a_uri = core::Uri::file(a);
    ASSERT_TRUE(service_.add_document(a_uri, make_document("hello")).has_value());
    synchronize();
    const core::Uri b_uri = core::Uri::file(path("b.txt"));
    const core::Uri c_uri = core::Uri::file(path("c.txt"));
    ASSERT_EQ(apply_edit({rename_item(a, path("b.txt")), text_item(b_uri, 0, 0, ">>"),
                          create_item(path("c.txt"), json::object(), "YWJj"),
                          text_item(c_uri, 0, 3, "!")})
                  .reply_to(1),
              "true");
    EXPECT_EQ(service_.find(b_uri)->text().to_string(), ">>hello");
    EXPECT_EQ(slurp(path("b.txt")), "hello");
    EXPECT_EQ(service_.find(c_uri)->text().to_string(), "abc!");
}

TEST_F(DocumentsServiceTest, AFailureDuringTheEditUndoesWhatWasDone) {
    const auto a = write("a.txt", "A");
    const core::Uri a_uri = core::Uri::file(a);
    const core::Uri c_uri = core::Uri::file(path("sub/c.txt"));
    ASSERT_TRUE(service_.add_document(a_uri, make_document("A")).has_value());
    const auto document = service_.find(a_uri);
    synchronize();
    // The two edits of the new file overlap, which is known only once the file exists.
    EXPECT_EQ(apply_edit({rename_item(a, path("b.txt")),
                          create_item(path("sub/c.txt"), json::object(), "YWJj"),
                          replace_item(c_uri, 0, 0, 2, "x"), replace_item(c_uri, 0, 1, 3, "y")})
                  .reply_to(1),
              "false");
    EXPECT_EQ(slurp(a), "A");
    EXPECT_FALSE(is_on_disk(path("b.txt")));
    EXPECT_FALSE(is_on_disk(path("sub")));
    EXPECT_EQ(service_.find(a_uri), document);
    EXPECT_EQ(service_.find(c_uri), nullptr);
}

TEST_F(DocumentsServiceTest, AFailureDuringTheEditUndoesTheTextEditsThatWereApplied) {
    write("a.txt", "abc");
    const core::Uri a_uri = core::Uri::file(path("a.txt"));
    ASSERT_TRUE(service_.add_document(a_uri, make_document("abc")).has_value());
    const auto c = path("c.txt");
    const core::Uri c_uri = core::Uri::file(c);
    synchronize();
    EXPECT_EQ(apply_edit({text_item(a_uri, 0, 0, "X"), create_item(c, json::object(), "YWJj"),
                          replace_item(c_uri, 0, 0, 2, "x"), replace_item(c_uri, 0, 1, 3, "y")})
                  .reply_to(1),
              "false");
    EXPECT_EQ(service_.find(a_uri)->text().to_string(), "abc");
    EXPECT_FALSE(service_.find(a_uri)->is_dirty());
    EXPECT_FALSE(is_on_disk(c));
}

TEST_F(DocumentsServiceTest, ARenamedFileKeepsItsDocumentUnderTheNewUri) {
    const auto a = write("a.txt", "hello");
    const core::Uri a_uri = core::Uri::file(a);
    const core::Uri b_uri = core::Uri::file(path("b.txt"));
    ASSERT_TRUE(service_.add_document(a_uri, make_document("hello")).has_value());
    const auto document = service_.find(a_uri);
    ASSERT_TRUE(document->apply_edits(std::vector<core::Edit>{{0, 0, "x"}}).has_value());
    const std::string editor = *service_.add_editor(a_uri);
    service_.set_active_editor(editor);
    synchronize();
    std::vector<std::string> events;
    DocumentsService::Observer observer;
    observer.renamed = [&](const core::Uri& from, const core::Uri& to) {
        events.push_back("renamed " + from.path + " " + to.path);
    };
    observer.deleted = [&](const core::Uri& uri) { events.push_back("deleted " + uri.path); };
    service_.set_observer(observer);

    const Sent out = apply_edit({rename_item(a, path("b.txt"))});
    EXPECT_EQ(out.reply_to(1), "true");
    EXPECT_EQ(service_.find(a_uri), nullptr);
    EXPECT_EQ(service_.find(b_uri), document);
    EXPECT_TRUE(document->is_dirty());
    EXPECT_TRUE(document->can_undo());
    EXPECT_EQ(events, (std::vector<std::string>{"renamed " + a_uri.path + " " + b_uri.path}));

    // The extension host sees the old document close and the new one open, with its editor.
    json removed, added, added_editors;
    for (const auto& [name, args] : out.calls()) {
        if (name != "ExtHostDocumentsAndEditors.$acceptDocumentsAndEditorsDelta") {
            continue;
        }
        const json& delta = args[0];
        if (delta.contains("removedDocuments")) {
            removed = delta["removedDocuments"];
        }
        if (delta.contains("addedDocuments")) {
            added = delta["addedDocuments"];
        }
        if (delta.contains("addedEditors")) {
            added_editors = delta["addedEditors"];
        }
    }
    ASSERT_EQ(removed.size(), 1u);
    EXPECT_EQ(removed[0]["path"], a_uri.path);
    ASSERT_EQ(added.size(), 1u);
    EXPECT_EQ(added[0]["uri"]["path"], b_uri.path);
    EXPECT_EQ(added[0]["lines"], json::parse(R"j(["xhello"])j"));
    EXPECT_TRUE(added[0]["isDirty"].get<bool>());
    ASSERT_EQ(added_editors.size(), 1u);
    EXPECT_EQ(added_editors[0]["documentUri"]["path"], b_uri.path);
    EXPECT_TRUE(service_.editor_for(b_uri).has_value());
}

TEST_F(DocumentsServiceTest, ARenamedDirectoryMovesTheDocumentsBelowIt) {
    const auto x = write("dir/x.txt", "x");
    const auto y = write("dir/sub/y.txt", "y");
    const auto other = write("dir2/z.txt", "z");
    for (const auto& file : {x, y, other}) {
        ASSERT_TRUE(service_.add_document(core::Uri::file(file), make_document("t")).has_value());
    }
    synchronize();
    std::vector<std::string> events;
    DocumentsService::Observer observer;
    observer.renamed = [&](const core::Uri& from, const core::Uri& to) {
        events.push_back(from.path + " -> " + to.path);
    };
    service_.set_observer(observer);

    EXPECT_EQ(apply_edit({rename_item(path("dir"), path("moved"))}).reply_to(1), "true");
    EXPECT_EQ(service_.find(core::Uri::file(x)), nullptr);
    EXPECT_NE(service_.find(core::Uri::file(path("moved/x.txt"))), nullptr);
    EXPECT_NE(service_.find(core::Uri::file(path("moved/sub/y.txt"))), nullptr);
    EXPECT_NE(service_.find(core::Uri::file(other)), nullptr);
    EXPECT_EQ(events.size(), 2u);
}

TEST_F(DocumentsServiceTest, ADeletedFileClosesItsCleanDocumentAndKeepsADirtyOne) {
    const auto clean = write("clean.txt", "c");
    const auto dirty = write("dirty.txt", "d");
    const core::Uri clean_uri = core::Uri::file(clean);
    const core::Uri dirty_uri = core::Uri::file(dirty);
    ASSERT_TRUE(service_.add_document(clean_uri, make_document("c")).has_value());
    ASSERT_TRUE(service_.add_document(dirty_uri, make_document("d")).has_value());
    ASSERT_TRUE(service_.find(dirty_uri)->apply_edits(std::vector<core::Edit>{{1, 1, "!"}}));
    synchronize();
    std::vector<std::string> events;
    DocumentsService::Observer observer;
    observer.deleted = [&](const core::Uri& uri) { events.push_back(uri.path); };
    service_.set_observer(observer);

    const Sent out = apply_edit({delete_item(clean), delete_item(dirty)});
    EXPECT_EQ(out.reply_to(1), "true");
    EXPECT_EQ(service_.find(clean_uri), nullptr);
    ASSERT_NE(service_.find(dirty_uri), nullptr);
    EXPECT_TRUE(service_.find(dirty_uri)->is_dirty());
    EXPECT_EQ(events, (std::vector<std::string>{clean_uri.path}));
    EXPECT_FALSE(is_on_disk(dirty));

    // Saving the document that was kept writes the file again.
    EXPECT_EQ(request(ProxyId::MainThreadDocuments, "$trySaveDocument",
                      json::array({uri_to_json(dirty_uri)}), 2)
                  .reply_to(2),
              "true");
    EXPECT_EQ(slurp(dirty), "d!");
    EXPECT_FALSE(service_.find(dirty_uri)->is_dirty());
}

TEST_F(DocumentsServiceTest, ADeletedDirectoryClosesTheCleanDocumentsBelowIt) {
    const auto x = write("dir/x.txt", "x");
    const core::Uri x_uri = core::Uri::file(x);
    ASSERT_TRUE(service_.add_document(x_uri, make_document("x")).has_value());
    synchronize();
    EXPECT_EQ(apply_edit({delete_item(path("dir"), {{"recursive", true}})}).reply_to(1), "true");
    EXPECT_EQ(service_.find(x_uri), nullptr);
}

TEST_F(DocumentsServiceTest, AnEditThatWouldReplaceADocumentWithUnsavedChangesIsRefused) {
    const auto from = write("a.txt", "from");
    const auto target = write("b.txt", "target");
    const core::Uri target_uri = core::Uri::file(target);
    ASSERT_TRUE(service_.add_document(target_uri, make_document("target")).has_value());
    ASSERT_TRUE(service_.find(target_uri)->apply_edits(std::vector<core::Edit>{{0, 0, "!"}}));
    synchronize();
    EXPECT_EQ(apply_edit({rename_item(from, target, {{"overwrite", true}})}).reply_to(1), "false");
    EXPECT_EQ(apply_edit({create_item(target, {{"overwrite", true}})}, 2).reply_to(2), "false");
    EXPECT_EQ(slurp(from), "from");
    EXPECT_EQ(slurp(target), "target");
    EXPECT_TRUE(service_.find(target_uri)->is_dirty());
}

TEST_F(DocumentsServiceTest, AReplacedFileClosesItsCleanDocument) {
    const auto from = write("a.txt", "from");
    const auto target = write("b.txt", "target");
    const core::Uri from_uri = core::Uri::file(from);
    const core::Uri target_uri = core::Uri::file(target);
    ASSERT_TRUE(service_.add_document(from_uri, make_document("from")).has_value());
    ASSERT_TRUE(service_.add_document(target_uri, make_document("target")).has_value());
    const auto moved = service_.find(from_uri);
    synchronize();
    EXPECT_EQ(apply_edit({rename_item(from, target, {{"overwrite", true}})}).reply_to(1), "true");
    EXPECT_EQ(service_.find(target_uri), moved);
    EXPECT_EQ(service_.find(from_uri), nullptr);
}

TEST_F(DocumentsServiceTest, AFailedEditPutsRenamedDocumentsBack) {
    const auto a = write("a.txt", "A");
    const core::Uri a_uri = core::Uri::file(a);
    const core::Uri b_uri = core::Uri::file(path("b.txt"));
    const core::Uri c_uri = core::Uri::file(path("c.txt"));
    ASSERT_TRUE(service_.add_document(a_uri, make_document("A")).has_value());
    const auto document = service_.find(a_uri);
    synchronize();
    std::vector<std::string> events;
    DocumentsService::Observer observer;
    observer.renamed = [&](const core::Uri& from, const core::Uri& to) {
        events.push_back(from.path + " -> " + to.path);
    };
    service_.set_observer(observer);

    EXPECT_EQ(apply_edit({rename_item(a, path("b.txt")),
                          create_item(path("c.txt"), json::object(), "YWJj"),
                          replace_item(c_uri, 0, 0, 2, "x"), replace_item(c_uri, 0, 1, 3, "y")})
                  .reply_to(1),
              "false");
    EXPECT_EQ(service_.find(a_uri), document);
    EXPECT_EQ(service_.find(b_uri), nullptr);
    EXPECT_EQ(events, (std::vector<std::string>{a_uri.path + " -> " + b_uri.path,
                                                b_uri.path + " -> " + a_uri.path}));
}

TEST_F(DocumentsServiceTest, PropertiesThatTheExtensionHostWritesAsUndefinedAreNotSet) {
    const auto file = write("a.txt", "x");
    synchronize();
    // `undefined` in an object with buffers is the placeholder `{"$$ref$$": -1}`.
    const json undefined = {{"$$ref$$", -1}};
    const json remove = {{"oldResource", uri_to_json(core::Uri::file(file))},
                         {"newResource", undefined},
                         {"metadata", undefined},
                         {"options", {{"contents", undefined}, {"recursive", undefined}}}};
    EXPECT_EQ(apply_edit({remove}).reply_to(1), "true");
    EXPECT_FALSE(is_on_disk(file));

    const auto created = path("b.txt");
    const json create = {{"oldResource", undefined},
                         {"newResource", uri_to_json(core::Uri::file(created))},
                         {"options", {{"overwrite", undefined}, {"ignoreIfExists", true}}}};
    EXPECT_EQ(apply_edit({create}, 2).reply_to(2), "true");
    EXPECT_TRUE(is_on_disk(created));
    EXPECT_EQ(apply_edit({create}, 3).reply_to(3), "true");
}

TEST_F(DocumentsServiceTest, SavingTheDocumentOfADeletedDirectoryMakesTheDirectoryAgain) {
    const auto file = write("dir/sub/x.txt", "x");
    const core::Uri uri = core::Uri::file(file);
    ASSERT_TRUE(service_.add_document(uri, make_document("x")).has_value());
    ASSERT_TRUE(service_.find(uri)->apply_edits(std::vector<core::Edit>{{1, 1, "!"}}));
    synchronize();
    ASSERT_EQ(apply_edit({delete_item(path("dir"), {{"recursive", true}})}).reply_to(1), "true");
    ASSERT_FALSE(is_on_disk(path("dir")));
    EXPECT_EQ(request(ProxyId::MainThreadDocuments, "$trySaveDocument",
                      json::array({uri_to_json(uri)}), 2)
                  .reply_to(2),
              "true");
    EXPECT_EQ(slurp(file), "x!");
}
