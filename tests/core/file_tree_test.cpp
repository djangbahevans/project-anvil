#include "../timing.hpp"
#include "anvil/core/file_name_order.hpp"
#include "anvil/core/file_tree.hpp"

#include <algorithm>
#include <chrono>
#include <gtest/gtest.h>
#include <map>
#include <nlohmann/json.hpp>
#include <ostream>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace anvil;
using namespace anvil::core;
using anvil::test::timing_is_meaningful;
using Kind = FileTreeChange::Kind;
namespace fs = std::filesystem;

namespace anvil::core {

void PrintTo(const FileTreeEntry& entry, std::ostream* out) {
    *out << (entry.is_directory ? "dir " : "file ") << entry.name
         << (entry.is_symbolic_link ? " (link)" : "");
}

void PrintTo(const FileTreeChange& change, std::ostream* out) {
    *out << (change.kind == FileTreeChange::Kind::added     ? "added "
             : change.kind == FileTreeChange::Kind::removed ? "removed "
                                                            : "changed ");
    PrintTo(change.entry, out);
}

} // namespace anvil::core

namespace {

FileTreeEntry file(std::string name) {
    return {.name = std::move(name)};
}
FileTreeEntry dir(std::string name) {
    return {.name = std::move(name), .is_directory = true};
}
FileTreeEntry link_to_dir(std::string name) {
    return {.name = std::move(name), .is_directory = true, .is_symbolic_link = true};
}

std::vector<std::string> names(const std::vector<FileTreeEntry>& entries) {
    std::vector<std::string> result;
    for (const FileTreeEntry& entry : entries) {
        result.push_back(entry.name);
    }
    return result;
}

using Names = std::vector<std::string>;

// A file system that lists what the test put in it and counts the listings.
class FakeFileSystem {
public:
    void set(const std::string& directory, std::vector<FileTreeEntry> entries) {
        contents_[directory] = std::move(entries);
        failures_.erase(directory);
    }
    void fail(const std::string& directory, std::string message) {
        failures_[directory] = std::move(message);
    }
    int listings(const std::string& directory) const {
        const auto found = listings_.find(directory);
        return found == listings_.end() ? 0 : found->second;
    }
    int total_listings() const {
        int total = 0;
        for (const auto& [directory, count] : listings_) {
            total += count;
        }
        return total;
    }

    DirectoryLister lister() {
        return
            [this](const fs::path& directory) -> std::expected<std::vector<FileTreeEntry>, Error> {
                const std::string key = directory.generic_string();
                ++listings_[key];
                if (const auto failure = failures_.find(key); failure != failures_.end()) {
                    return std::unexpected(Error{ErrorCode::permission_denied, failure->second});
                }
                const auto found = contents_.find(key);
                if (found == contents_.end()) {
                    return std::unexpected(Error{ErrorCode::not_found, "no such directory " + key});
                }
                return found->second;
            };
    }

private:
    std::map<std::string, std::vector<FileTreeEntry>> contents_;
    std::map<std::string, std::string> failures_;
    std::map<std::string, int> listings_;
};

} // namespace

TEST(FileTree, ListsNothingUntilADirectoryIsAskedFor) {
    FakeFileSystem files;
    files.set("/w", {dir("src"), file("a.txt")});
    files.set("/w/src", {file("main.cpp")});
    FileTree tree("/w", files.lister());
    EXPECT_EQ(files.total_listings(), 0);
    EXPECT_FALSE(tree.is_loaded("/w"));

    EXPECT_EQ(names(tree.children("/w")), (Names{"src", "a.txt"}));
    EXPECT_EQ(files.listings("/w"), 1);
    EXPECT_EQ(files.listings("/w/src"), 0);
    EXPECT_TRUE(tree.is_loaded("/w"));
    EXPECT_FALSE(tree.is_loaded("/w/src"));
}

TEST(FileTree, ADirectoryIsListedOnceNoMatterHowOftenItIsAskedFor) {
    FakeFileSystem files;
    files.set("/w", {dir("src")});
    files.set("/w/src", {file("main.cpp")});
    FileTree tree("/w", files.lister());
    for (int i = 0; i < 3; ++i) {
        tree.children("/w");
        tree.children("/w/src");
        tree.children("/w/src/");
        tree.children("/w/./src");
    }
    EXPECT_EQ(files.listings("/w"), 1);
    EXPECT_EQ(files.listings("/w/src"), 1);
}

TEST(FileTree, ARootWrittenWithATrailingSeparatorIsTheSameRoot) {
    FakeFileSystem files;
    files.set("/w", {file("a")});
    FileTree tree("/w/", files.lister());
    EXPECT_EQ(tree.root(), fs::path("/w"));
    EXPECT_EQ(names(tree.children("/w")), (Names{"a"}));
    EXPECT_EQ(names(tree.children("/w/")), (Names{"a"}));
    EXPECT_EQ(files.listings("/w"), 1);
}

TEST(FileTree, APathOutsideTheRootHasNoChildrenAndIsNotListed) {
    FakeFileSystem files;
    files.set("/w", {file("a")});
    files.set("/other", {file("b")});
    FileTree tree("/w", files.lister());
    EXPECT_TRUE(tree.children("/other").empty());
    EXPECT_TRUE(tree.children("/w/../other").empty());
    EXPECT_TRUE(tree.children("/").empty());
    EXPECT_TRUE(tree.children("relative").empty());
    EXPECT_EQ(files.total_listings(), 0);
    EXPECT_EQ(tree.error("/other"), nullptr);
    EXPECT_TRUE(tree.refresh("/other").empty());
}

TEST(FileTree, DirectoriesComeFirstAndEachGroupIsInNaturalOrder) {
    FakeFileSystem files;
    files.set("/w", {file("file10.txt"), dir("b"), file("File2.txt"), dir("A"), file("file2.txt"),
                     dir("a10"), dir("a9"), file(".env"), file("é"), file("e"), file("f")});
    FileTree tree("/w", files.lister());
    EXPECT_EQ(names(tree.children("/w")), (Names{"A", "a9", "a10", "b", ".env", "e", "é", "f",
                                                 "file2.txt", "File2.txt", "file10.txt"}));
}

TEST(FileTree, ALinkToADirectorySortsAmongTheDirectories) {
    FakeFileSystem files;
    files.set("/w",
              {file("a"), link_to_dir("b"), dir("c"), {.name = "d", .is_symbolic_link = true}});
    FileTree tree("/w", files.lister());
    const std::vector<FileTreeEntry> expected = {
        link_to_dir("b"), dir("c"), file("a"), {.name = "d", .is_symbolic_link = true}};
    EXPECT_EQ(tree.children("/w"), expected);
}

namespace {

std::string random_name(std::mt19937& random) {
    static const std::vector<std::string> pieces = {"a",  "b", "A",  "B", "z",  "0", "1", "2",
                                                    "10", "9", "07", ".", "-",  "_", " ", "é",
                                                    "e",  "ø", "o",  "ñ", "日", "ß", "~", "x"};
    std::string name;
    const int length = 1 + static_cast<int>(random() % 5);
    for (int i = 0; i < length; ++i) {
        name += pieces[random() % pieces.size()];
    }
    return name;
}

} // namespace

TEST(FileTree, SortsGeneratedNamesLikeAPlainReference) {
    std::mt19937 random(11);
    for (int round = 0; round < 200; ++round) {
        std::set<std::string> distinct;
        const int count = 1 + static_cast<int>(random() % 40);
        while (static_cast<int>(distinct.size()) < count) {
            distinct.insert(random_name(random));
        }
        std::vector<FileTreeEntry> entries;
        for (const std::string& name : distinct) {
            entries.push_back(random() % 3 == 0 ? dir(name) : file(name));
        }
        std::ranges::shuffle(entries, random);

        std::vector<FileTreeEntry> expected = entries;
        std::ranges::sort(expected, [](const FileTreeEntry& a, const FileTreeEntry& b) {
            if (a.is_directory != b.is_directory) {
                return a.is_directory;
            }
            return compare_file_names(a.name, b.name) < 0;
        });

        FakeFileSystem files;
        files.set("/w", entries);
        FileTree tree("/w", files.lister(), FileExcludes{});
        EXPECT_EQ(tree.children("/w"), expected) << "round " << round;
    }
}

TEST(FileTree, TheDefaultExcludesHideTheVersionControlFolders) {
    FakeFileSystem files;
    files.set("/w", {dir(".git"), dir(".svn"), dir(".hg"), dir(".jj"), file(".DS_Store"),
                     file("Thumbs.db"), dir(".github"), file(".gitignore"), dir("src"),
                     file("thumbs.db")});
    files.set("/w/src", {dir(".git"), file("a.cpp"), file("Thumbs.db")});
    FileTree tree("/w", files.lister());
    EXPECT_EQ(names(tree.children("/w")), (Names{".github", "src", ".gitignore", "thumbs.db"}));
    EXPECT_EQ(names(tree.children("/w/src")), (Names{"a.cpp"}));
}

TEST(FileTree, ExcludesMatchTheModelAgainstAPlainReference) {
    const std::set<std::string> hidden = {".git", ".svn", ".hg", ".jj", ".DS_Store", "Thumbs.db"};
    std::mt19937 random(5);
    for (int round = 0; round < 100; ++round) {
        std::set<std::string> distinct = {".git", "Thumbs.db", "keep"};
        const int count = static_cast<int>(random() % 30);
        for (int i = 0; i < count; ++i) {
            distinct.insert(random() % 4 == 0
                                ? *std::next(hidden.begin(), static_cast<long>(random() % 6))
                                : random_name(random));
        }
        std::vector<FileTreeEntry> entries;
        for (const std::string& name : distinct) {
            entries.push_back(file(name));
        }
        FakeFileSystem files;
        files.set("/w", entries);
        FileTree tree("/w", files.lister());

        std::set<std::string> expected;
        for (const std::string& name : distinct) {
            if (!hidden.contains(name)) {
                expected.insert(name);
            }
        }
        const std::vector<std::string> shown = names(tree.children("/w"));
        EXPECT_EQ(std::set<std::string>(shown.begin(), shown.end()), expected) << "round " << round;
        EXPECT_EQ(shown.size(), expected.size());
    }
}

TEST(FileTree, ExcludesMatchThePathRelativeToTheRoot) {
    FakeFileSystem files;
    files.set("/w", {dir("build"), dir("src"), file("notes.tmp")});
    files.set("/w/src", {dir("build"), file("a.tmp"), file("a.cpp")});
    FileTree tree("/w", files.lister(),
                  FileExcludes{.patterns = {"build", "src/**/*.tmp", "*.tmp"}, .problems = {}});
    EXPECT_EQ(names(tree.children("/w")), (Names{"src"}));
    EXPECT_EQ(names(tree.children("/w/src")), (Names{"build", "a.cpp"}));
}

TEST(FileTree, ADirectoryThatIsExcludedOrBelowAnExcludedOneIsNotListed) {
    FakeFileSystem files;
    files.set("/w", {dir(".git")});
    files.set("/w/.git", {dir("objects")});
    files.set("/w/.git/objects", {file("pack")});
    FileTree tree("/w", files.lister());
    EXPECT_TRUE(tree.children("/w/.git").empty());
    EXPECT_TRUE(tree.children("/w/.git/objects").empty());
    EXPECT_EQ(files.total_listings(), 0);
    EXPECT_FALSE(tree.is_loaded("/w/.git"));
}

TEST(FileTree, ExcludesWithNoPatternsHideNothing) {
    FakeFileSystem files;
    files.set("/w", {dir(".git"), file("a")});
    FileTree tree("/w", files.lister(), FileExcludes{});
    EXPECT_EQ(names(tree.children("/w")), (Names{".git", "a"}));
}

TEST(FileExcludes, TheDefaultsAreTheOnesOfVsCode) {
    const FileExcludes defaults = FileExcludes::defaults();
    EXPECT_EQ(defaults.patterns,
              (Names{"**/.git", "**/.svn", "**/.hg", "**/.jj", "**/.DS_Store", "**/Thumbs.db"}));
    EXPECT_TRUE(defaults.problems.empty());
}

TEST(FileExcludes, FalseTurnsAPatternOffAndAnObjectIsReported) {
    const auto setting = nlohmann::json::parse(R"j({
        "**/.git": true,
        "**/node_modules": false,
        "**/*.pyc": {"when": "$(basename).py"},
        "**/out": "yes",
        "build": true
    })j");
    const FileExcludes excludes = FileExcludes::from_setting(setting);
    EXPECT_EQ(excludes.patterns, (Names{"**/.git", "build"}));
    ASSERT_EQ(excludes.problems.size(), 2U);
    EXPECT_NE(excludes.problems[0].find("**/*.pyc"), std::string::npos);
    EXPECT_NE(excludes.problems[1].find("**/out"), std::string::npos);
}

TEST(FileExcludes, ASettingThatIsNotAnObjectIsAProblem) {
    for (const char* text : {"[]", "true", "\"x\"", "null", "3"}) {
        const FileExcludes excludes = FileExcludes::from_setting(nlohmann::json::parse(text));
        EXPECT_TRUE(excludes.patterns.empty()) << text;
        EXPECT_EQ(excludes.problems.size(), 1U) << text;
    }
}

TEST(FileTree, ALinkIsNotFollowedUntilItIsAskedFor) {
    FakeFileSystem files;
    files.set("/w", {link_to_dir("loop"), dir("real")});
    files.set("/w/loop", {link_to_dir("loop")});
    files.set("/w/loop/loop", {link_to_dir("loop")});
    files.set("/w/real", {file("a")});
    FileTree tree("/w", files.lister());
    EXPECT_EQ(tree.children("/w"), (std::vector<FileTreeEntry>{link_to_dir("loop"), dir("real")}));
    EXPECT_EQ(files.total_listings(), 1);

    EXPECT_EQ(tree.children("/w/loop"), (std::vector<FileTreeEntry>{link_to_dir("loop")}));
    EXPECT_EQ(files.total_listings(), 2);
    EXPECT_EQ(tree.children("/w/loop/loop"), (std::vector<FileTreeEntry>{link_to_dir("loop")}));
    EXPECT_EQ(files.total_listings(), 3);
    EXPECT_FALSE(tree.is_loaded("/w/loop/loop/loop"));
}

TEST(FileTree, ADirectoryThatCannotBeListedKeepsItsErrorAndTheTreeGoesOn) {
    FakeFileSystem files;
    files.set("/w", {dir("locked"), dir("open")});
    files.fail("/w/locked", "permission denied");
    files.set("/w/open", {file("a")});
    FileTree tree("/w", files.lister());
    ASSERT_EQ(tree.children("/w").size(), 2U);

    EXPECT_TRUE(tree.children("/w/locked").empty());
    ASSERT_NE(tree.error("/w/locked"), nullptr);
    EXPECT_EQ(tree.error("/w/locked")->message, "permission denied");
    EXPECT_EQ(tree.error("/w/locked")->code, ErrorCode::permission_denied);
    EXPECT_EQ(names(tree.children("/w/open")), (Names{"a"}));
    EXPECT_EQ(tree.error("/w/open"), nullptr);
    EXPECT_EQ(tree.error("/w"), nullptr);

    tree.children("/w/locked");
    EXPECT_EQ(files.listings("/w/locked"), 1);
    EXPECT_TRUE(tree.is_loaded("/w/locked"));
}

TEST(FileTree, ARootThatCannotBeListedHasAnError) {
    FakeFileSystem files;
    FileTree tree("/missing", files.lister());
    EXPECT_TRUE(tree.children("/missing").empty());
    ASSERT_NE(tree.error("/missing"), nullptr);
    EXPECT_EQ(tree.error("/missing")->code, ErrorCode::not_found);
}

TEST(FileTreeRefresh, ReportsTheEntriesThatWereAddedRemovedOrChanged) {
    FakeFileSystem files;
    files.set("/w", {file("keep"), file("gone"), file("becomes-dir"), dir("becomes-file"),
                     file("becomes-link")});
    FileTree tree("/w", files.lister());
    tree.children("/w");

    files.set("/w", {file("keep"),
                     file("new"),
                     dir("becomes-dir"),
                     file("becomes-file"),
                     {.name = "becomes-link", .is_symbolic_link = true},
                     dir("new-dir")});
    const std::vector<FileTreeChange> changes = tree.refresh("/w");

    const std::vector<FileTreeChange> expected = {
        {Kind::changed, dir("becomes-dir")},
        {Kind::added, dir("new-dir")},
        {Kind::changed, file("becomes-file")},
        {Kind::changed, {.name = "becomes-link", .is_symbolic_link = true}},
        {Kind::removed, file("gone")},
        {Kind::added, file("new")},
    };
    EXPECT_EQ(changes, expected);
    EXPECT_EQ(names(tree.children("/w")),
              (Names{"becomes-dir", "new-dir", "becomes-file", "becomes-link", "keep", "new"}));
    EXPECT_EQ(files.listings("/w"), 2);
}

TEST(FileTreeRefresh, ReportsNothingWhenNothingChanged) {
    FakeFileSystem files;
    files.set("/w", {file("a"), dir("b")});
    FileTree tree("/w", files.lister());
    tree.children("/w");
    EXPECT_TRUE(tree.refresh("/w").empty());
    EXPECT_EQ(files.listings("/w"), 2);
}

TEST(FileTreeRefresh, ReportsNothingAndListsNothingForADirectoryThatWasNeverLoaded) {
    FakeFileSystem files;
    files.set("/w", {dir("src")});
    files.set("/w/src", {file("a")});
    FileTree tree("/w", files.lister());
    EXPECT_TRUE(tree.refresh("/w").empty());
    tree.children("/w");
    EXPECT_TRUE(tree.refresh("/w/src").empty());
    EXPECT_EQ(files.listings("/w/src"), 0);
    EXPECT_EQ(files.listings("/w"), 1);
}

TEST(FileTreeRefresh, DoesNotReportEntriesThatTheExcludesHide) {
    FakeFileSystem files;
    files.set("/w", {file("a")});
    FileTree tree("/w", files.lister());
    tree.children("/w");
    files.set("/w", {file("a"), dir(".git")});
    EXPECT_TRUE(tree.refresh("/w").empty());
}

TEST(FileTreeRefresh, ForgetsTheDirectoriesBelowAnEntryThatWasReplaced) {
    FakeFileSystem files;
    files.set("/w", {dir("d"), dir("kept")});
    files.set("/w/d", {dir("inner")});
    files.set("/w/d/inner", {file("old")});
    files.set("/w/kept", {file("k")});
    FileTree tree("/w", files.lister());
    tree.children("/w");
    tree.children("/w/d");
    tree.children("/w/d/inner");
    tree.children("/w/kept");

    files.set("/w", {file("d"), dir("kept")});
    tree.refresh("/w");
    EXPECT_FALSE(tree.is_loaded("/w/d"));
    EXPECT_FALSE(tree.is_loaded("/w/d/inner"));
    EXPECT_TRUE(tree.is_loaded("/w/kept"));

    files.set("/w", {dir("d"), dir("kept")});
    files.set("/w/d/inner", {file("new")});
    tree.refresh("/w");
    EXPECT_EQ(names(tree.children("/w/d/inner")), (Names{"new"}));
    EXPECT_EQ(files.listings("/w/d/inner"), 2);
    EXPECT_EQ(files.listings("/w/kept"), 1);
}

TEST(FileTreeRefresh, ForgetsTheDirectoriesBelowAnEntryThatWasRemoved) {
    FakeFileSystem files;
    files.set("/w", {dir("d")});
    files.set("/w/d", {file("a")});
    FileTree tree("/w", files.lister());
    tree.children("/w");
    tree.children("/w/d");
    files.set("/w", {});
    EXPECT_EQ(tree.refresh("/w"), (std::vector<FileTreeChange>{{Kind::removed, dir("d")}}));
    EXPECT_FALSE(tree.is_loaded("/w/d"));
}

TEST(FileTreeRefresh, KeepsTheDirectoriesBelowAnEntryThatDidNotChange) {
    FakeFileSystem files;
    files.set("/w", {dir("d")});
    files.set("/w/d", {file("a")});
    FileTree tree("/w", files.lister());
    tree.children("/w");
    tree.children("/w/d");
    files.set("/w", {dir("d"), file("x")});
    tree.refresh("/w");
    EXPECT_TRUE(tree.is_loaded("/w/d"));
    EXPECT_EQ(files.listings("/w/d"), 1);
}

TEST(FileTreeRefresh, ADirectoryThatStopsBeingListableLosesAllItsEntries) {
    FakeFileSystem files;
    files.set("/w", {dir("d")});
    files.set("/w/d", {file("a"), file("b")});
    FileTree tree("/w", files.lister());
    tree.children("/w");
    tree.children("/w/d");

    files.fail("/w/d", "gone");
    EXPECT_EQ(tree.refresh("/w/d"), (std::vector<FileTreeChange>{{Kind::removed, file("a")},
                                                                 {Kind::removed, file("b")}}));
    EXPECT_TRUE(tree.children("/w/d").empty());
    ASSERT_NE(tree.error("/w/d"), nullptr);

    files.set("/w/d", {file("c")});
    EXPECT_EQ(tree.refresh("/w/d"), (std::vector<FileTreeChange>{{Kind::added, file("c")}}));
    EXPECT_EQ(tree.error("/w/d"), nullptr);
}

TEST(FileExcludes, TheSettingIsLaidOverTheDefaultsAsVsCodeMergesThem) {
    const FileExcludes defaults = FileExcludes::defaults();
    EXPECT_EQ(FileExcludes::from_settings(Settings{}).patterns, defaults.patterns);

    const auto settings = Settings::parse(R"j({
        "files.exclude": {"**/.git": false, "**/node_modules": true, "**/.svn": true, "**/x": false}
    })j");
    ASSERT_TRUE(settings.has_value());
    const FileExcludes merged = FileExcludes::from_settings(*settings);
    EXPECT_EQ(merged.patterns, (Names{"**/.svn", "**/.hg", "**/.jj", "**/.DS_Store", "**/Thumbs.db",
                                      "**/node_modules"}));
    EXPECT_TRUE(merged.problems.empty());
}

TEST(FileExcludes, TheSettingMayBeWrittenNestedAndItsProblemsAreReported) {
    const auto settings =
        Settings::parse(R"j({"files": {"exclude": {"a": true, "b": {"when": "c"}}}})j");
    ASSERT_TRUE(settings.has_value());
    const FileExcludes merged = FileExcludes::from_settings(*settings);
    EXPECT_EQ(merged.patterns.back(), "a");
    EXPECT_EQ(merged.problems.size(), 1U);

    const auto wrong = Settings::parse(R"j({"files.exclude": ["a"]})j");
    ASSERT_TRUE(wrong.has_value());
    const FileExcludes fallback = FileExcludes::from_settings(*wrong);
    EXPECT_EQ(fallback.patterns, FileExcludes::defaults().patterns);
    EXPECT_EQ(fallback.problems.size(), 1U);
}

TEST(FileTree, ChangingTheExcludesTakesEffectWhenADirectoryIsReadAgain) {
    FakeFileSystem files;
    files.set("/w", {dir("build"), dir(".git"), file("a")});
    FileTree tree("/w", files.lister());
    EXPECT_EQ(names(tree.children("/w")), (Names{"build", "a"}));

    tree.set_excludes(FileExcludes{.patterns = {"build"}, .problems = {}});
    EXPECT_EQ(names(tree.children("/w")), (Names{"build", "a"}));
    EXPECT_EQ(tree.refresh("/w"), (std::vector<FileTreeChange>{{Kind::added, dir(".git")},
                                                               {Kind::removed, dir("build")}}));
    EXPECT_EQ(names(tree.children("/w")), (Names{".git", "a"}));
}

TEST(DirectoryReader, ReadsADirectoryAsTheTreeHoldsIt) {
    FakeFileSystem files;
    files.set("/w", {file("zeta.txt"), dir("src"), file("a10.txt"), file("a9.txt"), dir(".git"),
                     dir("build")});
    FileTree tree("/w", files.lister());
    const DirectoryReader reader = tree.reader();

    const std::optional<FileTreeListing> listing = reader.read("/w");
    ASSERT_TRUE(listing.has_value());
    EXPECT_FALSE(listing->error.has_value());
    EXPECT_EQ(listing->entries, tree.children("/w"));
    EXPECT_EQ(names(listing->entries), (Names{"build", "src", "a9.txt", "a10.txt", "zeta.txt"}));
    EXPECT_EQ(files.listings("/w"), 2) << "the reader listed once, and so did the tree";
}

TEST(DirectoryReader, ADirectoryThatCannotBeListedHasItsErrorAndNoEntries) {
    FakeFileSystem files;
    files.fail("/w", "permission denied");
    const DirectoryReader reader = FileTree("/w", files.lister()).reader();
    const std::optional<FileTreeListing> listing = reader.read("/w");
    ASSERT_TRUE(listing.has_value());
    EXPECT_TRUE(listing->entries.empty());
    ASSERT_TRUE(listing->error.has_value());
    EXPECT_EQ(listing->error->message, "permission denied");
}

TEST(DirectoryReader, NothingIsListedForAPathOutsideTheRootOrExcluded) {
    FakeFileSystem files;
    files.set("/w", {dir(".git")});
    files.set("/w/.git", {file("config")});
    files.set("/other", {file("x")});
    const DirectoryReader reader = FileTree("/w", files.lister()).reader();
    for (const char* path : {"/other", "/w/../other", "/w/.git", "/w/.git/objects", "relative"}) {
        const std::optional<FileTreeListing> listing = reader.read(path);
        ASSERT_TRUE(listing.has_value()) << path;
        EXPECT_TRUE(listing->entries.empty()) << path;
        EXPECT_FALSE(listing->error.has_value()) << path;
    }
    EXPECT_EQ(files.total_listings(), 0);
}

TEST(DirectoryReader, ARequestToStopGivesNothing) {
    FakeFileSystem files;
    files.set("/w", {file("a"), file("b")});
    const DirectoryReader reader = FileTree("/w", files.lister()).reader();

    std::stop_source stopped;
    stopped.request_stop();
    EXPECT_FALSE(reader.read("/w", stopped.get_token()).has_value());
    EXPECT_EQ(files.listings("/w"), 1) << "the listing was one call and was not interrupted";

    std::stop_source running;
    EXPECT_TRUE(reader.read("/w", running.get_token()).has_value());
}

TEST(DirectoryReader, AReaderKeepsTheExcludesItWasMadeWith) {
    FakeFileSystem files;
    files.set("/w", {dir("build"), file("a")});
    FileTree tree("/w", files.lister(), FileExcludes{.patterns = {"build"}, .problems = {}});
    const DirectoryReader reader = tree.reader();
    tree.set_excludes(FileExcludes{});

    EXPECT_EQ(names(reader.read("/w")->entries), (Names{"a"}));
    EXPECT_EQ(names(tree.reader().read("/w")->entries), (Names{"build", "a"}));
}

TEST(FileTreeAdopt, ATakenListingIsNotListedAgainAndForgettingMakesItListedAgain) {
    FakeFileSystem files;
    files.set("/w", {dir("src"), file("a")});
    files.set("/w/src", {file("main.cpp")});
    FileTree tree("/w", files.lister());

    std::optional<FileTreeListing> listing = tree.reader().read("/w/src");
    ASSERT_TRUE(listing.has_value());
    ASSERT_TRUE(tree.adopt("/w/src", std::move(*listing)));
    EXPECT_TRUE(tree.is_loaded("/w/src"));
    EXPECT_EQ(names(tree.children("/w/src")), (Names{"main.cpp"}));
    EXPECT_EQ(files.listings("/w/src"), 1) << "children() used what was adopted";

    tree.forget("/w/src");
    EXPECT_FALSE(tree.is_loaded("/w/src"));
    EXPECT_EQ(names(tree.children("/w/src")), (Names{"main.cpp"}));
    EXPECT_EQ(files.listings("/w/src"), 2);
}

TEST(FileTreeAdopt, ForgettingADirectoryForgetsTheOnesBelowIt) {
    FakeFileSystem files;
    files.set("/w", {dir("a"), dir("ab")});
    files.set("/w/a", {dir("b")});
    files.set("/w/a/b", {file("x")});
    files.set("/w/ab", {file("y")});
    FileTree tree("/w", files.lister());
    tree.children("/w/a");
    tree.children("/w/a/b");
    tree.children("/w/ab");

    tree.forget("/w/a");
    EXPECT_FALSE(tree.is_loaded("/w/a"));
    EXPECT_FALSE(tree.is_loaded("/w/a/b"));
    EXPECT_TRUE(tree.is_loaded("/w/ab")) << "a name that only starts the same is not below it";
}

TEST(FileTreeAdopt, ADirectoryThatIsOutsideOrExcludedTakesNothing) {
    FakeFileSystem files;
    FileTree tree("/w", files.lister());
    EXPECT_FALSE(tree.adopt("/other", FileTreeListing{}));
    EXPECT_FALSE(tree.adopt("/w/.git", FileTreeListing{}));
    EXPECT_FALSE(tree.is_loaded("/other"));
}

TEST(FileTreeRefresh, ADirectoryOfTensOfThousandsOfEntriesIsComparedInOnePass) {
    FakeFileSystem files;
    std::vector<FileTreeEntry> before;
    std::vector<FileTreeEntry> after;
    for (int i = 0; i < 50000; ++i) {
        before.push_back(file("f" + std::to_string(i) + ".txt"));
        if (i % 2 == 0) {
            after.push_back(before.back());
        }
    }
    for (int i = 0; i < 25000; ++i) {
        after.push_back(file("g" + std::to_string(i) + ".txt"));
    }
    files.set("/w", before);
    FileTree tree("/w", files.lister());
    tree.children("/w");
    files.set("/w", after);

    const auto begin = std::chrono::steady_clock::now();
    const std::vector<FileTreeChange> changes = tree.refresh("/w");
    const double took =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    EXPECT_EQ(changes.size(), 25000U + 25000U);
    if (timing_is_meaningful) {
        EXPECT_LT(took, 3000.0) << "comparing 50,000 entries with 50,000 took " << took << " ms";
    }
}

namespace {

anvil::core::FileSearchOptions search_options(const std::string& json) {
    return anvil::core::FileSearchOptions::from_settings(*anvil::core::Settings::parse(json));
}
bool excludes(const anvil::core::FileSearchOptions& options, const std::string& pattern) {
    return std::ranges::find(options.excludes, pattern) != options.excludes.end();
}

} // namespace

TEST(FileSearchOptionsTest, TheDefaultsAreThoseOfVsCode) {
    const auto options = search_options("{}");
    EXPECT_EQ(options.excludes,
              (std::vector<std::string>{"**/.git", "**/.svn", "**/.hg", "**/.jj", "**/.DS_Store",
                                        "**/Thumbs.db", "**/node_modules", "**/bower_components",
                                        "**/*.code-search"}));
    EXPECT_TRUE(options.use_ignore_files);
    EXPECT_FALSE(options.use_global_ignore_files);
    EXPECT_FALSE(options.use_parent_ignore_files);
    EXPECT_TRUE(options.follow_symlinks);
    EXPECT_TRUE(options.problems.empty());
}

TEST(FileSearchOptionsTest, TheSearchExcludesLieOverTheFileExcludes) {
    const auto options = search_options(
        R"({"files.exclude": {"**/target": true, "**/dist": true, "**/.git": false,)"
        R"( "**/node_modules": false},)"
        R"( "search.exclude": {"**/dist": false, "**/out": true, "**/bower_components": false}})");
    EXPECT_TRUE(excludes(options, "**/target"));
    EXPECT_TRUE(excludes(options, "**/out"));
    EXPECT_FALSE(excludes(options, "**/dist"));
    EXPECT_FALSE(excludes(options, "**/.git"));
    EXPECT_FALSE(excludes(options, "**/bower_components"));
    // A default of `search.exclude` wins over `files.exclude`.
    EXPECT_TRUE(excludes(options, "**/node_modules"));
    EXPECT_TRUE(options.problems.empty());
}

TEST(FileSearchOptionsTest, TheSwitchesAreReadFlatOrNested) {
    const auto options = search_options(
        R"({"search.useIgnoreFiles": false, "search": {"useGlobalIgnoreFiles": true,)"
        R"( "useParentIgnoreFiles": true, "followSymlinks": false}})");
    EXPECT_FALSE(options.use_ignore_files);
    EXPECT_TRUE(options.use_global_ignore_files);
    EXPECT_TRUE(options.use_parent_ignore_files);
    EXPECT_FALSE(options.follow_symlinks);
}

TEST(FileSearchOptionsTest, AValueOfTheWrongTypeIsReportedByName) {
    for (const char* key :
         {"search.useIgnoreFiles", "search.useGlobalIgnoreFiles", "search.useParentIgnoreFiles",
          "search.followSymlinks", "search.exclude", "files.exclude"}) {
        const auto options = search_options(std::string("{\"") + key + "\": \"yes\"}");
        ASSERT_EQ(options.problems.size(), 1U) << key;
        EXPECT_NE(options.problems[0].find(key), std::string::npos) << options.problems[0];
        EXPECT_TRUE(options.use_ignore_files);
        EXPECT_TRUE(excludes(options, "**/node_modules"));
    }
    const auto entry = search_options(R"({"search.exclude": {"**/a": 1}})");
    ASSERT_EQ(entry.problems.size(), 1U);
    EXPECT_NE(entry.problems[0].find("search.exclude"), std::string::npos);
}
