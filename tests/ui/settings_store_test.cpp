#include "anvil/platform/file.hpp"
#include "anvil/ui/settings_store.hpp"

#include <QSignalSpy>
#include <QTest>
#include <gtest/gtest.h>
#include <unistd.h>

using namespace anvil;
using anvil::ui::SettingsStore;

namespace {

class SettingsStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        scratch_ = std::filesystem::temp_directory_path() /
                   ("anvil-settings-test-" + std::to_string(::getpid()));
        std::filesystem::create_directories(scratch_);
        file_ = scratch_ / "settings.json";
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
        store_ = std::make_unique<SettingsStore>(
            SettingsStore::Options{.file = file_,
                                   .defaults = {},
                                   .logger = logger_,
                                   .debounce = std::chrono::milliseconds(10)});
        failures_ = std::make_unique<QSignalSpy>(store_.get(), &SettingsStore::load_failed);
        changes_ = std::make_unique<QSignalSpy>(store_.get(), &SettingsStore::changed);
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

    std::filesystem::path scratch_;
    std::filesystem::path file_;
    std::shared_ptr<core::RecordingLogger> logger_;
    std::unique_ptr<SettingsStore> store_;
    std::unique_ptr<QSignalSpy> failures_;
    std::unique_ptr<QSignalSpy> changes_;
};

} // namespace

TEST_F(SettingsStoreTest, AMissingFileMeansTheDefaultsAndCreatesNothing) {
    start();
    EXPECT_EQ(store_->editor(), *core::EditorSettings::from_settings({}, {}));
    EXPECT_FALSE(std::filesystem::exists(file_));
    EXPECT_EQ(failures_->count(), 0);
}

TEST_F(SettingsStoreTest, TheFileIsReadAtStartWithoutAChangeSignal) {
    write(R"j({"editor.fontSize": 20, "workbench.colorTheme": "x"})j");
    start();
    EXPECT_EQ(store_->editor().font_size, 20);
    EXPECT_EQ(store_->settings().get<std::string>("workbench.colorTheme"), "x");
    EXPECT_EQ(changes_->count(), 0);
}

TEST_F(SettingsStoreTest, NoFileMeansNoSettingsAtAll) {
    store_ = std::make_unique<SettingsStore>(SettingsStore::Options{});
    store_->start();
    EXPECT_EQ(store_->editor().font_size, 14);
}

TEST_F(SettingsStoreTest, EditingTheFileInPlaceReloadsIt) {
    write(R"j({"editor.tabSize": 2})j");
    start();
    write(R"j({"editor.tabSize": 8})j");
    ASSERT_TRUE(wait_until([&] { return changes_->count() >= 1; }));
    EXPECT_EQ(store_->editor().tab_size, 8);
}

TEST_F(SettingsStoreTest, ReplacingTheFileByRenameIsFollowedEveryTime) {
    write(R"j({"editor.tabSize": 2})j");
    start();
    for (const int size : {3, 5, 7}) {
        const auto temporary = scratch_ / "settings.json.tmp";
        ASSERT_TRUE(
            platform::write_file(temporary, R"j({"editor.tabSize": )j" + std::to_string(size) + "}")
                .has_value());
        std::filesystem::rename(temporary, file_);
        ASSERT_TRUE(wait_until([&] { return store_->editor().tab_size == size; }))
            << "after replacing the file with tab size " << size;
    }
}

TEST_F(SettingsStoreTest, ACreatedFileIsPickedUp) {
    start();
    write(R"j({"editor.tabSize": 6})j");
    ASSERT_TRUE(wait_until([&] { return store_->editor().tab_size == 6; }));
    EXPECT_EQ(changes_->count(), 1);
}

TEST_F(SettingsStoreTest, AFileInADirectoryThatDoesNotExistYetIsPickedUp) {
    file_ = scratch_ / "config" / "anvil" / "settings.json";
    start();
    std::filesystem::create_directories(file_.parent_path());
    settle();
    write(R"j({"editor.tabSize": 6})j");
    ASSERT_TRUE(wait_until([&] { return store_->editor().tab_size == 6; }));
}

TEST_F(SettingsStoreTest, DeletingTheFileReturnsToTheDefaults) {
    write(R"j({"editor.tabSize": 6})j");
    start();
    std::filesystem::remove(file_);
    ASSERT_TRUE(wait_until([&] { return store_->editor().tab_size == 4; }));
    EXPECT_EQ(failures_->count(), 0);
}

TEST_F(SettingsStoreTest, TouchingTheFileWithoutChangingItDoesNotSignal) {
    write(R"j({"editor.tabSize": 6})j");
    start();
    write(R"j({"editor.tabSize": 6})j");
    settle();
    EXPECT_EQ(changes_->count(), 0);
}

TEST_F(SettingsStoreTest, ASyntaxErrorKeepsThePreviousValuesAndIsReportedOnce) {
    write(R"j({"editor.tabSize": 6})j");
    start();
    write("{\n  \"editor.tabSize\": 2,,\n}");
    ASSERT_TRUE(wait_until([&] { return failures_->count() >= 1; }));
    EXPECT_EQ(store_->editor().tab_size, 6);
    const QString message = failures_->at(0).at(0).toString();
    EXPECT_TRUE(message.startsWith("settings.json: line 2, column 23")) << message.toStdString();
    EXPECT_TRUE(logger_->contains(core::LogLevel::error, "line 2, column 23", "settings"));

    write("{\n  \"editor.tabSize\": 2,,\n}");
    write("{\n  \"editor.tabSize\": 2,,\n}");
    settle();
    EXPECT_EQ(failures_->count(), 1);
    EXPECT_EQ(changes_->count(), 0);
}

TEST_F(SettingsStoreTest, AWronglyTypedValueKeepsThePreviousValuesAndNamesTheKey) {
    write(R"j({"editor.tabSize": 6, "editor.fontSize": 20})j");
    start();
    write(R"j({"editor.tabSize": 2, "editor.fontSize": "big"})j");
    ASSERT_TRUE(wait_until([&] { return failures_->count() >= 1; }));
    EXPECT_EQ(store_->editor().tab_size, 6);
    EXPECT_EQ(store_->editor().font_size, 20);
    EXPECT_TRUE(failures_->at(0).at(0).toString().contains("editor.fontSize must be a number"));
}

TEST_F(SettingsStoreTest, AProblemInTheFileAtStartIsReportedAndTheDefaultsApply) {
    write("not json");
    start();
    EXPECT_EQ(failures_->count(), 1);
    EXPECT_EQ(store_->editor().tab_size, 4);
}

TEST_F(SettingsStoreTest, AFixedFileIsAppliedAndAnotherProblemIsReportedAgain) {
    write(R"j({"editor.tabSize": 6})j");
    start();
    write("{");
    ASSERT_TRUE(wait_until([&] { return failures_->count() == 1; }));
    write(R"j({"editor.tabSize": 3})j");
    ASSERT_TRUE(wait_until([&] { return store_->editor().tab_size == 3; }));
    write("{");
    ASSERT_TRUE(wait_until([&] { return failures_->count() == 2; }));
    EXPECT_EQ(store_->editor().tab_size, 3);
}

TEST_F(SettingsStoreTest, ATopLevelValueThatIsNotAnObjectIsAProblem) {
    write(R"j({"editor.tabSize": 6})j");
    start();
    write("[1, 2]");
    ASSERT_TRUE(wait_until([&] { return failures_->count() == 1; }));
    EXPECT_EQ(store_->editor().tab_size, 6);
}
