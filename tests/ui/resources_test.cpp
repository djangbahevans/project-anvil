#include "anvil/ui/resources.hpp"

#include <fstream>
#include <gtest/gtest.h>
#include <unistd.h>

namespace {

class ThemesDirectoryTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path() /
                ("anvil-resources-test-" + std::to_string(::getpid()));
        std::filesystem::create_directories(root_);
    }
    void TearDown() override {
        std::filesystem::remove_all(root_);
    }

    // A directory with a themes.json in it.
    std::filesystem::path make_themes(const std::filesystem::path& relative) {
        const std::filesystem::path directory = root_ / relative;
        std::filesystem::create_directories(directory);
        std::ofstream(directory / "themes.json") << "[]";
        return directory;
    }

    std::filesystem::path root_;
};

} // namespace

TEST_F(ThemesDirectoryTest, AnInstalledEditorUsesShareNextToBin) {
    const auto installed = make_themes("prefix/share/anvil/themes");
    const auto source = make_themes("source/resources/themes");
    std::filesystem::create_directories(root_ / "prefix" / "bin");
    EXPECT_EQ(anvil::ui::themes_directory(root_ / "prefix" / "bin", source),
              std::filesystem::weakly_canonical(installed));
}

TEST_F(ThemesDirectoryTest, ABuildDirectoryUsesTheSourceTree) {
    const auto source = make_themes("source/resources/themes");
    std::filesystem::create_directories(root_ / "build" / "bin");
    EXPECT_EQ(anvil::ui::themes_directory(root_ / "build" / "bin", source), source);
}

TEST_F(ThemesDirectoryTest, ADirectoryWithoutThemesJsonDoesNotCount) {
    std::filesystem::create_directories(root_ / "prefix" / "share" / "anvil" / "themes");
    std::filesystem::create_directories(root_ / "prefix" / "bin");
    const auto source = make_themes("source/resources/themes");
    EXPECT_EQ(anvil::ui::themes_directory(root_ / "prefix" / "bin", source), source);
}

TEST_F(ThemesDirectoryTest, NothingFoundGivesAnEmptyPath) {
    std::filesystem::create_directories(root_ / "bin");
    EXPECT_TRUE(anvil::ui::themes_directory(root_ / "bin", root_ / "nowhere").empty());
    EXPECT_TRUE(anvil::ui::themes_directory({}, {}).empty());
}

TEST_F(ThemesDirectoryTest, TheRunningApplicationFindsTheThemesOfTheSourceTreeItWasBuiltFrom) {
    const std::filesystem::path directory = anvil::ui::themes_directory();
    ASSERT_FALSE(directory.empty());
    EXPECT_TRUE(std::filesystem::exists(directory / "themes.json"));
}
