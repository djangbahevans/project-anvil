#include "anvil/exthost/init_data.hpp"

#include <fstream>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace anvil::exthost;
using nlohmann::json;

namespace {

class InitData : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path() / "anvil-init-data-test";
        std::filesystem::remove_all(root_);
        std::filesystem::create_directories(root_);
    }
    void TearDown() override {
        std::filesystem::remove_all(root_);
    }

    std::filesystem::path write_extension(const std::string& directory,
                                          const std::string& manifest) {
        const auto dir = root_ / directory;
        std::filesystem::create_directories(dir);
        std::ofstream(dir / "package.json") << manifest;
        return dir;
    }

    std::filesystem::path root_;
};

} // namespace

TEST_F(InitData, DescribesTheExtensionsFoundInTheGivenDirectories) {
    const auto dir = write_extension(
        "a",
        R"({"name":"Hello","publisher":"Acme","version":"1.0.0","activationEvents":["onCommand:x"]})");

    const auto text =
        build_init_data({.app_root = "/app", .extension_dirs = {dir}, .storage_dir = "/storage"});
    ASSERT_TRUE(text.has_value()) << text.error().message;
    const json data = json::parse(*text);

    const auto& extension = data["extensions"]["allExtensions"][0];
    EXPECT_EQ(extension["identifier"], (json{{"value", "Acme.Hello"}, {"_lower", "acme.hello"}}));
    EXPECT_EQ(extension["extensionLocation"]["path"], dir.generic_string());
    EXPECT_EQ(extension["extensionLocation"]["scheme"], "file");
    EXPECT_EQ(extension["name"], "Hello");
    EXPECT_FALSE(extension["isBuiltin"].get<bool>());
    EXPECT_FALSE(extension["targetPlatform"].get<std::string>().empty());
    EXPECT_EQ(data["extensions"]["activationEvents"]["Acme.Hello"],
              json::parse(R"(["onCommand:x"])"));
    EXPECT_EQ(data["extensions"]["myExtensions"][0]["_lower"], "acme.hello");
}

TEST_F(InitData, ExtensionsWithoutActivationEventsGetAnEmptyList) {
    const auto dir = write_extension("a", R"({"name":"n","publisher":"p"})");
    const auto text = build_init_data({.extension_dirs = {dir}});
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(json::parse(*text)["extensions"]["activationEvents"]["p.n"], json::array());
}

TEST_F(InitData, ReportsTheEditorAndStorageLocations) {
    const auto text = build_init_data({.app_root = "/app", .storage_dir = "/storage"});
    ASSERT_TRUE(text.has_value());
    const json data = json::parse(*text);
    EXPECT_EQ(data["environment"]["appName"], "Anvil");
    EXPECT_EQ(data["environment"]["appRoot"]["path"], "/app");
    EXPECT_EQ(data["environment"]["globalStorageHome"]["path"], "/storage/globalStorage");
    EXPECT_EQ(data["logsLocation"]["path"], "/storage/logs");
    EXPECT_TRUE(data["autoStart"].get<bool>());
    EXPECT_GT(data["parentPid"].get<long>(), 0);
}

TEST_F(InitData, FailsForMissingOrIncompleteManifests) {
    EXPECT_FALSE(build_init_data({.extension_dirs = {root_ / "missing"}}).has_value());
    const auto no_publisher = write_extension("b", R"({"name":"n"})");
    const auto result = build_init_data({.extension_dirs = {no_publisher}});
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("publisher"), std::string::npos);
    const auto broken = write_extension("c", "{ not json");
    EXPECT_FALSE(build_init_data({.extension_dirs = {broken}}).has_value());
}

TEST_F(InitData, TheLogsLocationIsTheGivenSessionDirectoryWhenThereIsOne) {
    InitDataOptions options;
    options.storage_dir = "/storage";
    options.logs_dir = "/storage/logs/20261121T143358Z-1234";
    const auto text = build_init_data(options);
    ASSERT_TRUE(text.has_value()) << text.error().message;
    EXPECT_EQ(json::parse(*text)["logsLocation"]["path"], "/storage/logs/20261121T143358Z-1234");
}

TEST_F(InitData, HasNoWorkspaceWithoutAFolder) {
    const auto text = build_init_data({.app_root = "/app", .storage_dir = "/storage"});
    ASSERT_TRUE(text.has_value()) << text.error().message;
    EXPECT_TRUE(json::parse(*text)["workspace"].is_null());
    EXPECT_EQ(workspace_initialization_args(std::nullopt), "[null,true]");
}

TEST_F(InitData, DescribesTheWorkspaceFolderForStorage) {
    const auto folder = root_ / "project";
    std::filesystem::create_directories(folder);
    const auto workspace = anvil::core::Workspace::open(folder);
    ASSERT_TRUE(workspace.has_value());

    const auto text =
        build_init_data({.app_root = "/app", .storage_dir = "/storage", .workspace = *workspace});
    ASSERT_TRUE(text.has_value()) << text.error().message;
    const json data = json::parse(*text)["workspace"];
    EXPECT_EQ(data["id"], workspace->id);
    EXPECT_EQ(data["name"], "project");
    EXPECT_EQ(data["isUntitled"], false);
    EXPECT_EQ(data["transient"], false);
}

TEST_F(InitData, SendsTheWorkspaceFolderAsTheOnlyFolderOfATrustedWorkspace) {
    const auto folder = root_ / "project";
    std::filesystem::create_directories(folder);
    const auto workspace = anvil::core::Workspace::open(folder);
    ASSERT_TRUE(workspace.has_value());

    const json args = json::parse(workspace_initialization_args(*workspace));
    ASSERT_EQ(args.size(), 2u);
    EXPECT_EQ(args[1], true);
    const json& data = args[0];
    EXPECT_EQ(data["id"], workspace->id);
    EXPECT_EQ(data["name"], "project");
    EXPECT_EQ(data["isUntitled"], false);
    EXPECT_EQ(data["transient"], false);
    ASSERT_EQ(data["folders"].size(), 1u);
    EXPECT_EQ(data["folders"][0]["name"], "project");
    EXPECT_EQ(data["folders"][0]["index"], 0);
    EXPECT_EQ(
        data["folders"][0]["uri"],
        (json{{"$mid", 1}, {"scheme", "file"}, {"path", workspace->folder.generic_string()}}));
}
