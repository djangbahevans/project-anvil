#include "anvil/platform/file.hpp"
#include "anvil/ui/about.hpp"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <regex>
#include <sstream>
#include <unistd.h>

namespace {

using namespace anvil;

ui::AboutVersions all_versions() {
    return {.anvil = "1.2.3", .qt_running = "6.4.2", .qt_built_with = "6.4.2", .vscode = "1.99.0"};
}

class AboutTextTest : public ::testing::Test {
protected:
    void SetUp() override {
        scratch_ = std::filesystem::temp_directory_path() /
                   ("anvil-about-test-" + std::to_string(::getpid()));
        std::filesystem::create_directories(scratch_);
    }
    void TearDown() override {
        std::filesystem::remove_all(scratch_);
    }
    void write(std::string_view text) {
        ASSERT_TRUE(platform::write_file(scratch_ / "package.json", text).has_value());
    }
    std::filesystem::path scratch_;
};

TEST_F(AboutTextTest, EveryVersionIsInTheText) {
    const QString text = ui::about_text(all_versions());
    EXPECT_TRUE(text.contains("Version: 1.2.3")) << text.toStdString();
    EXPECT_TRUE(text.contains("Qt: 6.4.2\n")) << text.toStdString();
    EXPECT_TRUE(text.contains("VS Code extension host: 1.99.0")) << text.toStdString();
    EXPECT_TRUE(text.contains("MIT license")) << text.toStdString();
    EXPECT_TRUE(text.contains("https://github.com/djangbahevans/project-anvil"));
    EXPECT_FALSE(text.contains("not available"));
}

TEST_F(AboutTextTest, ADifferentQtThatTheOneItWasBuiltWithIsSaid) {
    auto versions = all_versions();
    versions.qt_running = "6.8.1";
    const QString text = ui::about_text(versions);
    EXPECT_TRUE(text.contains("Qt: 6.8.1 (built with 6.4.2)")) << text.toStdString();
}

TEST_F(AboutTextTest, AMissingVersionReadsNotAvailableAndTheOthersStay) {
    const auto without = [](std::string ui::AboutVersions::*member) {
        auto versions = all_versions();
        versions.*member = "";
        return ui::about_text(versions);
    };
    QString text = without(&ui::AboutVersions::anvil);
    EXPECT_TRUE(text.contains("Version: not available")) << text.toStdString();
    EXPECT_TRUE(text.contains("Qt: 6.4.2")) << text.toStdString();
    EXPECT_TRUE(text.contains("extension host: 1.99.0")) << text.toStdString();

    text = without(&ui::AboutVersions::vscode);
    EXPECT_TRUE(text.contains("VS Code extension host: not available")) << text.toStdString();
    EXPECT_TRUE(text.contains("Version: 1.2.3")) << text.toStdString();

    text = without(&ui::AboutVersions::qt_running);
    EXPECT_TRUE(text.contains("Qt: not available (built with 6.4.2)")) << text.toStdString();

    text = without(&ui::AboutVersions::qt_built_with);
    EXPECT_TRUE(text.contains("Qt: 6.4.2 (built with not available)")) << text.toStdString();
}

TEST_F(AboutTextTest, NothingIsEmpty) {
    const QString text = ui::about_text({});
    EXPECT_TRUE(text.contains("Version: not available")) << text.toStdString();
    EXPECT_FALSE(text.contains("Version: \n"));
    EXPECT_FALSE(text.contains("host: \n"));
}

TEST_F(AboutTextTest, TheVersionOfTheVsCodeDirectoryIsItsPackageJsonVersion) {
    write(R"j({"name": "code-oss-dev", "version": "1.146.0", "main": "./out/main"})j");
    EXPECT_EQ(ui::read_vscode_version(scratch_), "1.146.0");
}

TEST_F(AboutTextTest, ADirectoryWithoutAUsableVersionHasNone) {
    EXPECT_EQ(ui::read_vscode_version(scratch_), std::nullopt);
    EXPECT_EQ(ui::read_vscode_version(scratch_ / "missing"), std::nullopt);
    for (const char* text : {"", "not json", "[]", R"j({"name": "x"})j", R"j({"version": 3})j",
                             R"j({"version": ""})j", R"j({"version": null})j"}) {
        write(text);
        EXPECT_EQ(ui::read_vscode_version(scratch_), std::nullopt) << text;
    }
}

TEST_F(AboutTextTest, TheVersionsOfABuildHaveEveryPartButTheVsCodeOneWithoutADirectory) {
    const auto versions = ui::about_versions(std::nullopt);
    EXPECT_EQ(versions.anvil, ui::anvil_version());
    EXPECT_EQ(versions.qt_running, qVersion());
    EXPECT_EQ(versions.qt_built_with, QT_VERSION_STR);
    EXPECT_EQ(versions.vscode, "");

    write(R"j({"version": "7.8.9"})j");
    EXPECT_EQ(ui::about_versions(scratch_).vscode, "7.8.9");
    EXPECT_EQ(ui::about_versions(scratch_ / "missing").vscode, "");
}

TEST_F(AboutTextTest, TheVersionOfAnvilIsTheOneOfTheProjectCall) {
    std::ifstream file(ANVIL_CMAKELISTS);
    ASSERT_TRUE(file.good()) << ANVIL_CMAKELISTS;
    std::stringstream text;
    text << file.rdbuf();
    std::smatch match;
    const std::string source = text.str();
    ASSERT_TRUE(std::regex_search(source, match,
                                  std::regex(R"(project\(anvil\s+VERSION\s+([0-9][0-9.]*))")));
    EXPECT_EQ(ui::anvil_version(), match[1].str());
}

} // namespace
