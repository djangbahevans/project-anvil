#include "anvil/platform/file.hpp"

#include <filesystem>
#include <gtest/gtest.h>
#include <unistd.h>

using namespace anvil::platform;

namespace {

class FileTest : public ::testing::Test {
protected:
    void SetUp() override {
        directory_ = std::filesystem::temp_directory_path() /
                     ("anvil-file-test-" + std::to_string(::getpid()));
        std::filesystem::create_directories(directory_);
    }
    void TearDown() override {
        std::filesystem::remove_all(directory_);
    }
    std::filesystem::path directory_;
};

} // namespace

TEST_F(FileTest, WrittenContentsReadBackExactly) {
    const std::string data("bin\0ary\r\n\xFF", 11);
    ASSERT_TRUE(write_file(directory_ / "a.bin", data).has_value());
    const auto read = read_file(directory_ / "a.bin");
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(*read, data);
}

TEST_F(FileTest, WritingReplacesExistingContentsAndLeavesNoTemporaryFile) {
    ASSERT_TRUE(write_file(directory_ / "a.txt", "long original contents").has_value());
    ASSERT_TRUE(write_file(directory_ / "a.txt", "short").has_value());
    EXPECT_EQ(*read_file(directory_ / "a.txt"), "short");
    std::size_t files = 0;
    for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator(directory_)) {
        ++files;
    }
    EXPECT_EQ(files, 1u);
}

TEST_F(FileTest, ReadingFailsForMissingFilesAndDirectories) {
    EXPECT_EQ(read_file(directory_ / "missing").error().code, anvil::ErrorCode::io_error);
    EXPECT_FALSE(read_file(directory_).has_value());
}

TEST_F(FileTest, WritingFailsWhenTheDirectoryIsMissing) {
    EXPECT_FALSE(write_file(directory_ / "no" / "such" / "dir.txt", "x").has_value());
}

TEST_F(FileTest, ReadsARangeOfAFile) {
    ASSERT_TRUE(write_file(directory_ / "r.txt", "0123456789").has_value());
    EXPECT_EQ(*read_file_range(directory_ / "r.txt", 2, 4), "2345");
    EXPECT_EQ(*read_file_range(directory_ / "r.txt", 8, 100), "89");
    EXPECT_EQ(*read_file_range(directory_ / "r.txt", 10, 5), "");
    EXPECT_EQ(*read_file_range(directory_ / "r.txt", 50, 5), "");
    EXPECT_FALSE(read_file_range(directory_ / "missing", 0, 5).has_value());
}

TEST_F(FileTest, ReadingAFileThatCannotBeOpenedSaysWhy) {
    const auto missing = read_file_range(directory_ / "missing.txt", 0, 10);
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code, anvil::ErrorCode::not_found);

    if (::geteuid() != 0) {
        const auto path = directory_ / "secret.txt";
        ASSERT_TRUE(write_file(path, "x").has_value());
        std::filesystem::permissions(path, std::filesystem::perms::none);
        const auto denied = read_file(path);
        ASSERT_FALSE(denied.has_value());
        EXPECT_EQ(denied.error().code, anvil::ErrorCode::permission_denied);
        const auto denied_range = read_file_range(path, 0, 1);
        ASSERT_FALSE(denied_range.has_value());
        EXPECT_EQ(denied_range.error().code, anvil::ErrorCode::permission_denied);
        std::filesystem::permissions(path, std::filesystem::perms::owner_all);
    }
}

TEST_F(FileTest, WritingWhereTheDirectoryIsMissingOrAFileSaysWhy) {
    const auto missing = write_file(directory_ / "no" / "such" / "a.txt", "x");
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code, anvil::ErrorCode::not_found);

    ASSERT_TRUE(write_file(directory_ / "file", "x").has_value());
    const auto beyond_a_file = write_file(directory_ / "file" / "a.txt", "x");
    ASSERT_FALSE(beyond_a_file.has_value());
    EXPECT_EQ(beyond_a_file.error().code, anvil::ErrorCode::not_a_directory);
}
