#include "anvil/workbench/state_store.hpp"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <sstream>
#include <unistd.h>

using anvil::core::LogLevel;
using anvil::core::RecordingLogger;
using anvil::workbench::StateScope;
using anvil::workbench::StateStore;

namespace {

class StateStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path() /
                ("anvil-state-test-" + std::to_string(::getpid()));
        std::filesystem::remove_all(root_);
        std::filesystem::create_directories(root_);
        directory_ = root_ / "state";
    }
    void TearDown() override {
        std::filesystem::remove_all(root_);
    }

    StateStore store(const std::optional<std::string>& workspace = "0123456789abcdef") {
        return StateStore(directory_, workspace, logger_);
    }
    void write_text(const std::filesystem::path& file, const std::string& text) {
        std::filesystem::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << text;
    }
    static nlohmann::json read_json(const std::filesystem::path& file) {
        std::ifstream stream(file, std::ios::binary);
        std::ostringstream text;
        text << stream.rdbuf();
        return nlohmann::json::parse(text.str());
    }
    std::filesystem::path global_file() const {
        return directory_ / "global.json";
    }
    std::filesystem::path workspace_file(const std::string& id = "0123456789abcdef") const {
        return directory_ / "workspaces" / (id + ".json");
    }

    std::filesystem::path root_;
    std::filesystem::path directory_;
    std::shared_ptr<RecordingLogger> logger_ = std::make_shared<RecordingLogger>();
};

} // namespace

TEST_F(StateStoreTest, ANewStoreHasNothingAndWritesNothing) {
    StateStore state = store();
    EXPECT_FALSE(state.boolean(StateScope::global, "a").has_value());
    EXPECT_FALSE(state.integer(StateScope::workspace, "a").has_value());
    EXPECT_FALSE(state.string(StateScope::workspace, "a").has_value());
    ASSERT_TRUE(state.flush().has_value());
    EXPECT_FALSE(std::filesystem::exists(directory_));
    EXPECT_TRUE(logger_->entries().empty());
}

TEST_F(StateStoreTest, ValuesAreInTheFileOfTheirScopeAndReadBack) {
    {
        StateStore state = store();
        state.set(StateScope::global, "window.width", 800);
        state.set(StateScope::workspace, "layout.sidebar.visible", true);
        state.set(StateScope::workspace, "layout.panel.position", "left");
        EXPECT_EQ(state.integer(StateScope::global, "window.width"), 800);
        EXPECT_FALSE(std::filesystem::exists(directory_));
        ASSERT_TRUE(state.flush().has_value());
    }
    EXPECT_EQ(read_json(global_file()), nlohmann::json({{"window.width", 800}}));
    EXPECT_EQ(read_json(workspace_file()), nlohmann::json({{"layout.sidebar.visible", true},
                                                           {"layout.panel.position", "left"}}));

    StateStore state = store();
    EXPECT_EQ(state.integer(StateScope::global, "window.width"), 800);
    EXPECT_EQ(state.boolean(StateScope::workspace, "layout.sidebar.visible"), true);
    EXPECT_EQ(state.string(StateScope::workspace, "layout.panel.position"), "left");
    EXPECT_FALSE(state.integer(StateScope::workspace, "window.width").has_value());
}

TEST_F(StateStoreTest, EachWorkspaceAndNoFolderHaveTheirOwnState) {
    {
        StateStore first = store("1111111111111111");
        first.set(StateScope::workspace, "key", 1);
        ASSERT_TRUE(first.flush().has_value());
        StateStore none = store(std::nullopt);
        none.set(StateScope::workspace, "key", 2);
        ASSERT_TRUE(none.flush().has_value());
    }
    EXPECT_EQ(read_json(workspace_file("1111111111111111")), nlohmann::json({{"key", 1}}));
    EXPECT_EQ(read_json(directory_ / "no-folder.json"), nlohmann::json({{"key", 2}}));
    EXPECT_EQ(store("1111111111111111").integer(StateScope::workspace, "key"), 1);
    EXPECT_EQ(store(std::nullopt).integer(StateScope::workspace, "key"), 2);
    EXPECT_FALSE(store("2222222222222222").integer(StateScope::workspace, "key").has_value());
}

TEST_F(StateStoreTest, ChangingTheWorkspaceWritesTheOneLeftAndReadsTheNewOne) {
    write_text(workspace_file("2222222222222222"), R"({"key": 20})");
    StateStore state = store("1111111111111111");
    state.set(StateScope::workspace, "key", 10);
    state.set(StateScope::global, "shared", 1);
    state.set_workspace("2222222222222222");
    EXPECT_EQ(read_json(workspace_file("1111111111111111")), nlohmann::json({{"key", 10}}));
    EXPECT_EQ(state.integer(StateScope::workspace, "key"), 20);
    EXPECT_EQ(state.integer(StateScope::global, "shared"), 1);

    state.set(StateScope::workspace, "other", 30);
    ASSERT_TRUE(state.flush().has_value());
    EXPECT_EQ(read_json(workspace_file("2222222222222222")),
              nlohmann::json({{"key", 20}, {"other", 30}}));
    EXPECT_EQ(read_json(workspace_file("1111111111111111")), nlohmann::json({{"key", 10}}));
}

TEST_F(StateStoreTest, TheStoreTellsWhetherItHasTheStateOfAFolder) {
    StateStore state = store();
    EXPECT_TRUE(state.has_workspace());
    state.set_workspace(std::nullopt);
    EXPECT_FALSE(state.has_workspace());
    state.set_workspace("2222222222222222");
    EXPECT_TRUE(state.has_workspace());
    EXPECT_FALSE(store(std::nullopt).has_workspace());
    // Also with the state in memory only.
    EXPECT_TRUE(StateStore({}, "1111111111111111", logger_).has_workspace());
}

TEST_F(StateStoreTest, TwoStoresKeepWhatTheOtherWrote) {
    write_text(global_file(), R"({"unknown.key": [1, 2]})");
    StateStore first = store();
    StateStore second = store();
    first.set(StateScope::global, "first", 1);
    second.set(StateScope::global, "second", 2);
    ASSERT_TRUE(first.flush().has_value());
    ASSERT_TRUE(second.flush().has_value());
    EXPECT_EQ(read_json(global_file()),
              nlohmann::json({{"first", 1}, {"second", 2}, {"unknown.key", {1, 2}}}));

    // The first store writes only the key it sets again, not what it holds of the others.
    first.set(StateScope::global, "third", 3);
    ASSERT_TRUE(first.flush().has_value());
    EXPECT_EQ(read_json(global_file()),
              nlohmann::json({{"first", 1}, {"second", 2}, {"third", 3}, {"unknown.key", {1, 2}}}));
}

TEST_F(StateStoreTest, SettingTheValueThatIsThereIsNotAChange) {
    write_text(global_file(), R"({"width": 300})");
    StateStore first = store();
    write_text(global_file(), R"({"width": 500})");
    first.set(StateScope::global, "width", 300);
    ASSERT_TRUE(first.flush().has_value());
    EXPECT_EQ(read_json(global_file()), nlohmann::json({{"width", 500}}));

    first.set(StateScope::global, "width", 301);
    ASSERT_TRUE(first.flush().has_value());
    EXPECT_EQ(read_json(global_file()), nlohmann::json({{"width", 301}}));
}

TEST_F(StateStoreTest, OverwritingWritesAlsoAValueThatTheStoreHasAlready) {
    write_text(global_file(), R"({"last": "mine"})");
    StateStore state = store();
    write_text(global_file(), R"({"last": "theirs"})");
    state.set(StateScope::global, "last", "mine");
    ASSERT_TRUE(state.flush().has_value());
    EXPECT_EQ(read_json(global_file()), nlohmann::json({{"last", "theirs"}}));

    state.overwrite(StateScope::global, "last", "mine");
    state.overwrite(StateScope::workspace, "here", 1);
    ASSERT_TRUE(state.flush().has_value());
    EXPECT_EQ(read_json(global_file()), nlohmann::json({{"last", "mine"}}));
    EXPECT_EQ(read_json(workspace_file()), nlohmann::json({{"here", 1}}));
}

TEST_F(StateStoreTest, AValueWithAFormOfItsOwnReadsBackAsItIs) {
    const nlohmann::json value = {{"root", {{"tabs", {1, 2}}}}};
    StateStore first = store();
    EXPECT_FALSE(first.value(StateScope::workspace, "layout").has_value());
    first.set(StateScope::workspace, "layout", value);
    ASSERT_TRUE(first.flush().has_value());
    EXPECT_EQ(store().value(StateScope::workspace, "layout"), value);
}

TEST_F(StateStoreTest, AValueOfAnotherTypeIsNoValue) {
    write_text(global_file(), R"({"text": "7", "number": 7, "real": 7.5, "flag": 1,)"
                              R"( "huge": 18446744073709551615, "null": null})");
    const StateStore state = store();
    EXPECT_FALSE(state.integer(StateScope::global, "text").has_value());
    EXPECT_FALSE(state.integer(StateScope::global, "real").has_value());
    EXPECT_FALSE(state.integer(StateScope::global, "huge").has_value());
    EXPECT_FALSE(state.integer(StateScope::global, "null").has_value());
    EXPECT_FALSE(state.boolean(StateScope::global, "flag").has_value());
    EXPECT_FALSE(state.string(StateScope::global, "number").has_value());
    EXPECT_EQ(state.integer(StateScope::global, "number"), 7);
}

TEST_F(StateStoreTest, AListOfStringsReadsBackAndAnythingElseIsNone) {
    write_text(global_file(),
               R"({"list": ["b", "a"], "empty": [], "mixed": ["a", 1], "text": "a"})");
    const StateStore state = store();
    EXPECT_EQ(state.strings(StateScope::global, "list"), (std::vector<std::string>{"b", "a"}));
    EXPECT_EQ(state.strings(StateScope::global, "empty"), std::vector<std::string>{});
    EXPECT_FALSE(state.strings(StateScope::global, "mixed").has_value());
    EXPECT_FALSE(state.strings(StateScope::global, "text").has_value());
    EXPECT_FALSE(state.strings(StateScope::global, "missing").has_value());
}

TEST_F(StateStoreTest, ADamagedFileIsReadAsEmptyLoggedOnceAndReplaced) {
    for (const std::string& damaged : {std::string("{\"key\": "), std::string("[1, 2]"),
                                       std::string("\xFF\xFE"), std::string()}) {
        logger_ = std::make_shared<RecordingLogger>();
        write_text(global_file(), damaged);
        StateStore state = store();
        EXPECT_FALSE(state.integer(StateScope::global, "key").has_value());
        state.set(StateScope::global, "key", 4);
        ASSERT_TRUE(state.flush().has_value());
        EXPECT_EQ(read_json(global_file()), nlohmann::json({{"key", 4}}));
        EXPECT_EQ(logger_->entries().size(), 1u) << damaged;
        EXPECT_TRUE(logger_->contains(LogLevel::warning, "global.json", "state"));
    }
}

TEST_F(StateStoreTest, AFileThatCannotBeReadIsNotReplacedAndTheValuesWait) {
    // A directory where the file belongs can be neither read nor replaced.
    std::filesystem::create_directories(global_file() / "inside");
    StateStore state = store();
    state.set(StateScope::global, "key", 4);
    state.set(StateScope::workspace, "other", 5);
    EXPECT_FALSE(state.flush().has_value());
    EXPECT_TRUE(logger_->contains(LogLevel::warning, "global.json", "state"));
    EXPECT_TRUE(std::filesystem::is_directory(global_file() / "inside"));
    EXPECT_EQ(read_json(workspace_file()), nlohmann::json({{"other", 5}}));

    std::filesystem::remove_all(global_file());
    ASSERT_TRUE(state.flush().has_value());
    EXPECT_EQ(read_json(global_file()), nlohmann::json({{"key", 4}}));
}

TEST_F(StateStoreTest, AFileWithoutReadPermissionKeepsWhatItHolds) {
    write_text(global_file(), R"({"other": 1})");
    std::filesystem::permissions(global_file(), std::filesystem::perms::none);
    if (std::ifstream(global_file()).good()) {
        GTEST_SKIP() << "the permission does not stop this user from reading";
    }
    StateStore state = store();
    state.set(StateScope::global, "key", 4);
    EXPECT_FALSE(state.flush().has_value());
    std::filesystem::permissions(global_file(), std::filesystem::perms::owner_read |
                                                    std::filesystem::perms::owner_write);
    EXPECT_EQ(read_json(global_file()), nlohmann::json({{"other", 1}}));
    ASSERT_TRUE(state.flush().has_value());
    EXPECT_EQ(read_json(global_file()), nlohmann::json({{"key", 4}, {"other", 1}}));
}

TEST_F(StateStoreTest, WithNoDirectoryTheStateStaysInMemory) {
    StateStore state(std::filesystem::path(), "1111111111111111", logger_);
    state.set(StateScope::global, "a", 1);
    state.set(StateScope::workspace, "b", 2);
    ASSERT_TRUE(state.flush().has_value());
    EXPECT_EQ(state.integer(StateScope::global, "a"), 1);
    EXPECT_EQ(state.integer(StateScope::workspace, "b"), 2);
    state.set_workspace(std::nullopt);
    EXPECT_FALSE(state.integer(StateScope::workspace, "b").has_value());
    EXPECT_TRUE(std::filesystem::is_empty(root_));
    EXPECT_TRUE(logger_->entries().empty());
}
