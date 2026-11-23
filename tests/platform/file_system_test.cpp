#include "anvil/platform/file.hpp"
#include "anvil/platform/file_system.hpp"

#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace anvil;
using namespace anvil::platform;
namespace fs = std::filesystem;

namespace {

class FileSystemTest : public ::testing::Test {
protected:
    void SetUp() override {
        directory_ =
            fs::temp_directory_path() / ("anvil-file-system-test-" + std::to_string(::getpid()));
        fs::remove_all(directory_);
        fs::create_directories(directory_);
    }
    void TearDown() override {
        std::error_code ignored;
        fs::permissions(directory_, fs::perms::all, fs::perm_options::add, ignored);
        for (const auto& entry : fs::recursive_directory_iterator(directory_, ignored)) {
            fs::permissions(entry.path(), fs::perms::all, fs::perm_options::add, ignored);
        }
        fs::remove_all(directory_, ignored);
    }

    fs::path write(const std::string& name, const std::string& contents = "x") {
        const fs::path path = directory_ / name;
        fs::create_directories(path.parent_path());
        EXPECT_TRUE(write_file(path, contents).has_value());
        return path;
    }
    fs::path directory(const std::string& name) {
        const fs::path path = directory_ / name;
        fs::create_directories(path);
        return path;
    }
    fs::path link(const std::string& name, const fs::path& target) {
        const fs::path path = directory_ / name;
        fs::create_symlink(target, path);
        return path;
    }
    static std::string contents(const fs::path& path) {
        return *read_file(path);
    }
    static ErrorCode code_of(const std::expected<void, Error>& result) {
        return result ? ErrorCode::io_error : result.error().code;
    }

    fs::path directory_;
};

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

// --- stat ---

TEST_F(FileSystemTest, AFileHasTheFileTypeItsSizeAndTimes) {
    const fs::path path = write("a.txt", "hello");
    const auto stat = stat_path(path);
    ASSERT_TRUE(stat.has_value()) << stat.error().message;
    EXPECT_EQ(stat->type, file_type::file);
    EXPECT_EQ(stat->size, 5U);
    EXPECT_FALSE(stat->read_only);
    EXPECT_NEAR(static_cast<double>(stat->modified_ms), static_cast<double>(now_ms()), 60000.0);
    EXPECT_NEAR(static_cast<double>(stat->created_ms), static_cast<double>(now_ms()), 60000.0);
}

TEST_F(FileSystemTest, TheCreationTimeIsNotTheModificationTimeWhereTheSystemRecordsOne) {
    const fs::path path = write("a.txt");
    fs::last_write_time(path, fs::file_time_type::clock::now() - std::chrono::hours(2));
    const auto stat = stat_path(path);
    ASSERT_TRUE(stat.has_value());
    EXPECT_NEAR(static_cast<double>(stat->modified_ms),
                static_cast<double>(now_ms() - 2 * 3600 * 1000), 60000.0);
    struct statx recorded {};
    if (::statx(AT_FDCWD, path.c_str(), 0, STATX_BTIME, &recorded) != 0 ||
        !(recorded.stx_mask & STATX_BTIME)) {
        GTEST_SKIP() << "the file system records no creation time";
    }
    EXPECT_NEAR(static_cast<double>(stat->created_ms), static_cast<double>(now_ms()), 60000.0);
}

TEST_F(FileSystemTest, ADirectoryHasTheDirectoryTypeAndNoSize) {
    const auto stat = stat_path(directory("d"));
    ASSERT_TRUE(stat.has_value());
    EXPECT_EQ(stat->type, file_type::directory);
    EXPECT_EQ(stat->size, 0U);
}

TEST_F(FileSystemTest, AReadOnlyFileSaysSo) {
    const fs::path path = write("a.txt");
    fs::permissions(path, fs::perms::owner_read, fs::perm_options::replace);
    EXPECT_TRUE(stat_path(path)->read_only);
}

TEST_F(FileSystemTest, ALinkIsDescribedByItsTargetWithTheLinkBitAdded) {
    const fs::path file = write("target.txt", "12345");
    const fs::path folder = directory("folder");
    const auto to_file = stat_path(link("to-file", file));
    const auto to_folder = stat_path(link("to-folder", folder));
    ASSERT_TRUE(to_file.has_value());
    ASSERT_TRUE(to_folder.has_value());
    EXPECT_EQ(to_file->type, file_type::file | file_type::symbolic_link);
    EXPECT_EQ(to_file->size, 5U);
    EXPECT_EQ(to_folder->type, file_type::directory | file_type::symbolic_link);
}

TEST_F(FileSystemTest, ABrokenLinkHasTheLinkBitAlone) {
    const auto stat = stat_path(link("broken", directory_ / "nothing"));
    ASSERT_TRUE(stat.has_value()) << stat.error().message;
    EXPECT_EQ(stat->type, file_type::symbolic_link);
    EXPECT_EQ(stat->size, 0U);
    EXPECT_NEAR(static_cast<double>(stat->modified_ms), static_cast<double>(now_ms()), 60000.0);
}

TEST_F(FileSystemTest, AMissingPathIsNotFoundAndNamesThePath) {
    const auto stat = stat_path(directory_ / "missing" / "file.txt");
    ASSERT_FALSE(stat.has_value());
    EXPECT_EQ(stat.error().code, ErrorCode::not_found);
    EXPECT_NE(stat.error().message.find("missing"), std::string::npos);
}

TEST_F(FileSystemTest, APathBeyondAFileIsNotADirectory) {
    write("a.txt");
    const auto stat = stat_path(directory_ / "a.txt" / "child");
    ASSERT_FALSE(stat.has_value());
    EXPECT_EQ(stat.error().code, ErrorCode::not_a_directory);
}

TEST_F(FileSystemTest, ADirectoryThatCannotBeSearchedIsPermissionDenied) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "the permissions of the super user do not stop it";
    }
    const fs::path inner = write("closed/a.txt");
    fs::permissions(inner.parent_path(), fs::perms::none, fs::perm_options::replace);
    const auto stat = stat_path(inner);
    ASSERT_FALSE(stat.has_value());
    EXPECT_EQ(stat.error().code, ErrorCode::permission_denied);
}

TEST_F(FileSystemTest, NamesWithSpacesAndOtherScriptsAreStatted) {
    const fs::path path = write(
        "a b/\xD0\xB4\xD0\xB0\xD0\xBD\xD0\xBD\xD1\x8B\xD0\xB5 \xE6\x97\xA5\xE6\x9C\xAC.txt", "xyz");
    EXPECT_EQ(stat_path(path)->size, 3U);
}

// --- list ---

TEST_F(FileSystemTest, ListingGivesTheEntriesSortedByNameWithTheirTypes) {
    const fs::path folder = directory("d");
    write("d/b.txt");
    directory("d/a");
    fs::create_symlink(directory_ / "d" / "b.txt", folder / "c-link");
    fs::create_symlink(directory_ / "nothing", folder / "d-broken");
    const auto list = list_directory(folder);
    ASSERT_TRUE(list.has_value()) << list.error().message;
    ASSERT_EQ(list->size(), 4U);
    EXPECT_EQ((*list)[0].name, "a");
    EXPECT_EQ((*list)[0].type, file_type::directory);
    EXPECT_EQ((*list)[1].name, "b.txt");
    EXPECT_EQ((*list)[1].type, file_type::file);
    EXPECT_EQ((*list)[2].name, "c-link");
    EXPECT_EQ((*list)[2].type, file_type::file | file_type::symbolic_link);
    EXPECT_EQ((*list)[3].name, "d-broken");
    EXPECT_EQ((*list)[3].type, file_type::symbolic_link);
}

TEST_F(FileSystemTest, AnEmptyDirectoryHasNoEntries) {
    const auto list = list_directory(directory("empty"));
    ASSERT_TRUE(list.has_value());
    EXPECT_TRUE(list->empty());
}

TEST_F(FileSystemTest, ANameThatIsNotUtf8IsListedAsItIsOnDisk) {
    const std::string name = "bad-\xFF\xFE-name";
    write("d/" + name);
    const auto list = list_directory(directory_ / "d");
    ASSERT_TRUE(list.has_value());
    ASSERT_EQ(list->size(), 1U);
    EXPECT_EQ((*list)[0].name, name);
}

TEST_F(FileSystemTest, ListingAFileIsNotADirectoryAndAMissingOneIsNotFound) {
    const fs::path file = write("a.txt");
    const auto of_file = list_directory(file);
    ASSERT_FALSE(of_file.has_value());
    EXPECT_EQ(of_file.error().code, ErrorCode::not_a_directory);
    const auto missing = list_directory(directory_ / "missing");
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code, ErrorCode::not_found);
}

// --- create ---

TEST_F(FileSystemTest, CreatingADirectoryCreatesItsParents) {
    ASSERT_TRUE(make_directory(directory_ / "a" / "b" / "c").has_value());
    EXPECT_TRUE(fs::is_directory(directory_ / "a" / "b" / "c"));
}

TEST_F(FileSystemTest, CreatingADirectoryThatExistsSucceeds) {
    directory("a");
    EXPECT_TRUE(make_directory(directory_ / "a").has_value());
    const fs::path through_link = link("to-a", directory_ / "a");
    EXPECT_TRUE(make_directory(through_link).has_value());
}

TEST_F(FileSystemTest, CreatingADirectoryWhereAFileIsFails) {
    write("a");
    EXPECT_EQ(code_of(make_directory(directory_ / "a")), ErrorCode::already_exists);
    EXPECT_EQ(code_of(make_directory(directory_ / "a" / "b")), ErrorCode::not_a_directory);
    EXPECT_TRUE(fs::is_regular_file(directory_ / "a"));
}

// --- remove ---

TEST_F(FileSystemTest, RemovingAFileAnEmptyDirectoryAndALinkWorks) {
    const fs::path file = write("a.txt");
    const fs::path empty = directory("empty");
    EXPECT_TRUE(remove_path(file, false).has_value());
    EXPECT_TRUE(remove_path(empty, false).has_value());
    EXPECT_FALSE(fs::exists(file));
    EXPECT_FALSE(fs::exists(empty));
}

TEST_F(FileSystemTest, RemovingALinkRemovesTheLinkAndNotItsTarget) {
    const fs::path folder = directory("folder");
    write("folder/keep.txt");
    const fs::path to_folder = link("to-folder", folder);
    EXPECT_TRUE(remove_path(to_folder, true).has_value());
    EXPECT_FALSE(fs::exists(fs::symlink_status(to_folder)));
    EXPECT_TRUE(fs::exists(folder / "keep.txt"));

    const fs::path file = write("file.txt");
    const fs::path to_file = link("to-file", file);
    EXPECT_TRUE(remove_path(to_file, false).has_value());
    EXPECT_TRUE(fs::exists(file));
}

TEST_F(FileSystemTest, ANonEmptyDirectoryNeedsTheRecursiveFlag) {
    write("d/a.txt");
    const auto failed = remove_path(directory_ / "d", false);
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code, ErrorCode::io_error);
    EXPECT_TRUE(fs::exists(directory_ / "d" / "a.txt"));

    EXPECT_TRUE(remove_path(directory_ / "d", true).has_value());
    EXPECT_FALSE(fs::exists(directory_ / "d"));
}

TEST_F(FileSystemTest, ARecursiveRemoveDoesNotFollowALinkOutOfTheTree) {
    const fs::path outside = directory("outside");
    write("outside/keep.txt");
    write("tree/a/b.txt");
    fs::create_symlink(outside, directory_ / "tree" / "a" / "escape");
    ASSERT_TRUE(remove_path(directory_ / "tree", true).has_value());
    EXPECT_FALSE(fs::exists(directory_ / "tree"));
    EXPECT_TRUE(fs::exists(outside / "keep.txt"));
}

TEST_F(FileSystemTest, RemovingAMissingPathIsNotFound) {
    EXPECT_EQ(code_of(remove_path(directory_ / "missing", true)), ErrorCode::not_found);
}

// --- rename ---

TEST_F(FileSystemTest, RenamingAFileAndADirectoryMovesThem) {
    const fs::path file = write("a.txt", "content");
    write("d/inner.txt");
    ASSERT_TRUE(rename_path(file, directory_ / "b.txt", false).has_value());
    EXPECT_FALSE(fs::exists(file));
    EXPECT_EQ(contents(directory_ / "b.txt"), "content");
    ASSERT_TRUE(rename_path(directory_ / "d", directory_ / "e", false).has_value());
    EXPECT_TRUE(fs::exists(directory_ / "e" / "inner.txt"));
}

TEST_F(FileSystemTest, ATargetThatExistsStopsARenameWithoutOverwrite) {
    const fs::path a = write("a.txt", "a");
    const fs::path b = write("b.txt", "b");
    EXPECT_EQ(code_of(rename_path(a, b, false)), ErrorCode::already_exists);
    EXPECT_EQ(contents(a), "a");
    EXPECT_EQ(contents(b), "b");
}

TEST_F(FileSystemTest, OverwritingReplacesAFileADirectoryAndATargetOfAnotherType) {
    const fs::path a = write("a.txt", "new");
    write("b.txt", "old");
    ASSERT_TRUE(rename_path(a, directory_ / "b.txt", true).has_value());
    EXPECT_EQ(contents(directory_ / "b.txt"), "new");

    write("src/s.txt", "s");
    write("dst/old.txt", "old");
    ASSERT_TRUE(rename_path(directory_ / "src", directory_ / "dst", true).has_value());
    EXPECT_TRUE(fs::exists(directory_ / "dst" / "s.txt"));
    EXPECT_FALSE(fs::exists(directory_ / "dst" / "old.txt"));

    write("dir-src/s.txt");
    write("file-dst", "file");
    ASSERT_TRUE(rename_path(directory_ / "dir-src", directory_ / "file-dst", true).has_value());
    EXPECT_TRUE(fs::is_directory(directory_ / "file-dst"));
}

TEST_F(FileSystemTest, RenamingToItselfChangesNothing) {
    const fs::path a = write("a.txt", "a");
    EXPECT_TRUE(rename_path(a, a, false).has_value());
    EXPECT_TRUE(rename_path(a, directory_ / "." / "a.txt", false).has_value());
    EXPECT_EQ(contents(a), "a");
}

TEST_F(FileSystemTest, RenamingAMissingSourceIsNotFound) {
    EXPECT_EQ(code_of(rename_path(directory_ / "missing", directory_ / "b", true)),
              ErrorCode::not_found);
}

TEST_F(FileSystemTest, OverwritingATargetThatContainsTheSourceIsRefusedAndNothingIsLost) {
    write("outer/inner/a.txt", "keep");
    EXPECT_EQ(code_of(rename_path(directory_ / "outer" / "inner", directory_ / "outer", true)),
              ErrorCode::invalid_argument);
    EXPECT_EQ(contents(directory_ / "outer" / "inner" / "a.txt"), "keep");
}

// --- copy ---

TEST_F(FileSystemTest, CopyingAFileKeepsTheBytesAndTheSource) {
    const std::string bytes("a\0b\xFF\r\n", 6);
    const fs::path a = write("a.bin", bytes);
    ASSERT_TRUE(copy_path(a, directory_ / "b.bin", false).has_value());
    EXPECT_EQ(contents(directory_ / "b.bin"), bytes);
    EXPECT_EQ(contents(a), bytes);
}

TEST_F(FileSystemTest, CopyingADirectoryCopiesEverythingInIt) {
    write("d/a.txt", "a");
    write("d/sub/b.txt", "b");
    ASSERT_TRUE(copy_path(directory_ / "d", directory_ / "e", false).has_value());
    EXPECT_EQ(contents(directory_ / "e" / "a.txt"), "a");
    EXPECT_EQ(contents(directory_ / "e" / "sub" / "b.txt"), "b");
    EXPECT_TRUE(fs::exists(directory_ / "d" / "sub" / "b.txt"));
}

TEST_F(FileSystemTest, ALinkIsCopiedAsALink) {
    const fs::path file = write("file.txt");
    const fs::path original = link("original", file);
    ASSERT_TRUE(copy_path(original, directory_ / "copy", false).has_value());
    EXPECT_TRUE(fs::is_symlink(directory_ / "copy"));
    EXPECT_EQ(fs::read_symlink(directory_ / "copy"), file);
}

TEST_F(FileSystemTest, ATargetThatExistsStopsACopyWithoutOverwrite) {
    const fs::path a = write("a.txt", "a");
    const fs::path b = write("b.txt", "b");
    EXPECT_EQ(code_of(copy_path(a, b, false)), ErrorCode::already_exists);
    EXPECT_EQ(contents(b), "b");
}

TEST_F(FileSystemTest, CopyingWithOverwriteReplacesTheTarget) {
    const fs::path a = write("a.txt", "new");
    write("b.txt", "old");
    ASSERT_TRUE(copy_path(a, directory_ / "b.txt", true).has_value());
    EXPECT_EQ(contents(directory_ / "b.txt"), "new");

    write("src/s.txt", "s");
    write("dst/old.txt", "old");
    ASSERT_TRUE(copy_path(directory_ / "src", directory_ / "dst", true).has_value());
    EXPECT_TRUE(fs::exists(directory_ / "dst" / "s.txt"));
    EXPECT_FALSE(fs::exists(directory_ / "dst" / "old.txt"));
}

TEST_F(FileSystemTest, ADirectoryCannotBeCopiedIntoItself) {
    write("d/a.txt");
    EXPECT_EQ(code_of(copy_path(directory_ / "d", directory_ / "d" / "inner", false)),
              ErrorCode::invalid_argument);
    EXPECT_EQ(code_of(copy_path(directory_ / "d", directory_ / "d", true)),
              ErrorCode::invalid_argument);
    EXPECT_TRUE(fs::exists(directory_ / "d" / "a.txt"));
}

TEST_F(FileSystemTest, OverwritingATargetThatContainsTheSourceIsRefusedForACopyToo) {
    write("outer/inner/a.txt", "keep");
    EXPECT_EQ(
        code_of(copy_path(directory_ / "outer" / "inner" / "a.txt", directory_ / "outer", true)),
        ErrorCode::invalid_argument);
    EXPECT_EQ(contents(directory_ / "outer" / "inner" / "a.txt"), "keep");
}

TEST_F(FileSystemTest, CopyingAMissingSourceIsNotFound) {
    EXPECT_EQ(code_of(copy_path(directory_ / "missing", directory_ / "b", false)),
              ErrorCode::not_found);
}
