#include "anvil/exthost/file_system_service.hpp"
#include "anvil/exthost/vscode_types.hpp"
#include "anvil/platform/file.hpp"
#include "fake_host.hpp"

#include <gtest/gtest.h>
#include <unistd.h>

using namespace anvil::exthost;
using anvil::Bytes;
using anvil::exthost::test::FakeHost;
using nlohmann::json;
namespace fs = std::filesystem;
namespace core = anvil::core;

namespace {

class FileSystemServiceTest : public ::testing::Test {
protected:
    FileSystemServiceTest() : session_("{}"), host_(session_), service_(router_) {
        session_.set_request_handler(
            [this](const Request& request) { return router_.dispatch(request); });
        host_.complete_handshake();
        directory_ =
            fs::temp_directory_path() / ("anvil-fs-service-test-" + std::to_string(::getpid()));
        fs::remove_all(directory_);
        fs::create_directories(directory_);
    }
    ~FileSystemServiceTest() override {
        std::error_code ignored;
        fs::permissions(directory_, fs::perms::all, fs::perm_options::add, ignored);
        for (const auto& entry : fs::recursive_directory_iterator(directory_, ignored)) {
            fs::permissions(entry.path(), fs::perms::all, fs::perm_options::add, ignored);
        }
        fs::remove_all(directory_, ignored);
    }

    json uri(const fs::path& path) {
        return uri_to_json(core::Uri::file(path));
    }

    // What the session sent for a request: the acknowledgement and then the reply.
    RpcMessage send(const std::string& method, MixedArgs args) {
        const Request request{next_req_, static_cast<std::uint8_t>(ProxyId::MainThreadFileSystem),
                              method, std::move(args)};
        EXPECT_TRUE(host_.send_rpc(request).has_value());
        auto messages = host_.drain_rpc();
        EXPECT_EQ(messages.size(), 2U) << method;
        ++next_req_;
        if (messages.size() != 2) {
            return EmptyReply{0};
        }
        EXPECT_EQ(messages[0], RpcMessage{Acknowledged{request.req}});
        return messages[1];
    }
    RpcMessage call(const std::string& method, const json& args) {
        const Request request{next_req_, static_cast<std::uint8_t>(ProxyId::MainThreadFileSystem),
                              method, JsonArgs{args.dump()}};
        EXPECT_TRUE(host_.send_rpc(request).has_value());
        auto messages = host_.drain_rpc();
        EXPECT_EQ(messages.size(), 2U) << method;
        ++next_req_;
        return messages.size() == 2 ? messages[1] : RpcMessage{EmptyReply{0}};
    }

    static json result_of(const RpcMessage& reply) {
        EXPECT_TRUE(std::holds_alternative<JsonReply>(reply));
        return std::holds_alternative<JsonReply>(reply)
                   ? json::parse(std::get<JsonReply>(reply).json)
                   : json();
    }
    static json error_of(const RpcMessage& reply) {
        EXPECT_TRUE(std::holds_alternative<ErrorReply>(reply));
        return std::holds_alternative<ErrorReply>(reply)
                   ? json::parse(std::get<ErrorReply>(reply).json)
                   : json();
    }
    static std::string name_of(const RpcMessage& reply) {
        return error_of(reply).value("name", "");
    }
    static bool is_empty_reply(const RpcMessage& reply) {
        return std::holds_alternative<EmptyReply>(reply);
    }

    fs::path write(const std::string& name, const std::string& contents = "x") {
        const fs::path path = directory_ / name;
        fs::create_directories(path.parent_path());
        EXPECT_TRUE(anvil::platform::write_file(path, contents).has_value());
        return path;
    }
    static std::string contents(const fs::path& path) {
        const auto read = anvil::platform::read_file(path);
        return read ? *read : "<unreadable>";
    }

    Session session_;
    FakeHost host_;
    RequestRouter router_;
    FileSystemService service_;
    fs::path directory_;
    std::uint32_t next_req_ = 1;
};

} // namespace

// --- stat ---

TEST_F(FileSystemServiceTest, StatGivesTheTypeTheTimesAndTheSizeOfAFile) {
    const fs::path path = write("a.txt", "hello");
    const json stat = result_of(call("$stat", json::array({uri(path)})));
    EXPECT_EQ(stat.at("type"), 1);
    EXPECT_EQ(stat.at("size"), 5);
    EXPECT_GT(stat.at("mtime").get<std::int64_t>(), 1'600'000'000'000);
    EXPECT_GT(stat.at("ctime").get<std::int64_t>(), 1'600'000'000'000);
    EXPECT_FALSE(stat.contains("permissions"));
}

TEST_F(FileSystemServiceTest, StatOfADirectoryALinkAndAReadOnlyFile) {
    const fs::path file = write("d/a.txt");
    fs::create_symlink(file, directory_ / "link");
    fs::create_symlink(directory_ / "nothing", directory_ / "broken");
    EXPECT_EQ(result_of(call("$stat", json::array({uri(directory_ / "d")}))).at("type"), 2);
    EXPECT_EQ(result_of(call("$stat", json::array({uri(directory_ / "link")}))).at("type"), 65);
    EXPECT_EQ(result_of(call("$stat", json::array({uri(directory_ / "broken")}))).at("type"), 64);
    fs::permissions(file, fs::perms::owner_read, fs::perm_options::replace);
    EXPECT_EQ(result_of(call("$stat", json::array({uri(file)}))).at("permissions"), 1);
}

TEST_F(FileSystemServiceTest, StatOfAMissingFileIsEntryNotFoundAndNamesTheFile) {
    const json error = error_of(call("$stat", json::array({uri(directory_ / "missing.txt")})));
    EXPECT_TRUE(error.at("$isError").get<bool>());
    EXPECT_EQ(error.at("name"), "EntryNotFound");
    EXPECT_NE(error.at("message").get<std::string>().find("missing.txt"), std::string::npos);
}

// --- readdir ---

TEST_F(FileSystemServiceTest, ReaddirGivesTheNamesAndTypesSortedByName) {
    write("d/b.txt");
    fs::create_directories(directory_ / "d" / "a");
    EXPECT_EQ(result_of(call("$readdir", json::array({uri(directory_ / "d")}))),
              json::parse(R"([["a",2],["b.txt",1]])"));
}

TEST_F(FileSystemServiceTest, ReaddirOfAFileIsEntryNotADirectoryAndOfAMissingOneEntryNotFound) {
    const fs::path file = write("a.txt");
    EXPECT_EQ(name_of(call("$readdir", json::array({uri(file)}))), "EntryNotADirectory");
    EXPECT_EQ(name_of(call("$readdir", json::array({uri(directory_ / "missing")}))),
              "EntryNotFound");
}

TEST_F(FileSystemServiceTest, ANameThatIsNotUtf8CannotBeListedAndSaysSo) {
    write(std::string("d/bad-\xFF-name"));
    const json error = error_of(call("$readdir", json::array({uri(directory_ / "d")})));
    EXPECT_EQ(error.at("name"), "Unknown");
    EXPECT_NE(error.at("message").get<std::string>().find("UTF-8"), std::string::npos);
}

// --- readFile ---

TEST_F(FileSystemServiceTest, ReadFileRepliesWithTheExactBytesAsABuffer) {
    const std::string bytes("a\0b\xFF\xFE\r\n\x80", 8);
    const fs::path path = write("a.bin", bytes);
    const RpcMessage reply = call("$readFile", json::array({uri(path)}));
    ASSERT_TRUE(std::holds_alternative<BufferReply>(reply));
    EXPECT_EQ(anvil::as_text(std::get<BufferReply>(reply).data), bytes);
}

TEST_F(FileSystemServiceTest, ReadFileOfAnEmptyFileIsAnEmptyBufferAndNotAnEmptyReply) {
    const RpcMessage reply = call("$readFile", json::array({uri(write("empty", ""))}));
    ASSERT_TRUE(std::holds_alternative<BufferReply>(reply));
    EXPECT_TRUE(std::get<BufferReply>(reply).data.empty());
}

TEST_F(FileSystemServiceTest,
       ReadFileOfADirectoryIsEntryIsADirectoryAndOfAMissingFileEntryNotFound) {
    fs::create_directories(directory_ / "d");
    EXPECT_EQ(name_of(call("$readFile", json::array({uri(directory_ / "d")}))),
              "EntryIsADirectory");
    EXPECT_EQ(name_of(call("$readFile", json::array({uri(directory_ / "missing")}))),
              "EntryNotFound");
}

TEST_F(FileSystemServiceTest, ReadFileOfAFileThatCannotBeReadIsNoPermissions) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "the permissions of the super user do not stop it";
    }
    const fs::path path = write("secret");
    fs::permissions(path, fs::perms::none, fs::perm_options::replace);
    EXPECT_EQ(name_of(call("$readFile", json::array({uri(path)}))), "NoPermissions");
}

// --- writeFile ---

TEST_F(FileSystemServiceTest, WriteFileWritesTheExactBytesAndRepliesEmpty) {
    const std::string bytes("a\0b\xFF\r\n", 6);
    const fs::path path = directory_ / "out.bin";
    const RpcMessage reply =
        send("$writeFile", {JsonArg{uri(path).dump()}, BufferArg{anvil::to_bytes(bytes)}});
    EXPECT_TRUE(is_empty_reply(reply));
    EXPECT_EQ(contents(path), bytes);
}

TEST_F(FileSystemServiceTest, WriteFileReplacesAnExistingFileAndCreatesMissingParents) {
    const fs::path path = write("a.txt", "old contents that are longer");
    EXPECT_TRUE(is_empty_reply(
        send("$writeFile", {JsonArg{uri(path).dump()}, BufferArg{anvil::to_bytes("new")}})));
    EXPECT_EQ(contents(path), "new");

    const fs::path nested = directory_ / "x" / "y" / "z.txt";
    EXPECT_TRUE(is_empty_reply(
        send("$writeFile", {JsonArg{uri(nested).dump()}, BufferArg{anvil::to_bytes("deep")}})));
    EXPECT_EQ(contents(nested), "deep");
}

TEST_F(FileSystemServiceTest, WriteFileWritesThroughALinkAndKeepsTheLink) {
    const fs::path target = write("target.txt", "old");
    fs::create_symlink(target, directory_ / "link.txt");
    EXPECT_TRUE(is_empty_reply(send("$writeFile", {JsonArg{uri(directory_ / "link.txt").dump()},
                                                   BufferArg{anvil::to_bytes("through")}})));
    EXPECT_TRUE(fs::is_symlink(directory_ / "link.txt"));
    EXPECT_EQ(contents(target), "through");
}

TEST_F(FileSystemServiceTest, WriteFileOverADirectoryIsEntryIsADirectoryAndWithoutABufferFails) {
    fs::create_directories(directory_ / "d");
    EXPECT_EQ(name_of(send("$writeFile", {JsonArg{uri(directory_ / "d").dump()},
                                          BufferArg{anvil::to_bytes("x")}})),
              "EntryIsADirectory");
    const json error =
        error_of(send("$writeFile", {JsonArg{uri(directory_ / "e").dump()}, JsonArg{"1"}}));
    EXPECT_NE(error.at("message").get<std::string>().find("buffer"), std::string::npos);
    EXPECT_FALSE(fs::exists(directory_ / "e"));
}

TEST_F(FileSystemServiceTest, WriteFileWhereAParentIsAFileIsEntryNotADirectory) {
    write("a");
    EXPECT_EQ(name_of(send("$writeFile", {JsonArg{uri(directory_ / "a" / "b").dump()},
                                          BufferArg{anvil::to_bytes("x")}})),
              "EntryNotADirectory");
}

// --- mkdir, delete, rename, copy ---

TEST_F(FileSystemServiceTest, MkdirCreatesParentsAndAcceptsAnExistingDirectory) {
    EXPECT_TRUE(is_empty_reply(call("$mkdir", json::array({uri(directory_ / "a" / "b")}))));
    EXPECT_TRUE(fs::is_directory(directory_ / "a" / "b"));
    EXPECT_TRUE(is_empty_reply(call("$mkdir", json::array({uri(directory_ / "a" / "b")}))));
}

TEST_F(FileSystemServiceTest, MkdirOverAFileIsEntryExists) {
    write("a");
    EXPECT_EQ(name_of(call("$mkdir", json::array({uri(directory_ / "a")}))), "EntryExists");
}

TEST_F(FileSystemServiceTest, DeleteRemovesAFileAndADirectoryWithTheRecursiveOption) {
    const fs::path file = write("a.txt");
    write("d/inner.txt");
    EXPECT_TRUE(is_empty_reply(
        call("$delete", json::array({uri(file), {{"recursive", false}, {"useTrash", false}}}))));
    EXPECT_FALSE(fs::exists(file));

    EXPECT_EQ(name_of(call("$delete", json::array({uri(directory_ / "d"),
                                                   {{"recursive", false}, {"useTrash", false}}}))),
              "Unknown");
    EXPECT_TRUE(fs::exists(directory_ / "d" / "inner.txt"));
    EXPECT_TRUE(is_empty_reply(
        call("$delete",
             json::array({uri(directory_ / "d"), {{"recursive", true}, {"useTrash", true}}}))));
    EXPECT_FALSE(fs::exists(directory_ / "d"));
}

TEST_F(FileSystemServiceTest, DeleteOfAMissingFileIsEntryNotFound) {
    EXPECT_EQ(
        name_of(call("$delete", json::array({uri(directory_ / "missing"), {{"recursive", true}}}))),
        "EntryNotFound");
}

TEST_F(FileSystemServiceTest, RenameMovesAndCreatesTheParentsOfTheTarget) {
    const fs::path a = write("a.txt", "content");
    EXPECT_TRUE(is_empty_reply(
        call("$rename",
             json::array({uri(a), uri(directory_ / "x" / "b.txt"), {{"overwrite", false}}}))));
    EXPECT_FALSE(fs::exists(a));
    EXPECT_EQ(contents(directory_ / "x" / "b.txt"), "content");
}

TEST_F(FileSystemServiceTest, RenameOntoAnExistingFileNeedsOverwrite) {
    const fs::path a = write("a.txt", "a");
    const fs::path b = write("b.txt", "b");
    EXPECT_EQ(name_of(call("$rename", json::array({uri(a), uri(b), {{"overwrite", false}}}))),
              "EntryExists");
    EXPECT_EQ(contents(b), "b");
    EXPECT_TRUE(
        is_empty_reply(call("$rename", json::array({uri(a), uri(b), {{"overwrite", true}}}))));
    EXPECT_EQ(contents(b), "a");
}

TEST_F(FileSystemServiceTest, CopyCopiesFilesAndDirectoriesAndLeavesTheSource) {
    write("d/a.txt", "a");
    write("d/sub/b.txt", "b");
    EXPECT_TRUE(is_empty_reply(
        call("$copy",
             json::array({uri(directory_ / "d"), uri(directory_ / "e"), {{"overwrite", false}}}))));
    EXPECT_EQ(contents(directory_ / "e" / "sub" / "b.txt"), "b");
    EXPECT_TRUE(fs::exists(directory_ / "d" / "a.txt"));
    EXPECT_EQ(name_of(call("$copy", json::array({uri(directory_ / "d"),
                                                 uri(directory_ / "e"),
                                                 {{"overwrite", false}}}))),
              "EntryExists");
}

TEST_F(FileSystemServiceTest, RenameAndCopyOfAMissingSourceAreEntryNotFound) {
    const json options = {{"overwrite", true}};
    EXPECT_EQ(name_of(call("$rename", json::array({uri(directory_ / "nothing"),
                                                   uri(directory_ / "b"), options}))),
              "EntryNotFound");
    EXPECT_EQ(name_of(call("$copy", json::array({uri(directory_ / "nothing"), uri(directory_ / "b"),
                                                 options}))),
              "EntryNotFound");
}

// --- other schemes and activation ---

TEST_F(FileSystemServiceTest, AResourceOfAnotherSchemeIsUnavailableAsWhenNoProviderIsRegistered) {
    const json resource = uri_to_json(core::Uri{"memfs", "", "/a.txt", "", ""});
    for (const std::string method : {"$stat", "$readdir", "$readFile", "$mkdir"}) {
        const json error = error_of(call(method, json::array({resource})));
        EXPECT_EQ(error.at("name"), "Unavailable") << method;
        EXPECT_NE(error.at("message").get<std::string>().find("ENOPRO"), std::string::npos)
            << method;
    }
    EXPECT_EQ(name_of(call("$delete", json::array({resource, {{"recursive", false}}}))),
              "Unavailable");
    EXPECT_EQ(
        name_of(call("$rename", json::array({uri(write("a")), resource, {{"overwrite", false}}}))),
        "Unavailable");
}

TEST_F(FileSystemServiceTest, EnsureActivationIsAnsweredEmpty) {
    EXPECT_TRUE(is_empty_reply(call("$ensureActivation", json::array({"file"}))));
}

TEST_F(FileSystemServiceTest, AMalformedResourceFailsTheRequest) {
    const json error = error_of(call("$stat", json::array({"not a uri"})));
    EXPECT_EQ(error.at("name"), "Error");
}
