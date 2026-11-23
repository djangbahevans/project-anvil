#include "anvil/platform/file.hpp"
#include "anvil/ui/settings_store.hpp"

#include <QSignalSpy>
#include <QTest>
#include <gtest/gtest.h>
#include <sys/stat.h>
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
                                   .debounce = std::chrono::milliseconds(10),
                                   .is_unsaved = is_unsaved_,
                                   .read_file = read_hook_});
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

    std::string read_text() {
        auto text = platform::read_file(file_);
        EXPECT_TRUE(text.has_value());
        return text ? *text : std::string();
    }

    std::function<bool(const std::filesystem::path&)> is_unsaved_;
    std::function<std::expected<std::string, Error>(const std::filesystem::path&)> read_hook_;
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

TEST_F(SettingsStoreTest, ChangingTheColorThemeSignalsAndTheDefaultIsVsCodes) {
    write(R"j({"editor.tabSize": 2})j");
    start();
    EXPECT_EQ(store_->editor().color_theme, "Dark 2026");
    write(R"j({"workbench.colorTheme": "Light Modern"})j");
    ASSERT_TRUE(wait_until([&] { return changes_->count() >= 1; }));
    EXPECT_EQ(store_->editor().color_theme, "Light Modern");
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

// --- change ---

using Outcome = SettingsStore::ChangeResult::Outcome;

TEST_F(SettingsStoreTest, ChangeEditsTheFileInPlaceAndTheSettingsAreInForceAtOnce) {
    write("{\n    // the size\n    \"editor.fontSize\": 14, // trailing\n    \"a\": 1\n}\n");
    start();
    const auto result = store_->change("editor.fontSize", 18);
    EXPECT_EQ(result.outcome, Outcome::written);
    EXPECT_EQ(result.reason, "");
    EXPECT_EQ(read_text(),
              "{\n    // the size\n    \"editor.fontSize\": 18, // trailing\n    \"a\": 1\n}\n");
    EXPECT_EQ(store_->editor().font_size, 18);
    EXPECT_EQ(store_->settings().get<std::int64_t>("editor.fontSize"), 18);
    EXPECT_EQ(changes_->count(), 1);
    EXPECT_EQ(failures_->count(), 0);
}

TEST_F(SettingsStoreTest, TheWatcherReadingTheFileBackSignalsNothingMore) {
    write(R"j({"editor.fontSize": 14})j");
    start();
    ASSERT_EQ(store_->change("editor.fontSize", 18).outcome, Outcome::written);
    ASSERT_EQ(changes_->count(), 1);
    settle();
    settle();
    EXPECT_EQ(changes_->count(), 1);
    EXPECT_EQ(failures_->count(), 0);
    EXPECT_EQ(store_->editor().font_size, 18);
}

TEST_F(SettingsStoreTest, ChangeAddsAKeyTheFileDoesNotHave) {
    write("{\n    \"editor.fontSize\": 14\n}\n");
    start();
    EXPECT_EQ(store_->change("workbench.colorTheme", "Light 2026").outcome, Outcome::written);
    EXPECT_EQ(read_text(),
              "{\n    \"editor.fontSize\": 14,\n    \"workbench.colorTheme\": \"Light 2026\"\n}\n");
    EXPECT_EQ(store_->editor().color_theme, "Light 2026");
}

TEST_F(SettingsStoreTest, ChangeFollowsTheSpellingTheFileUses) {
    write(R"j({"editor": {"fontSize": 14, "tabSize": 2}})j");
    start();
    EXPECT_EQ(store_->change("editor.fontSize", 18).outcome, Outcome::written);
    EXPECT_EQ(read_text(), R"j({"editor": {"fontSize": 18, "tabSize": 2}})j");
    EXPECT_EQ(store_->editor().font_size, 18);
}

TEST_F(SettingsStoreTest, ChangeCreatesAMissingFileAndItsDirectory) {
    file_ = scratch_ / "config" / "anvil" / "settings.json";
    start();
    ASSERT_FALSE(std::filesystem::exists(file_));
    EXPECT_EQ(store_->change("editor.tabSize", 2).outcome, Outcome::written);
    EXPECT_EQ(read_text(), "{\n    \"editor.tabSize\": 2\n}\n");
    EXPECT_EQ(store_->editor().tab_size, 2);
}

TEST_F(SettingsStoreTest, AFileWithASyntaxErrorIsLeftAloneAndTheChangeIsForTheSession) {
    const std::string broken = "{\n    \"editor.tabSize\": 2,\n    \"a\": \n}\n";
    write(broken);
    start();
    ASSERT_EQ(failures_->count(), 1);
    const auto result = store_->change("editor.fontSize", 22);
    EXPECT_EQ(result.outcome, Outcome::session);
    EXPECT_TRUE(result.reason.starts_with("settings.json: line 4")) << result.reason;
    EXPECT_EQ(read_text(), broken);
    EXPECT_EQ(store_->editor().font_size, 22);
    EXPECT_EQ(store_->settings().get<std::int64_t>("editor.fontSize"), 22);
    EXPECT_EQ(changes_->count(), 1);
}

TEST_F(SettingsStoreTest, AFileOpenWithUnsavedChangesIsLeftAlone) {
    const std::string text = R"j({"editor.fontSize": 14})j";
    write(text);
    is_unsaved_ = [this](const std::filesystem::path& path) { return path == file_; };
    start();
    const auto result = store_->change("editor.fontSize", 18);
    EXPECT_EQ(result.outcome, Outcome::session);
    EXPECT_EQ(result.reason, "settings.json has unsaved changes in an editor");
    EXPECT_EQ(read_text(), text);
    EXPECT_EQ(store_->editor().font_size, 18);
}

TEST_F(SettingsStoreTest, AFileThatChangesBetweenTheTwoReadsIsLeftAlone) {
    write(R"j({"editor.fontSize": 14})j");
    bool armed = false;
    int reads = 0;
    const std::string other = R"j({"editor.fontSize": 14, "other": true})j";
    read_hook_ = [&](const std::filesystem::path& path) -> std::expected<std::string, Error> {
        auto text = platform::read_file(path);
        // Someone else writes the file right after the first read of the edit.
        if (armed && ++reads == 1) {
            EXPECT_TRUE(platform::write_file(path, other).has_value());
        }
        return text;
    };
    start();
    armed = true;
    const auto result = store_->change("editor.fontSize", 18);
    EXPECT_EQ(result.outcome, Outcome::session);
    EXPECT_EQ(result.reason, "settings.json changed while it was being edited");
    EXPECT_EQ(read_text(), other);
    EXPECT_EQ(store_->editor().font_size, 18);
}

TEST_F(SettingsStoreTest, AFileThatCannotBeWrittenIsLeftAlone) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "a directory is writable for root whatever its mode";
    }
    const std::string text = R"j({"editor.fontSize": 14})j";
    write(text);
    start();
    ASSERT_EQ(::chmod(scratch_.c_str(), 0555), 0);
    const auto result = store_->change("editor.fontSize", 18);
    ::chmod(scratch_.c_str(), 0755);
    EXPECT_EQ(result.outcome, Outcome::session);
    EXPECT_TRUE(result.reason.starts_with("settings.json cannot be written")) << result.reason;
    EXPECT_EQ(read_text(), text);
    EXPECT_EQ(store_->editor().font_size, 18);
}

TEST_F(SettingsStoreTest, WithoutASettingsFileTheChangeIsForTheSession) {
    file_.clear();
    start();
    const auto result = store_->change("editor.fontSize", 18);
    EXPECT_EQ(result.outcome, Outcome::session);
    EXPECT_EQ(result.reason, "there is no settings file");
    EXPECT_EQ(store_->editor().font_size, 18);
    EXPECT_EQ(changes_->count(), 1);
}

TEST_F(SettingsStoreTest, AFileWithAnotherInvalidKeyIsLeftAloneAndSaysWhich) {
    const std::string text = R"j({"editor.tabSize": "wide"})j";
    write(text);
    start();
    const auto result = store_->change("editor.fontSize", 18);
    EXPECT_EQ(result.outcome, Outcome::session);
    EXPECT_NE(result.reason.find("editor.tabSize"), std::string::npos) << result.reason;
    EXPECT_EQ(read_text(), text);
}

TEST_F(SettingsStoreTest, AValueThatIsNotValidForTheKeyIsRejectedAndNothingChanges) {
    const std::string text = R"j({"editor.fontSize": 14})j";
    write(text);
    start();
    const auto result = store_->change("editor.fontSize", "big");
    EXPECT_EQ(result.outcome, Outcome::rejected);
    EXPECT_NE(result.reason.find("editor.fontSize"), std::string::npos) << result.reason;
    EXPECT_EQ(read_text(), text);
    EXPECT_EQ(store_->editor().font_size, 14);
    EXPECT_EQ(changes_->count(), 0);
}

TEST_F(SettingsStoreTest, AnOverrideLastsUntilTheKeyIsChangedAgain) {
    write("{\n    \"editor.fontSize\": \n}\n");
    start();
    ASSERT_EQ(store_->change("editor.fontSize", 22).outcome, Outcome::session);

    // The file is fixed, and says another size; the override still rules.
    write("{\n    \"editor.fontSize\": 14\n}\n");
    ASSERT_TRUE(wait_until([&] { return failures_->count() >= 0 && changes_->count() >= 1; }));
    settle();
    EXPECT_EQ(store_->editor().font_size, 22);

    // Another key is changed: the override of the first stays.
    EXPECT_EQ(store_->change("editor.tabSize", 8).outcome, Outcome::written);
    EXPECT_EQ(store_->editor().font_size, 22);
    EXPECT_EQ(store_->editor().tab_size, 8);

    // The same key again goes to the file, and the override is gone.
    EXPECT_EQ(store_->change("editor.fontSize", 16).outcome, Outcome::written);
    EXPECT_EQ(store_->editor().font_size, 16);
    EXPECT_NE(read_text().find("\"editor.fontSize\": 16"), std::string::npos) << read_text();
    write("{\n    \"editor.fontSize\": 12\n}\n");
    ASSERT_TRUE(wait_until([&] { return store_->editor().font_size == 12; }));
}

TEST_F(SettingsStoreTest, AnOverrideKeepsAFlatKeyAboveANestedSpelling) {
    write("{\n    \"editor\": {\"fontSize\": 14}\n    \"x\": \n}\n");
    start();
    ASSERT_EQ(store_->change("editor.fontSize", 22).outcome, Outcome::session);
    EXPECT_EQ(store_->settings().get<std::int64_t>("editor.fontSize"), 22);
}

TEST_F(SettingsStoreTest, ChangingToWhatIsInForceSignalsNothing) {
    write(R"j({"editor.fontSize": 14})j");
    start();
    EXPECT_EQ(store_->change("editor.fontSize", 14).outcome, Outcome::written);
    EXPECT_EQ(changes_->count(), 0);
    EXPECT_EQ(read_text(), R"j({"editor.fontSize": 14})j");
}
