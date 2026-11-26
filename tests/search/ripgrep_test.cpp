#include "anvil/search/ripgrep.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <thread>
#include <unistd.h>

using anvil::core::FileSearchOptions;
using anvil::core::Settings;
using anvil::search::FileListing;
using anvil::search::find_ripgrep;
using anvil::search::list_arguments;
using anvil::search::RipgrepLocations;
using std::chrono::milliseconds;
namespace fs = std::filesystem;

namespace {

std::string env(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}

// The options of a workspace whose settings are `json`.
FileSearchOptions options_of(const std::string& json = "{}") {
    return FileSearchOptions::from_settings(*Settings::parse(json));
}

struct Listed {
    std::vector<std::string> files;
    std::optional<anvil::Error> error;
    bool finished = false;
};

// Reads a listing to its end, or for ten seconds.
Listed collect(FileListing& listing) {
    Listed result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!result.finished && std::chrono::steady_clock::now() < deadline) {
        auto batch = listing.read(milliseconds(100));
        std::ranges::move(batch.files, std::back_inserter(result.files));
        result.finished = batch.finished;
        result.error = batch.error;
    }
    std::ranges::sort(result.files);
    return result;
}

using Names = std::vector<std::string>;

class ScratchTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = fs::temp_directory_path() / ("anvil-search-test-" + std::to_string(::getpid()));
        fs::remove_all(root_);
        fs::create_directories(root_);
        root_ = fs::canonical(root_);
    }
    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    void write(const std::string& name, const std::string& contents = "x") {
        const fs::path path = root_ / name;
        fs::create_directories(path.parent_path());
        std::ofstream(path) << contents;
    }
    fs::path program(const std::string& name, const std::string& script) {
        write(name, "#!/bin/sh\n" + script + "\n");
        fs::permissions(root_ / name, fs::perms::owner_all);
        return root_ / name;
    }

    fs::path root_;
};

// A shell script in place of ripgrep, so that a test decides what arrives and when.
class StandInTest : public ScratchTest {
protected:
    FileListing start(const std::string& script) {
        auto listing =
            FileListing::start(program("stand-in/rg", script), root_, FileSearchOptions{});
        EXPECT_TRUE(listing.has_value()) << listing.error().message;
        return std::move(*listing);
    }
    // The process id that the script wrote.
    pid_t written_pid() {
        for (int i = 0; i < 500; ++i) {
            std::ifstream in(root_ / "pid");
            pid_t pid = 0;
            if (in >> pid) {
                return pid;
            }
            std::this_thread::sleep_for(milliseconds(10));
        }
        return 0;
    }
};

// The real ripgrep over a real tree. Where it is required, as in CI, its absence is a failure.
class RipgrepTest : public ScratchTest {
protected:
    void SetUp() override {
        ScratchTest::SetUp();
        const auto found = find_ripgrep({.override_path = env("ANVIL_RG"),
                                         .vscode_dir = ANVIL_VSCODE_DIR,
                                         .path_variable = env("PATH")});
        if (!found || ::access(found->c_str(), X_OK) != 0) {
            ASSERT_TRUE(env("ANVIL_REQUIRE_RIPGREP").empty()) << "ripgrep is required and missing";
            GTEST_SKIP() << "needs ripgrep (scripts/fetch-vscode.sh, PATH or ANVIL_RG)";
        }
        rg_ = *found;
    }

    Listed list(const FileSearchOptions& options = options_of(), const fs::path& folder = {}) {
        auto listing = FileListing::start(rg_, folder.empty() ? root_ : folder, options);
        EXPECT_TRUE(listing.has_value()) << listing.error().message;
        if (!listing) {
            return {};
        }
        Listed result = collect(*listing);
        EXPECT_TRUE(result.finished);
        return result;
    }

    fs::path rg_;
};

} // namespace

TEST(RipgrepArguments, AreThoseOfVsCode) {
    FileSearchOptions options;
    options.excludes = {"**/.git", "build", "/dist/", "a\\b", ""};
    EXPECT_EQ(
        list_arguments(options),
        (Names{"--files", "--hidden", "--case-sensitive", "--no-require-git", "-g", "!**/.git",
               "-g", "!/build", "-g", "!/dist", "-g", "!/a/b", "--no-ignore-parent", "--follow",
               "--no-config", "--no-ignore-global", "--null", "--path-separator=/"}));
}

TEST(RipgrepArguments, FollowTheOptions) {
    FileSearchOptions options;
    options.use_ignore_files = false;
    options.follow_symlinks = false;
    options.use_global_ignore_files = true;
    EXPECT_EQ(list_arguments(options),
              (Names{"--files", "--hidden", "--case-sensitive", "--no-require-git", "--no-ignore",
                     "--no-config", "--null", "--path-separator=/"}));
    options.use_ignore_files = true;
    options.use_parent_ignore_files = true;
    EXPECT_EQ(list_arguments(options),
              (Names{"--files", "--hidden", "--case-sensitive", "--no-require-git", "--no-config",
                     "--null", "--path-separator=/"}));
}

TEST(RipgrepArguments, IncludesComeAnchoredBeforeTheExcludes) {
    FileSearchOptions options;
    options.excludes = {"**/.git"};
    const Names arguments = list_arguments(options, {"**/*.toml", "a.txt", "b\\c", ""});
    EXPECT_EQ((Names(arguments.begin(), arguments.begin() + 12)),
              (Names{"--files", "--hidden", "--case-sensitive", "--no-require-git", "-g",
                     "**/*.toml", "-g", "/a.txt", "-g", "/b/c", "-g", "!**/.git"}));
}

class FindRipgrepTest : public ScratchTest {
protected:
    fs::path bundled() {
        return program("vscode/node_modules/@vscode/ripgrep-universal/bin/" +
                           std::string(ANVIL_TARGET) + "/rg",
                       "exit 0");
    }
    std::string path_of(std::initializer_list<const char*> directories) const {
        std::string result;
        for (const char* directory : directories) {
            result += (result.empty() ? "" : ":") + (root_ / directory).string();
        }
        return result;
    }
};

TEST_F(FindRipgrepTest, TheOverrideComesFirstWhetherOrNotItCanRun) {
    bundled();
    const RipgrepLocations locations{.override_path = root_ / "no-such-rg",
                                     .vscode_dir = root_ / "vscode"};
    EXPECT_EQ(find_ripgrep(locations), root_ / "no-such-rg");
    EXPECT_EQ(find_ripgrep({.override_path = "relative/rg"}), fs::current_path() / "relative/rg");
}

TEST_F(FindRipgrepTest, ThenTheOneOfTheCheckoutAndThenThePath) {
    const fs::path shipped = bundled();
    const fs::path on_path = program("bin/rg", "exit 0");
    const RipgrepLocations locations{.vscode_dir = root_ / "vscode",
                                     .path_variable = path_of({"bin"})};
    EXPECT_EQ(find_ripgrep(locations), shipped);
    fs::permissions(shipped, fs::perms::owner_read);
    EXPECT_EQ(find_ripgrep(locations), on_path);
    fs::remove(shipped);
    EXPECT_EQ(find_ripgrep(locations), on_path);
}

TEST_F(FindRipgrepTest, ThePathIsSearchedInOrderPassingOverWhatCannotRun) {
    write("first/rg"); // not executable
    fs::create_directories(root_ / "second" / "rg");
    const fs::path third = program("third/rg", "exit 0");
    program("fourth/rg", "exit 0");
    EXPECT_EQ(find_ripgrep({.path_variable = path_of({"first", "second", "third", "fourth"})}),
              third);
    EXPECT_EQ(find_ripgrep({.path_variable = path_of({"first", "second"})}), std::nullopt);
    EXPECT_EQ(find_ripgrep({}), std::nullopt);
    // A relative entry would depend on the working directory.
    const fs::path before = fs::current_path();
    fs::current_path(root_);
    EXPECT_EQ(find_ripgrep({.path_variable = "third::."}), std::nullopt);
    fs::current_path(before);
}

TEST_F(StandInTest, NamesArriveWhileRipgrepRunsAndOnesThatArriveInPiecesAreWhole) {
    FileListing listing = start("printf 'one\\0tw'; sleep 0.3; printf 'o\\0'; exec sleep 30");
    Names files;
    for (int i = 0; i < 100 && files.size() < 2; ++i) {
        auto batch = listing.read(milliseconds(100));
        EXPECT_FALSE(batch.finished);
        std::ranges::move(batch.files, std::back_inserter(files));
    }
    EXPECT_EQ(files, (Names{"one", "two"}));
    const auto start = std::chrono::steady_clock::now();
    const auto batch = listing.read(milliseconds(100));
    EXPECT_TRUE(batch.files.empty());
    EXPECT_FALSE(batch.finished);
    EXPECT_GE(std::chrono::steady_clock::now() - start, milliseconds(90));
}

TEST_F(StandInTest, AReadWithoutATimeoutDoesNotWaitForRipgrepToExit) {
    FileListing listing = start("exec >&- 2>&-; exec sleep 5");
    // Until both streams are seen closed.
    std::this_thread::sleep_for(milliseconds(300));
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 5; ++i) {
        EXPECT_FALSE(listing.read(milliseconds(0)).finished);
    }
    EXPECT_LT(std::chrono::steady_clock::now() - start, milliseconds(100));
}

TEST_F(StandInTest, CancellingEndsRipgrepAtOnce) {
    FileListing listing = start("echo $$ > pid; exec sleep 30");
    const pid_t pid = written_pid();
    ASSERT_GT(pid, 0);
    ASSERT_EQ(::kill(pid, 0), 0);
    listing.cancel();
    EXPECT_EQ(::kill(pid, 0), -1);
    const auto batch = listing.read(milliseconds(0));
    EXPECT_TRUE(batch.finished);
    EXPECT_TRUE(batch.files.empty());
    EXPECT_FALSE(batch.error.has_value());
}

TEST_F(StandInTest, DestroyingAListingEndsRipgrep) {
    pid_t pid = 0;
    {
        FileListing listing = start("echo $$ > pid; exec sleep 30");
        pid = written_pid();
        ASSERT_GT(pid, 0);
    }
    EXPECT_EQ(::kill(pid, 0), -1);
}

TEST_F(StandInTest, StatusOneIsAFolderWithoutFiles) {
    FileListing listing = start("exit 1");
    const Listed result = collect(listing);
    EXPECT_TRUE(result.finished);
    EXPECT_TRUE(result.files.empty());
    EXPECT_FALSE(result.error.has_value());
}

TEST_F(StandInTest, AFailureIsReportedWithWhatRipgrepSaidAndTheFilesAreKept) {
    FileListing listing = start("printf 'kept\\0'; echo 'cannot read that' >&2; exit 2");
    const Listed result = collect(listing);
    EXPECT_EQ(result.files, Names{"kept"});
    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(result.error->message, "cannot read that");
    const auto after = listing.read(milliseconds(0));
    EXPECT_TRUE(after.finished);
    EXPECT_FALSE(after.error.has_value());
}

TEST_F(StandInTest, AFailureThatSaysNothingIsReportedByItsStatus) {
    FileListing listing = start("kill -9 $$");
    const Listed result = collect(listing);
    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(result.error->message, "ripgrep failed with status 137");
}

TEST_F(StandInTest, OnlyTheStartOfWhatRipgrepSaysIsKeptWhileAllOfItIsRead) {
    FileListing listing =
        start("head -c 1048576 /dev/zero | tr '\\0' e >&2; printf 'last\\0'; exit 2");
    const Listed result = collect(listing);
    EXPECT_TRUE(result.finished);
    EXPECT_EQ(result.files, Names{"last"});
    ASSERT_TRUE(result.error.has_value());
    EXPECT_EQ(result.error->message.size(), 4096U);
}

TEST_F(StandInTest, RipgrepRunsInTheFolderWithTheArguments) {
    FileListing listing = start("printf '%s\\0' \"$(pwd -P)\" \"$@\"");
    Listed result = collect(listing);
    Names expected = list_arguments(FileSearchOptions{});
    expected.push_back(root_.string());
    std::ranges::sort(expected);
    EXPECT_EQ(result.files, expected);
}

TEST_F(StandInTest, StartingFailsWithoutAProgramOrAFolder) {
    EXPECT_FALSE(FileListing::start(root_ / "missing", root_, {}).has_value());
    write("file");
    const fs::path rg = program("rg", "exit 0");
    EXPECT_FALSE(FileListing::start(rg, root_ / "file", {}).has_value());
    EXPECT_FALSE(FileListing::start(rg, root_ / "no-folder", {}).has_value());
}

TEST_F(RipgrepTest, ListsHiddenFilesAndLeavesOutWhatTheSettingsExclude) {
    write("a.txt");
    write(".hidden");
    write("sub/b.txt");
    write("sub/node_modules/dep.js");
    write("bower_components/c.js");
    write("x.code-search");
    write(".git/config");
    write("sub/.DS_Store");
    EXPECT_EQ(list().files, (Names{".hidden", "a.txt", "sub/b.txt"}));
    EXPECT_EQ(
        list(options_of(R"({"search.exclude": {"sub": true, "**/node_modules": false}})")).files,
        (Names{".hidden", "a.txt"}));
    EXPECT_EQ(list(options_of(R"({"files.exclude": {"**/*.txt": true}})")).files, Names{".hidden"});
}

TEST_F(RipgrepTest, RespectsAnIgnoreFileInAFolderWithoutGitUnlessTold) {
    write("sub/.gitignore", "out/\n");
    write("sub/out/o.txt");
    write("sub/kept.txt");
    write(".ignore", "*.log\n");
    write("run.log");
    EXPECT_EQ(list().files, (Names{".ignore", "sub/.gitignore", "sub/kept.txt"}));
    EXPECT_EQ(list(options_of(R"({"search.useIgnoreFiles": false})")).files,
              (Names{".ignore", "run.log", "sub/.gitignore", "sub/kept.txt", "sub/out/o.txt"}));
}

TEST_F(RipgrepTest, RespectsTheIgnoreFilesAboveTheFolderOnlyWhenTold) {
    write(".gitignore", "secret.txt\n");
    write("project/secret.txt");
    write("project/open.txt");
    EXPECT_EQ(list(options_of(), root_ / "project").files, (Names{"open.txt", "secret.txt"}));
    EXPECT_EQ(list(options_of(R"({"search.useParentIgnoreFiles": true})"), root_ / "project").files,
              Names{"open.txt"});
}

TEST_F(RipgrepTest, RespectsTheIgnoreFileOfTheUserOnlyWhenTold) {
    write("config/git/ignore", "private.txt\n");
    write("project/private.txt");
    write("project/open.txt");
    const std::string home = env("HOME");
    const std::string config = env("XDG_CONFIG_HOME");
    ::setenv("HOME", (root_ / "home").c_str(), 1);
    ::setenv("XDG_CONFIG_HOME", (root_ / "config").c_str(), 1);
    EXPECT_EQ(list(options_of(), root_ / "project").files, (Names{"open.txt", "private.txt"}));
    EXPECT_EQ(list(options_of(R"({"search.useGlobalIgnoreFiles": true})"), root_ / "project").files,
              Names{"open.txt"});
    ::setenv("HOME", home.c_str(), 1);
    if (config.empty()) {
        ::unsetenv("XDG_CONFIG_HOME");
    } else {
        ::setenv("XDG_CONFIG_HOME", config.c_str(), 1);
    }
}

TEST_F(RipgrepTest, NamesWithNewlinesAndBytesThatAreNotTextArriveIntact) {
    write("new\nline");
    write("bad\xff name");
    write("sp ace/ü.txt");
    EXPECT_EQ(list().files, (Names{"bad\xff name", "new\nline", "sp ace/ü.txt"}));
}

TEST_F(RipgrepTest, ManyFilesArriveOnce) {
    Names expected;
    for (int i = 0; i < 4000; ++i) {
        const std::string name =
            "many/a-file-with-a-long-enough-name-to-fill-the-batches-" + std::to_string(i);
        write(name);
        expected.push_back(name);
    }
    std::ranges::sort(expected);
    EXPECT_EQ(list().files, expected);
}

TEST_F(RipgrepTest, ASymbolicLinkLoopIsReportedAndTheRestListed) {
    write("sub/a.txt");
    fs::create_directory_symlink(root_, root_ / "sub" / "loop");
    const Listed followed = list();
    EXPECT_EQ(followed.files, Names{"sub/a.txt"});
    ASSERT_TRUE(followed.error.has_value());
    EXPECT_NE(followed.error->message.find("loop"), std::string::npos) << followed.error->message;
    const Listed not_followed = list(options_of(R"({"search.followSymlinks": false})"));
    EXPECT_EQ(not_followed.files, Names{"sub/a.txt"});
    EXPECT_FALSE(not_followed.error.has_value());
}

TEST_F(RipgrepTest, AFolderThatCannotBeReadIsReportedAndTheRestListed) {
    write("closed/in.txt");
    write("open.txt");
    fs::permissions(root_ / "closed", fs::perms::none);
    if (::access((root_ / "closed").c_str(), R_OK) == 0) {
        fs::permissions(root_ / "closed", fs::perms::owner_all);
        GTEST_SKIP() << "the permission does not stop this user from reading";
    }
    const Listed result = list();
    fs::permissions(root_ / "closed", fs::perms::owner_all);
    EXPECT_EQ(result.files, Names{"open.txt"});
    ASSERT_TRUE(result.error.has_value());
    EXPECT_NE(result.error->message.find("closed"), std::string::npos);
}

TEST_F(RipgrepTest, AnEmptyFolderHasNoFilesAndNoError) {
    const Listed result = list();
    EXPECT_TRUE(result.files.empty());
    EXPECT_FALSE(result.error.has_value());
}

TEST_F(RipgrepTest, ACancelledListingGivesNothingMore) {
    write("a.txt");
    auto listing = FileListing::start(rg_, root_, options_of());
    ASSERT_TRUE(listing.has_value());
    listing->cancel();
    const auto batch = listing->read(milliseconds(100));
    EXPECT_TRUE(batch.finished);
    EXPECT_TRUE(batch.files.empty());
}

TEST_F(RipgrepTest, AnIncludeNamesAnIgnoredFileButEntersNoIgnoredFolderAndAnExcludeWins) {
    write(".gitignore", "build/\nignored.fixture\n");
    write("build/in_build.fixture");
    write("ignored.fixture");
    write("sub/a.fixture");
    write("sub/b.txt");
    write("node_modules/dep/x.fixture");
    write("named.fixture/inside.txt");
    auto listing = FileListing::start(rg_, root_, options_of(), {"**/*.fixture"});
    ASSERT_TRUE(listing.has_value());
    EXPECT_EQ(collect(*listing).files, (Names{"ignored.fixture", "sub/a.fixture"}));
}
