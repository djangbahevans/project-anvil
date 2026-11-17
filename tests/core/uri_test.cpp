#include "anvil/core/uri.hpp"

#include <gtest/gtest.h>

using anvil::core::Uri;

TEST(Uri, FileUrisHaveForwardSlashPathsAndAnEmptyAuthority) {
    const Uri uri = Uri::file("/home/me/a.txt");
    EXPECT_EQ(uri.scheme, "file");
    EXPECT_EQ(uri.authority, "");
    EXPECT_EQ(uri.path, "/home/me/a.txt");
    EXPECT_EQ(uri.to_string(), "file:///home/me/a.txt");
}

TEST(Uri, FilePathRoundTrips) {
    EXPECT_EQ(Uri::file("/tmp/x y/z.txt").file_path(), std::filesystem::path("/tmp/x y/z.txt"));
}

TEST(Uri, WindowsDriveLettersUseTheLowercaseSlashPrefixedForm) {
    const Uri uri = Uri::file("C:/Users/me/a.txt");
    EXPECT_EQ(uri.path, "/c:/Users/me/a.txt");
    EXPECT_EQ(uri.file_path(), std::filesystem::path("c:/Users/me/a.txt"));
}

TEST(Uri, OtherSchemesHaveNoFilePath) {
    EXPECT_EQ((Uri{"untitled", "", "Untitled-1", "", ""}).file_path(), std::nullopt);
    EXPECT_EQ((Uri{"https", "example.com", "/a", "q=1", "top"}).to_string(),
              "https://example.com/a?q=1#top");
    EXPECT_EQ((Uri{"untitled", "", "Untitled-1", "", ""}).to_string(), "untitled:Untitled-1");
}

TEST(Uri, ComparesByComponents) {
    EXPECT_EQ(Uri::file("/a"), Uri::file("/a"));
    EXPECT_NE(Uri::file("/a"), Uri::file("/b"));
}
