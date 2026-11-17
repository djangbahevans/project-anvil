#include "anvil/exthost/extension_host.hpp"
#include "anvil/exthost/vscode_types.hpp"
#include "anvil/platform/file.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

using namespace anvil::exthost;
using nlohmann::json;
using std::chrono::milliseconds;
namespace core = anvil::core;

// Extensions reading and editing documents through the real extension host. Needs the pinned
// checkout from scripts/fetch-vscode.sh and Node.js; the tests skip when either is missing.
class RealDocuments : public ::testing::Test {
protected:
    void SetUp() override {
        const std::filesystem::path vscode_dir = ANVIL_VSCODE_DIR;
        node_ = ANVIL_NODE_EXECUTABLE;
        if (node_.empty() || !std::filesystem::exists(vscode_dir / "out" / "bootstrap-fork.js")) {
            GTEST_SKIP()
                << "needs Node.js and the transpiled VS Code checkout (scripts/fetch-vscode.sh)";
        }
        vscode_dir_ = vscode_dir;
        scratch_ = std::filesystem::temp_directory_path() /
                   ("anvil-docs-it-" + std::to_string(::getpid()));
        std::filesystem::create_directories(scratch_);
    }

    void TearDown() override {
        host_.reset();
        std::filesystem::remove_all(scratch_);
    }

    // Writes `bytes` to a file, shows it in an active editor, and starts the extension host.
    core::Uri start_with_active_file(std::string_view bytes, const std::string& name = "a.txt") {
        const auto path = scratch_ / name;
        EXPECT_TRUE(anvil::platform::write_file(path, bytes).has_value());
        auto started = ExtensionHost::start(ExtensionHostOptions{
            .node_executable = node_,
            .vscode_dir = vscode_dir_,
            .extension_dirs = {ANVIL_DOCUMENTS_EXTENSION},
            .storage_dir = scratch_ / "storage",
        });
        EXPECT_TRUE(started.has_value()) << (started ? "" : started.error().message);
        host_ = std::move(*started);

        const core::Uri uri = core::Uri::file(path);
        document_ = std::make_shared<core::Document>(*core::Document::from_file_bytes(bytes));
        EXPECT_TRUE(host_->documents().add_document(uri, document_).has_value());
        editor_ = *host_->documents().add_editor(uri);
        host_->documents().set_active_editor(editor_);

        EXPECT_TRUE(pump_until([&] { return host_->state() != HostState::starting; }));
        EXPECT_EQ(host_->state(), HostState::ready) << host_->failure();
        return uri;
    }

    template <class Done> bool pump_until(Done done) {
        const auto deadline = std::chrono::steady_clock::now() + milliseconds(30000);
        while (!done() && std::chrono::steady_clock::now() < deadline) {
            const auto pumped = host_->pump(milliseconds(20));
            if (!pumped) {
                ADD_FAILURE() << pumped.error().message;
                return false;
            }
            if (!*pumped) {
                break;
            }
        }
        return done();
    }

    // Runs an extension command with `args` and returns its result as JSON.
    json run(const std::string& command, const json& args = json::array()) {
        std::optional<CallResult> result;
        host_->commands().execute(command, args.dump(),
                                  [&](CallResult r) { result = std::move(r); });
        EXPECT_TRUE(pump_until([&] { return result.has_value(); })) << "no result from " << command;
        if (!result || !result->has_value()) {
            ADD_FAILURE() << command << " failed: " << (result ? result->error() : "timed out");
            return nullptr;
        }
        const std::optional<std::string>& value = **result;
        return value ? json::parse(*value) : json(nullptr);
    }

    // The editor changed the document: report it to the extension host.
    void editor_edits(const core::Uri& uri, std::vector<core::Edit> edits) {
        auto change = document_->apply_edits(edits);
        ASSERT_TRUE(change.has_value()) << change.error().message;
        host_->documents().document_changed(uri, *change);
    }

    // Pumps until `condition` holds; for effects of calls the extension does not wait for.
    template <class Condition> void eventually(Condition condition) {
        EXPECT_TRUE(pump_until(condition)) << "condition not reached in time";
    }

    std::filesystem::path node_;
    std::filesystem::path vscode_dir_;
    std::filesystem::path scratch_;
    std::unique_ptr<ExtensionHost> host_;
    std::shared_ptr<core::Document> document_;
    std::string editor_;
};

TEST_F(RealDocuments, ExtensionsSeeTheActiveDocumentAtStartup) {
    const core::Uri uri = start_with_active_file("hello\nworld");
    const json active = run("anvil.docs.active");
    EXPECT_EQ(active["uri"], uri.to_string());
    EXPECT_EQ(active["text"], "hello\nworld");
    EXPECT_EQ(active["lineCount"], 2);
    EXPECT_EQ(active["languageId"], "plaintext");
    EXPECT_EQ(active["version"], 1);
    EXPECT_FALSE(active["isDirty"].get<bool>());
    EXPECT_EQ(active["eol"], "lf");
    EXPECT_EQ(active["selections"], json::parse("[[0,0,0,0]]"));
}

TEST_F(RealDocuments, CrlfDocumentsKeepTheirLineEndingInExtensions) {
    start_with_active_file("ab\r\ncd");
    const json active = run("anvil.docs.active");
    EXPECT_EQ(active["eol"], "crlf");
    EXPECT_EQ(active["text"], "ab\r\ncd");
    EXPECT_EQ(active["lineCount"], 2);
}

TEST_F(RealDocuments, EditsMadeInTheEditorReachExtensionsWithExactRanges) {
    const core::Uri uri = start_with_active_file("hello\nworld");
    editor_edits(uri, {{6, 11, "there"}, {0, 0, ">> "}});

    const json active = run("anvil.docs.active");
    EXPECT_EQ(active["text"], ">> hello\nthere");
    EXPECT_EQ(active["version"], 2);
    EXPECT_TRUE(active["isDirty"].get<bool>());

    const json events = run("anvil.docs.changes");
    ASSERT_EQ(events.size(), 1u);
    ASSERT_EQ(events[0]["contentChanges"].size(), 2u);
    const json& first = events[0]["contentChanges"][0];
    EXPECT_EQ(first["start"], json::parse("[1,0]"));
    EXPECT_EQ(first["end"], json::parse("[1,5]"));
    EXPECT_EQ(first["rangeOffset"], 6);
    EXPECT_EQ(first["rangeLength"], 5);
    EXPECT_EQ(first["text"], "there");
}

TEST_F(RealDocuments, OffsetsInCrlfDocumentsCountTheFullLineEnding) {
    const core::Uri uri = start_with_active_file("ab\r\ncd");
    editor_edits(uri, {{4, 5, "X"}}); // the "d" of the normalized text "ab\ncd"
    const json events = run("anvil.docs.changes");
    ASSERT_EQ(events.size(), 1u);
    const json& change = events[0]["contentChanges"][0];
    EXPECT_EQ(change["start"], json::parse("[1,1]"));
    EXPECT_EQ(change["end"], json::parse("[1,2]"));
    EXPECT_EQ(change["rangeOffset"], 5); // "ab\r\nc" before it
    EXPECT_EQ(change["rangeLength"], 1);
    EXPECT_EQ(run("anvil.docs.active")["text"], "ab\r\ncX");
}

TEST_F(RealDocuments, UndoIsReportedWithItsReason) {
    const core::Uri uri = start_with_active_file("abc");
    editor_edits(uri, {{3, 3, "d"}});
    auto undone = document_->undo();
    ASSERT_TRUE(undone.has_value() && undone->has_value());
    host_->documents().document_changed(uri, **undone);

    EXPECT_EQ(run("anvil.docs.active")["text"], "abc");
    const json events = run("anvil.docs.changes");
    ASSERT_EQ(events.size(), 2u);
    EXPECT_TRUE(events[0]["reason"].is_null());
    EXPECT_EQ(events[1]["reason"], 1); // TextDocumentChangeReason.Undo
}

TEST_F(RealDocuments, ExtensionEditsApplyToTheDocumentAndComeBackAsEvents) {
    start_with_active_file("hello\nworld");
    EXPECT_TRUE(run("anvil.docs.insert", json::array({"Hi "})).get<bool>());
    EXPECT_EQ(document_->text().to_string(), "Hi hello\nworld");
    EXPECT_TRUE(document_->is_dirty());

    const json active = run("anvil.docs.active");
    EXPECT_EQ(active["text"], "Hi hello\nworld");
    EXPECT_EQ(active["version"], 2);
    const json events = run("anvil.docs.changes");
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0]["contentChanges"][0]["text"], "Hi ");
}

TEST_F(RealDocuments, ExtensionsCanApplySeveralEditsAtOnce) {
    start_with_active_file("hello\nworld");
    EXPECT_TRUE(run("anvil.docs.replaceTwo").get<bool>());
    EXPECT_EQ(document_->text().to_string(), "Xello\norld");
    document_->undo();
    EXPECT_EQ(document_->text().to_string(), "hello\nworld"); // one undo step for the whole batch
}

TEST_F(RealDocuments, SelectionsFlowInBothDirections) {
    const core::Uri uri = start_with_active_file("hello\nworld");
    ASSERT_TRUE(document_->set_selections(core::SelectionSet::from({{1, 3}})).has_value());
    host_->documents().selections_changed(uri);
    eventually(
        [&] { return run("anvil.docs.active")["selections"] == json::parse("[[0,1,0,3]]"); });

    EXPECT_TRUE(run("anvil.docs.select", json::array({1, 1, 1, 4})).get<bool>());
    eventually([&] { return document_->selections().primary() == core::Selection{7, 10}; });
}

TEST_F(RealDocuments, OpenTextDocumentLoadsFilesFromDisk) {
    start_with_active_file("main");
    const auto other = scratch_ / "other.txt";
    ASSERT_TRUE(anvil::platform::write_file(other, "line1\r\nline2\r\nline3").has_value());

    const json opened = run("anvil.docs.open", json::array({other.string()}));
    EXPECT_EQ(opened["text"], "line1\r\nline2\r\nline3");
    EXPECT_EQ(opened["lineCount"], 3);
    EXPECT_EQ(opened["eol"], "crlf");
    ASSERT_NE(host_->documents().find(core::Uri::file(other)), nullptr);
    EXPECT_EQ(host_->documents().find(core::Uri::file(other))->text().to_string(),
              "line1\nline2\nline3");
}

TEST_F(RealDocuments, ShowTextDocumentMakesTheOpenedDocumentActive) {
    start_with_active_file("main");
    const auto other = scratch_ / "other.txt";
    ASSERT_TRUE(anvil::platform::write_file(other, "second").has_value());
    const json shown = run("anvil.docs.show", json::array({other.string()}));
    EXPECT_EQ(shown["shown"], core::Uri::file(other).to_string());
    EXPECT_EQ(shown["active"], core::Uri::file(other).to_string());
}

TEST_F(RealDocuments, SavingFromAnExtensionWritesTheFileAndCleansTheDocument) {
    const core::Uri uri = start_with_active_file("a\r\nb");
    editor_edits(uri, {{1, 1, "!"}});
    ASSERT_TRUE(document_->is_dirty());

    run("anvil.docs.save");
    EXPECT_FALSE(document_->is_dirty());
    EXPECT_EQ(*anvil::platform::read_file(*uri.file_path()), "a!\r\nb");
    eventually([&] { return !run("anvil.docs.active")["isDirty"].get<bool>(); });
    EXPECT_EQ(run("anvil.docs.saves").size(), 1u);
}

TEST_F(RealDocuments, WorkspaceEditsFromExtensionsApplyToOpenAndUnopenedFiles) {
    const core::Uri uri = start_with_active_file("text");
    EXPECT_TRUE(
        run("anvil.docs.workspaceEdit", json::array({uri.file_path()->string()})).get<bool>());
    EXPECT_EQ(document_->text().to_string(), ">>text");

    const auto other = scratch_ / "other.txt";
    ASSERT_TRUE(anvil::platform::write_file(other, "body").has_value());
    EXPECT_TRUE(run("anvil.docs.workspaceEdit", json::array({other.string()})).get<bool>());
    EXPECT_EQ(host_->documents().find(core::Uri::file(other))->text().to_string(), ">>body");
}
