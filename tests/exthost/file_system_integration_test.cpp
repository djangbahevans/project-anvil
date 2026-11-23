#include "anvil/exthost/extension_host.hpp"
#include "anvil/platform/file.hpp"

#include <fstream>
#include <gtest/gtest.h>
#include <unistd.h>

using namespace anvil::exthost;
using nlohmann::json;
using std::chrono::milliseconds;
namespace fs = std::filesystem;

namespace {

std::string base64(const std::string& bytes) {
    static const char* alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const unsigned a = static_cast<unsigned char>(bytes[i]);
        const unsigned b = i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i + 1]) : 0;
        const unsigned c = i + 2 < bytes.size() ? static_cast<unsigned char>(bytes[i + 2]) : 0;
        const unsigned triple = (a << 16) | (b << 8) | c;
        result += alphabet[(triple >> 18) & 63];
        result += alphabet[(triple >> 12) & 63];
        result += i + 1 < bytes.size() ? alphabet[(triple >> 6) & 63] : '=';
        result += i + 2 < bytes.size() ? alphabet[triple & 63] : '=';
    }
    return result;
}

} // namespace

// `vscode.workspace.fs` through the real extension host. The extension host serves `file:`
// resources itself for stat, readDirectory, readFile, writeFile, createDirectory and delete, and
// asks the editor for rename, copy, a delete that uses the trash and resources of other schemes,
// so the tests of those three show what the editor's service does and the others show what
// extensions get on the way. Needs the pinned checkout from
// scripts/fetch-vscode.sh and Node.js; the tests skip when either is missing.
class RealFileSystem : public ::testing::Test {
protected:
    void SetUp() override {
        const fs::path vscode_dir = ANVIL_VSCODE_DIR;
        const fs::path node = ANVIL_NODE_EXECUTABLE;
        if (node.empty() || !fs::exists(vscode_dir / "out" / "bootstrap-fork.js")) {
            GTEST_SKIP()
                << "needs Node.js and the transpiled VS Code checkout (scripts/fetch-vscode.sh)";
        }
        scratch_ = fs::temp_directory_path() / ("anvil-fs-it-" + std::to_string(::getpid()));
        fs::remove_all(scratch_);
        fs::create_directories(scratch_ / "files");
        auto started = ExtensionHost::start(ExtensionHostOptions{
            .node_executable = node,
            .vscode_dir = vscode_dir,
            .extension_dirs = {ANVIL_FILESYSTEM_EXTENSION},
            .storage_dir = scratch_ / "storage",
        });
        ASSERT_TRUE(started.has_value()) << started.error().message;
        host_ = std::move(*started);
        ASSERT_TRUE(pump_until([&] { return host_->state() != HostState::starting; }));
        ASSERT_EQ(host_->state(), HostState::ready) << host_->failure();
    }

    void TearDown() override {
        host_.reset();
        std::error_code ignored;
        fs::permissions(scratch_ / "files", fs::perms::all, fs::perm_options::add, ignored);
        fs::remove_all(scratch_, ignored);
    }

    template <class Done> bool pump_until(Done done) {
        const auto deadline = std::chrono::steady_clock::now() + milliseconds(30000);
        while (!done() && std::chrono::steady_clock::now() < deadline) {
            const auto pumped = host_->pump(milliseconds(20));
            if (!pumped || !*pumped) {
                break;
            }
        }
        return done();
    }

    // Runs one of the fixture's commands and returns {ok: ...} or {error: ...}.
    json run(const std::string& command, const json& args) {
        std::optional<CallResult> result;
        host_->commands().execute("anvil.fs." + command, args.dump(),
                                  [&](CallResult r) { result = std::move(r); });
        EXPECT_TRUE(pump_until([&] { return result.has_value(); })) << command;
        if (!result || !result->has_value() || !**result) {
            ADD_FAILURE() << command << " gave no result";
            return json::object();
        }
        return json::parse(***result);
    }

    // The code of the `FileSystemError` that the call failed with, or "" when it succeeded.
    std::string code_of(const json& outcome) {
        if (!outcome.contains("error")) {
            return "(" + outcome.dump() + ")";
        }
        EXPECT_TRUE(outcome["error"]["isFileSystemError"].get<bool>()) << outcome.dump();
        return outcome["error"]["code"].get<std::string>();
    }

    fs::path file(const std::string& name) const {
        return scratch_ / "files" / name;
    }
    void write(const std::string& name, const std::string& contents) {
        fs::create_directories(file(name).parent_path());
        ASSERT_TRUE(anvil::platform::write_file(file(name), contents).has_value());
    }
    std::string contents(const std::string& name) {
        const auto read = anvil::platform::read_file(file(name));
        return read ? *read : "<unreadable>";
    }

    fs::path scratch_;
    std::unique_ptr<ExtensionHost> host_;
};

TEST_F(RealFileSystem, StatReportsTheTypeSizeAndTimesOfFilesAndDirectories) {
    write("a.txt", "hello");
    fs::create_directories(file("d"));
    const json stat = run("stat", json::array({file("a.txt").string()}));
    ASSERT_TRUE(stat.contains("ok")) << stat.dump();
    EXPECT_EQ(stat["ok"]["type"], 1);
    EXPECT_EQ(stat["ok"]["size"], 5);
    EXPECT_GT(stat["ok"]["mtime"].get<std::int64_t>(), 1'600'000'000'000);
    EXPECT_EQ(run("stat", json::array({file("d").string()}))["ok"]["type"], 2);
}

TEST_F(RealFileSystem, ReadDirectoryListsNamesAndTypes) {
    write("d/b.txt", "b");
    fs::create_directories(file("d/a"));
    const json listing = run("readDirectory", json::array({file("d").string()}));
    EXPECT_EQ(listing["ok"], json::parse(R"([["a",2],["b.txt",1]])"));
}

TEST_F(RealFileSystem, FileContentsRoundTripByteForByteInBothDirections) {
    std::string bytes;
    for (int i = 0; i < 256; ++i) {
        bytes.push_back(static_cast<char>(i));
    }
    bytes += std::string(100000, '\xA5');
    write("binary.bin", bytes);
    const json read = run("readFile", json::array({file("binary.bin").string()}));
    ASSERT_TRUE(read.contains("ok")) << read.dump().substr(0, 200);
    EXPECT_EQ(read["ok"].get<std::string>(), base64(bytes));

    const json written =
        run("writeFile", json::array({file("copy/out.bin").string(), base64(bytes)}));
    EXPECT_TRUE(written.contains("ok")) << written.dump();
    EXPECT_EQ(contents("copy/out.bin"), bytes);
}

TEST_F(RealFileSystem, AnEmptyFileReadsAsAnEmptyBufferAndAnEmptyBufferWritesAnEmptyFile) {
    write("empty", "");
    EXPECT_EQ(run("readFile", json::array({file("empty").string()}))["ok"], "");
    EXPECT_TRUE(run("writeFile", json::array({file("made").string(), ""})).contains("ok"));
    EXPECT_TRUE(fs::exists(file("made")));
    EXPECT_EQ(fs::file_size(file("made")), 0U);
}

TEST_F(RealFileSystem, TheFailuresOfFileOperationsAreFileSystemErrorsWithTheRightCode) {
    write("a.txt", "a");
    fs::create_directories(file("d"));
    EXPECT_EQ(code_of(run("stat", json::array({file("missing").string()}))), "FileNotFound");
    EXPECT_EQ(code_of(run("readFile", json::array({file("missing").string()}))), "FileNotFound");
    EXPECT_EQ(code_of(run("readFile", json::array({file("d").string()}))), "FileIsADirectory");
    EXPECT_EQ(code_of(run("readDirectory", json::array({file("a.txt").string()}))),
              "FileNotADirectory");
    EXPECT_EQ(code_of(run("writeFile", json::array({file("d").string(), ""}))), "FileIsADirectory");
    EXPECT_EQ(code_of(run("createDirectory", json::array({file("a.txt").string()}))), "FileExists");
    EXPECT_EQ(code_of(run("delete", json::array({file("missing").string(), true, true}))),
              "FileNotFound");
    EXPECT_EQ(
        code_of(run("rename", json::array({file("a.txt").string(), file("d").string(), false}))),
        "FileExists");
    EXPECT_EQ(
        code_of(run("copy", json::array({file("missing").string(), file("x").string(), false}))),
        "FileNotFound");
}

TEST_F(RealFileSystem, APermissionProblemIsNoPermissions) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "the permissions of the super user do not stop it";
    }
    write("secret.txt", "x");
    fs::permissions(file("secret.txt"), fs::perms::none);
    EXPECT_EQ(code_of(run("readFile", json::array({file("secret.txt").string()}))),
              "NoPermissions");
}

TEST_F(RealFileSystem, CreateDeleteRenameAndCopyChangeTheFileSystem) {
    EXPECT_TRUE(run("createDirectory", json::array({file("a/b/c").string()})).contains("ok"));
    EXPECT_TRUE(fs::is_directory(file("a/b/c")));
    EXPECT_TRUE(run("createDirectory", json::array({file("a/b/c").string()})).contains("ok"));

    write("a/b/f.txt", "f");
    EXPECT_TRUE(run("rename",
                    json::array({file("a/b/f.txt").string(), file("moved/g.txt").string(), false}))
                    .contains("ok"));
    EXPECT_EQ(contents("moved/g.txt"), "f");
    EXPECT_FALSE(fs::exists(file("a/b/f.txt")));

    EXPECT_TRUE(run("copy", json::array({file("moved").string(), file("copied").string(), false}))
                    .contains("ok"));
    EXPECT_EQ(contents("copied/g.txt"), "f");
    EXPECT_EQ(contents("moved/g.txt"), "f");

    write("moved/g.txt", "changed");
    EXPECT_TRUE(run("copy", json::array({file("moved/g.txt").string(),
                                         file("copied/g.txt").string(), true}))
                    .contains("ok"));
    EXPECT_EQ(contents("copied/g.txt"), "changed");

    EXPECT_EQ(code_of(run("delete", json::array({file("a").string(), false, true}))), "Unknown");
    EXPECT_TRUE(fs::exists(file("a/b/c")));
    EXPECT_TRUE(run("delete", json::array({file("a").string(), true, true})).contains("ok"));
    EXPECT_FALSE(fs::exists(file("a")));
}

TEST_F(RealFileSystem, AResourceOfAnotherSchemeIsUnavailable) {
    const json outcome = run("statUri", json::array({"memfs:/some/file.txt"}));
    EXPECT_EQ(code_of(outcome), "Unavailable");
}
