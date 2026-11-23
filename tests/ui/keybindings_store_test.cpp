#include "anvil/platform/file.hpp"
#include "anvil/ui/keybindings_store.hpp"

#include <QSignalSpy>
#include <QTest>
#include <gtest/gtest.h>
#include <unistd.h>

using namespace anvil;
using anvil::ui::KeybindingsStore;

namespace {

class KeybindingsStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        scratch_ = std::filesystem::temp_directory_path() /
                   ("anvil-keybindings-test-" + std::to_string(::getpid()));
        std::filesystem::create_directories(scratch_);
        file_ = scratch_ / "keybindings.json";
        logger_ = std::make_shared<core::RecordingLogger>();
    }
    void TearDown() override {
        store_.reset();
        std::filesystem::remove_all(scratch_);
    }

    void write(const std::string& text) {
        ASSERT_TRUE(platform::write_file(file_, text).has_value());
    }

    void start() {
        store_ = std::make_unique<KeybindingsStore>(KeybindingsStore::Options{
            .file = file_, .logger = logger_, .debounce = std::chrono::milliseconds(10)});
        failures_ = std::make_unique<QSignalSpy>(store_.get(), &KeybindingsStore::load_failed);
        changes_ = std::make_unique<QSignalSpy>(store_.get(), &KeybindingsStore::changed);
        store_->start();
    }

    template <class Done> bool wait_until(Done done) {
        for (int i = 0; i < 500 && !done(); ++i) {
            QTest::qWait(10);
        }
        return done();
    }

    void settle() {
        QTest::qWait(200);
    }

    std::vector<std::string> commands() const {
        std::vector<std::string> result;
        for (const core::Keybinding& binding : store_->bindings()) {
            result.push_back(binding.command);
        }
        return result;
    }

    static std::string entry(const std::string& key, const std::string& command) {
        return R"j({"key": ")j" + key + R"j(", "command": ")j" + command + R"j("})j";
    }

    std::filesystem::path scratch_;
    std::filesystem::path file_;
    std::shared_ptr<core::RecordingLogger> logger_;
    std::unique_ptr<KeybindingsStore> store_;
    std::unique_ptr<QSignalSpy> failures_;
    std::unique_ptr<QSignalSpy> changes_;
};

} // namespace

TEST_F(KeybindingsStoreTest, AMissingFileMeansNoBindingsAndCreatesNothing) {
    start();
    EXPECT_TRUE(store_->bindings().empty());
    EXPECT_FALSE(std::filesystem::exists(file_));
    EXPECT_EQ(failures_->count(), 0);
}

TEST_F(KeybindingsStoreTest, NoFileMeansNoBindingsAtAll) {
    store_ = std::make_unique<KeybindingsStore>(KeybindingsStore::Options{});
    store_->start();
    EXPECT_TRUE(store_->bindings().empty());
}

TEST_F(KeybindingsStoreTest, TheFileIsReadAtStartWithoutAChangeSignal) {
    write("[" + entry("ctrl+s", "a") + ", " + entry("ctrl+k ctrl+c", "b") + "]");
    start();
    EXPECT_EQ(commands(), (std::vector<std::string>{"a", "b"}));
    for (const core::Keybinding& binding : store_->bindings()) {
        EXPECT_EQ(binding.source, core::KeybindingSource::user);
    }
    EXPECT_EQ(changes_->count(), 0);
    EXPECT_EQ(failures_->count(), 0);
}

TEST_F(KeybindingsStoreTest, CommentsAndTrailingCommasAreAllowed) {
    write("// mine\n[\n  /* one */ " + entry("ctrl+s", "a") + ",\n]\n");
    start();
    EXPECT_EQ(commands(), (std::vector<std::string>{"a"}));
    EXPECT_EQ(failures_->count(), 0);
}

TEST_F(KeybindingsStoreTest, AnEmptyFileMeansNoBindings) {
    write("  \n");
    start();
    EXPECT_TRUE(store_->bindings().empty());
    EXPECT_EQ(failures_->count(), 0);
}

TEST_F(KeybindingsStoreTest, EditingTheFileInPlaceReloadsIt) {
    write("[" + entry("ctrl+s", "a") + "]");
    start();
    write("[" + entry("ctrl+s", "b") + "]");
    ASSERT_TRUE(wait_until([&] { return changes_->count() >= 1; }));
    EXPECT_EQ(commands(), (std::vector<std::string>{"b"}));
}

TEST_F(KeybindingsStoreTest, ReplacingTheFileByRenameIsFollowedEveryTime) {
    write("[" + entry("ctrl+s", "a") + "]");
    start();
    for (const std::string name : {"one", "two", "three"}) {
        const auto temporary = scratch_ / "keybindings.json.tmp";
        ASSERT_TRUE(platform::write_file(temporary, "[" + entry("ctrl+s", name) + "]").has_value());
        std::filesystem::rename(temporary, file_);
        ASSERT_TRUE(wait_until([&] { return commands() == std::vector<std::string>{name}; }))
            << "after replacing the file with " << name;
    }
}

TEST_F(KeybindingsStoreTest, ACreatedFileIsPickedUp) {
    start();
    write("[" + entry("ctrl+s", "a") + "]");
    ASSERT_TRUE(wait_until([&] { return !store_->bindings().empty(); }));
    EXPECT_EQ(changes_->count(), 1);
}

TEST_F(KeybindingsStoreTest, AFileInADirectoryThatDoesNotExistYetIsPickedUp) {
    file_ = scratch_ / "config" / "anvil" / "keybindings.json";
    start();
    std::filesystem::create_directories(file_.parent_path());
    settle();
    write("[" + entry("ctrl+s", "a") + "]");
    ASSERT_TRUE(wait_until([&] { return !store_->bindings().empty(); }));
}

TEST_F(KeybindingsStoreTest, DeletingTheFileRemovesTheBindings) {
    write("[" + entry("ctrl+s", "a") + "]");
    start();
    std::filesystem::remove(file_);
    ASSERT_TRUE(wait_until([&] { return store_->bindings().empty(); }));
    EXPECT_EQ(changes_->count(), 1);
    EXPECT_EQ(failures_->count(), 0);
}

TEST_F(KeybindingsStoreTest, TouchingTheFileWithoutChangingItDoesNotSignal) {
    write("[" + entry("ctrl+s", "a") + "]");
    start();
    write("[" + entry("ctrl+s", "a") + "]");
    settle();
    EXPECT_EQ(changes_->count(), 0);
}

TEST_F(KeybindingsStoreTest, ASyntaxErrorKeepsThePreviousBindingsAndIsReportedOnce) {
    write("[" + entry("ctrl+s", "a") + "]");
    start();
    write("[\n  {\"key\": \"ctrl+s\",,\n]");
    ASSERT_TRUE(wait_until([&] { return failures_->count() >= 1; }));
    EXPECT_EQ(commands(), (std::vector<std::string>{"a"}));
    const QString message = failures_->at(0).at(0).toString();
    EXPECT_TRUE(message.startsWith("keybindings.json: line 2")) << message.toStdString();
    EXPECT_TRUE(logger_->contains(core::LogLevel::error, "line 2", "keybindings"));

    write("[\n  {\"key\": \"ctrl+s\",,\n]");
    write("[\n  {\"key\": \"ctrl+s\",,\n]");
    settle();
    EXPECT_EQ(failures_->count(), 1);
    EXPECT_EQ(changes_->count(), 0);
}

TEST_F(KeybindingsStoreTest, AValueThatIsNotAnArrayKeepsThePreviousBindings) {
    write("[" + entry("ctrl+s", "a") + "]");
    start();
    write(R"j({"key": "ctrl+s", "command": "b"})j");
    ASSERT_TRUE(wait_until([&] { return failures_->count() == 1; }));
    EXPECT_EQ(commands(), (std::vector<std::string>{"a"}));
    EXPECT_TRUE(failures_->at(0).at(0).toString().contains("array"));
}

TEST_F(KeybindingsStoreTest, AnEntryThatCannotBeUsedIsLeftOutAndTheOthersApply) {
    write("[" + entry("ctrl+s", "a") + ", " + entry("not a key", "b") + ", " +
          entry("ctrl+o", "c") + ", {\"command\": 3}]");
    start();
    EXPECT_EQ(commands(), (std::vector<std::string>{"a", "c"}));
    ASSERT_EQ(failures_->count(), 1);
    const QString message = failures_->at(0).at(0).toString();
    EXPECT_TRUE(message.startsWith("keybindings.json: entry 1")) << message.toStdString();
    EXPECT_TRUE(message.endsWith("(and 1 more)")) << message.toStdString();
    EXPECT_TRUE(logger_->contains(core::LogLevel::error, "entry 1", "keybindings"));
    EXPECT_TRUE(logger_->contains(core::LogLevel::error, "entry 3", "keybindings"));
}

TEST_F(KeybindingsStoreTest, AFixedFileClearsTheProblemSoItIsReportedAgain) {
    write("[" + entry("ctrl+s", "a") + "]");
    start();
    write("[");
    ASSERT_TRUE(wait_until([&] { return failures_->count() == 1; }));
    write("[" + entry("ctrl+s", "b") + "]");
    ASSERT_TRUE(wait_until([&] { return commands() == std::vector<std::string>{"b"}; }));
    write("[");
    ASSERT_TRUE(wait_until([&] { return failures_->count() == 2; }));
    EXPECT_EQ(commands(), (std::vector<std::string>{"b"}));
}

TEST_F(KeybindingsStoreTest, ARemovalIsKeptForTheTable) {
    write("[" + entry("ctrl+s", "-workbench.action.files.save") + "]");
    start();
    EXPECT_EQ(commands(), (std::vector<std::string>{"-workbench.action.files.save"}));
}
