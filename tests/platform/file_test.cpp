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
