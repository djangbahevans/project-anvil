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

class DocumentsServiceTest : public ::testing::Test {
protected:
    DocumentsServiceTest() : session_("{}"), host_(session_), service_(session_, router_) {
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

    // Sends a request from the extension host and returns everything the service wrote back.
    Sent request(ProxyId service, std::string method, json args, std::uint32_t req = 1) {
        EXPECT_TRUE(host_
                        .send_rpc(Request{req, static_cast<std::uint8_t>(service),
                                          std::move(method), JsonArgs{args.dump()}})
                        .has_value());
        return sent();
    }

    Session session_;
    FakeHost host_;
    RequestRouter router_;
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

TEST_F(DocumentsServiceTest, ALanguageResolverChoosesTheLanguageId) {
    service_.set_language_resolver(
        [](const core::Uri& uri) { return uri.path.ends_with(".md") ? "markdown" : "plaintext"; });
    open("readme.md", "# hi");
    EXPECT_EQ(synchronize()["addedDocuments"][0]["languageId"], "markdown");
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

TEST_F(DocumentsServiceTest, WorkspaceEditsWithStaleVersionsOrFileOperationsAreRejected) {
    const core::Uri a = open("a.txt", "aaa");
    synchronize();
    const json range = {
        {"startLineNumber", 1}, {"startColumn", 1}, {"endLineNumber", 1}, {"endColumn", 1}};
    const json stale = {
        {"edits", json::array({{{"resource", uri_to_json(a)},
                                {"versionId", 9},
                                {"textEdit", {{"range", range}, {"text", "X"}}}}})}};
    EXPECT_EQ(request(ProxyId::MainThreadBulkEdits, "$tryApplyWorkspaceEdit", json::array({stale}))
                  .reply_to(1),
              "false");

    const json rename = {
        {"edits",
         json::array({{{"oldResource", uri_to_json(a)},
                       {"newResource", uri_to_json(core::Uri::file(scratch_ / "z.txt"))}}})}};
    EXPECT_EQ(
        request(ProxyId::MainThreadBulkEdits, "$tryApplyWorkspaceEdit", json::array({rename}), 2)
            .reply_to(2),
        "false");
    EXPECT_EQ(service_.find(a)->text().to_string(), "aaa");
}
