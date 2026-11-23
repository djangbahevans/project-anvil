#include "anvil/core/workspace.hpp"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <unistd.h>

using anvil::ErrorCode;
using anvil::core::Workspace;
namespace fs = std::filesystem;

class WorkspaceTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = fs::weakly_canonical(fs::temp_directory_path()) /
                ("anvil-workspace-test-" + std::to_string(::getpid()));
        fs::remove_all(root_);
        fs::create_directories(root_ / "project" / "src");
        fs::create_directories(root_ / "other");
    }
    void TearDown() override {
        fs::remove_all(root_);
    }

    fs::path root_;
};

TEST_F(WorkspaceTest, HoldsTheCanonicalFolderAndItsName) {
    const auto workspace = Workspace::open(root_ / "project");
    ASSERT_TRUE(workspace.has_value()) << workspace.error().message;
    EXPECT_EQ(workspace->folder, root_ / "project");
    EXPECT_EQ(workspace->name, "project");
}

TEST_F(WorkspaceTest, TheSameFolderHasTheSameId) {
    const auto first = Workspace::open(root_ / "project");
    const auto second = Workspace::open(root_ / "project");
    ASSERT_TRUE(first && second);
    EXPECT_EQ(first->id, second->id);
    EXPECT_EQ(first->id.size(), 16u);
}

TEST_F(WorkspaceTest, DifferentFoldersHaveDifferentIds) {
    const auto project = Workspace::open(root_ / "project");
    const auto other = Workspace::open(root_ / "other");
    const auto nested = Workspace::open(root_ / "project" / "src");
    ASSERT_TRUE(project && other && nested);
    EXPECT_NE(project->id, other->id);
    EXPECT_NE(project->id, nested->id);
    EXPECT_NE(other->id, nested->id);
}

TEST_F(WorkspaceTest, TheIdIsTheFnv1aHashOfThePathAndDoesNotChangeBetweenReleases) {
    const auto workspace = Workspace::open("/");
    ASSERT_TRUE(workspace.has_value()) << workspace.error().message;
    EXPECT_EQ(workspace->name, "/");
    EXPECT_EQ(workspace->id, "af63a24c860189fe");
}

TEST_F(WorkspaceTest, ATrailingSeparatorNamesTheSameWorkspace) {
    const auto plain = Workspace::open(root_ / "project");
    const auto slash = Workspace::open((root_ / "project").string() + "/");
    ASSERT_TRUE(plain && slash);
    EXPECT_EQ(*plain, *slash);
    EXPECT_EQ(slash->name, "project");
}

TEST_F(WorkspaceTest, ARelativePathNamesTheSameWorkspace) {
    const fs::path before = fs::current_path();
    fs::current_path(root_);
    const auto relative = Workspace::open("project/../project");
    fs::current_path(before);
    const auto absolute = Workspace::open(root_ / "project");
    ASSERT_TRUE(relative && absolute);
    EXPECT_EQ(*relative, *absolute);
}

TEST_F(WorkspaceTest, ASymbolicLinkNamesTheFolderItPointsTo) {
    fs::create_directory_symlink(root_ / "project", root_ / "link");
    const auto linked = Workspace::open(root_ / "link");
    const auto direct = Workspace::open(root_ / "project");
    ASSERT_TRUE(linked && direct);
    EXPECT_EQ(*linked, *direct);
    EXPECT_EQ(linked->name, "project");
}

TEST_F(WorkspaceTest, AMissingFolderIsNotFound) {
    const auto workspace = Workspace::open(root_ / "missing");
    ASSERT_FALSE(workspace.has_value());
    EXPECT_EQ(workspace.error().code, ErrorCode::not_found);
}

TEST_F(WorkspaceTest, AFileIsNotAFolder) {
    { std::ofstream(root_ / "file.txt") << "x"; }
    const auto workspace = Workspace::open(root_ / "file.txt");
    ASSERT_FALSE(workspace.has_value());
    EXPECT_EQ(workspace.error().code, ErrorCode::not_a_directory);
}

TEST_F(WorkspaceTest, TheFirstFolderOnTheCommandLineIsTheWorkspaceAndTheRestAreFiles) {
    const auto launch = anvil::core::parse_launch_arguments(
        {(root_ / "a.txt").string(), (root_ / "project").string(), (root_ / "b.txt").string()});
    ASSERT_TRUE(launch.workspace.has_value());
    EXPECT_EQ(launch.workspace->folder, root_ / "project");
    EXPECT_EQ(launch.files, (std::vector<fs::path>{root_ / "a.txt", root_ / "b.txt"}));
    EXPECT_TRUE(launch.problems.empty());
}

TEST_F(WorkspaceTest, ArgumentsWithoutAFolderAreAllFiles) {
    const auto launch = anvil::core::parse_launch_arguments({"x.txt", "dir/y.txt"});
    EXPECT_FALSE(launch.workspace.has_value());
    EXPECT_EQ(launch.files, (std::vector<fs::path>{"x.txt", "dir/y.txt"}));
}

TEST_F(WorkspaceTest, AFolderThatDoesNotExistIsReportedAndNotOpenedAsAFile) {
    const auto launch =
        anvil::core::parse_launch_arguments({(root_ / "missing").string() + "/", "a.txt"});
    EXPECT_FALSE(launch.workspace.has_value());
    EXPECT_EQ(launch.files, (std::vector<fs::path>{"a.txt"}));
    ASSERT_EQ(launch.problems.size(), 1u);
    EXPECT_NE(launch.problems[0].find("missing"), std::string::npos);
}

TEST_F(WorkspaceTest, ASecondFolderIsReportedAndIgnored) {
    const auto launch = anvil::core::parse_launch_arguments(
        {(root_ / "project").string(), (root_ / "other").string()});
    ASSERT_TRUE(launch.workspace.has_value());
    EXPECT_EQ(launch.workspace->name, "project");
    EXPECT_TRUE(launch.files.empty());
    ASSERT_EQ(launch.problems.size(), 1u);
    EXPECT_NE(launch.problems[0].find("other"), std::string::npos);
}
