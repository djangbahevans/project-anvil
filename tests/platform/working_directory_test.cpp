#include "anvil/platform/working_directory.hpp"

#include <cstdlib>
#include <filesystem>
#include <gtest/gtest.h>
#include <unistd.h>

using anvil::platform::ensure_working_directory;
namespace fs = std::filesystem;

namespace {

// Puts the test process back where it was and removes what the test made.
class WorkingDirectoryTest : public ::testing::Test {
protected:
    void SetUp() override {
        saved_ = fs::current_path();
        scratch_ = fs::canonical(fs::temp_directory_path()) /
                   ("anvil-working-directory-" + std::to_string(::getpid()));
        fs::create_directories(scratch_ / "fallback");
    }
    void TearDown() override {
        std::error_code ignored;
        fs::current_path(saved_, ignored);
        fs::remove_all(scratch_, ignored);
    }

    // Makes the process's working directory one that has been deleted.
    void delete_working_directory() {
        const fs::path doomed = scratch_ / "doomed";
        fs::create_directories(doomed);
        fs::current_path(doomed);
        fs::remove(doomed);
        std::error_code error;
        const fs::path unused = fs::current_path(error);
        ASSERT_TRUE(error) << "the directory should be gone, but the working directory is "
                           << unused;
    }

    fs::path saved_;
    fs::path scratch_;
};

} // namespace

TEST_F(WorkingDirectoryTest, AValidDirectoryIsLeftAlone) {
    const auto result = ensure_working_directory({scratch_ / "fallback"});
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_FALSE(result->changed);
    EXPECT_EQ(result->directory, saved_);
    EXPECT_EQ(fs::current_path(), saved_);
}

TEST_F(WorkingDirectoryTest, ADeletedDirectoryIsReplacedByTheFirstUsableFallback) {
    ASSERT_NO_FATAL_FAILURE(delete_working_directory());
    const auto result =
        ensure_working_directory({scratch_ / "missing", scratch_ / "fallback", scratch_});
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_TRUE(result->changed);
    EXPECT_EQ(result->directory, scratch_ / "fallback");
    EXPECT_EQ(fs::current_path(), scratch_ / "fallback");
}

TEST_F(WorkingDirectoryTest, ItFailsWhenNoFallbackCanBeEntered) {
    ASSERT_NO_FATAL_FAILURE(delete_working_directory());
    const auto result = ensure_working_directory({scratch_ / "missing", scratch_ / "other"});
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("working directory"), std::string::npos)
        << result.error().message;
}

TEST_F(WorkingDirectoryTest, TheDefaultFallbacksAreTheHomeDirectoryThenTheTemporaryDirectory) {
    const char* previous_home = std::getenv("HOME");
    const std::string saved_home = previous_home != nullptr ? previous_home : "";

    ASSERT_NO_FATAL_FAILURE(delete_working_directory());
    ::setenv("HOME", (scratch_ / "fallback").c_str(), 1);
    auto result = ensure_working_directory();
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->directory, scratch_ / "fallback");

    ASSERT_NO_FATAL_FAILURE(delete_working_directory());
    ::setenv("HOME", (scratch_ / "no-such-home").c_str(), 1);
    result = ensure_working_directory();
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_EQ(result->directory, fs::canonical(fs::temp_directory_path()));

    if (previous_home != nullptr) {
        ::setenv("HOME", saved_home.c_str(), 1);
    } else {
        ::unsetenv("HOME");
    }
}
