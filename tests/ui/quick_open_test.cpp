#include "../timing.hpp"
#include "anvil/core/fuzzy.hpp"
#include "anvil/ui/quick_open.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QSignalSpy>
#include <QTest>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <gtest/gtest.h>
#include <mutex>
#include <thread>

using namespace anvil;
using anvil::ui::FileSource;
using anvil::ui::QuickOpen;
using anvil::ui::QuickOpenEntry;
using anvil::ui::WorkbenchColors;
using std::chrono::milliseconds;

namespace {

using Names = std::vector<std::string>;

// What a test gives the listings, which run on a thread of their own.
struct Listings {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<search::ListingBatch> batches;
    // While held, a read does not return, so that the thread cannot go on.
    bool held = false;
    bool waiting = false;
    int started = 0;
    int ended = 0;
    std::optional<std::string> start_error;

    void give(std::vector<std::string> files, bool finished = false,
              std::optional<std::string> error = std::nullopt) {
        const std::lock_guard lock(mutex);
        search::ListingBatch batch{.files = std::move(files), .finished = finished};
        if (error) {
            batch.error = Error{ErrorCode::io_error, *error};
        }
        batches.push_back(std::move(batch));
        changed.notify_all();
    }
    void hold(bool on) {
        const std::lock_guard lock(mutex);
        held = on;
        changed.notify_all();
    }
    int count_of(const int Listings::*counter) {
        const std::lock_guard lock(mutex);
        return this->*counter;
    }
    bool is_waiting() {
        const std::lock_guard lock(mutex);
        return waiting;
    }
};

class FakeSource final : public FileSource {
public:
    explicit FakeSource(std::shared_ptr<Listings> listings) : listings_(std::move(listings)) {}
    ~FakeSource() override {
        const std::lock_guard lock(listings_->mutex);
        ++listings_->ended;
    }
    search::ListingBatch read(milliseconds timeout) override {
        std::unique_lock lock(listings_->mutex);
        if (listings_->held) {
            listings_->waiting = true;
            // Not for ever: a test that goes wrong while holding must fail rather than hang.
            listings_->changed.wait_for(lock, std::chrono::seconds(3),
                                        [&] { return !listings_->held; });
            listings_->waiting = false;
        }
        listings_->changed.wait_for(lock, timeout, [&] { return !listings_->batches.empty(); });
        if (listings_->batches.empty()) {
            return {};
        }
        search::ListingBatch batch = std::move(listings_->batches.front());
        listings_->batches.pop_front();
        return batch;
    }

private:
    std::shared_ptr<Listings> listings_;
};

class QuickOpenTest : public ::testing::Test {
protected:
    void SetUp() override {
        colors_ = std::make_unique<WorkbenchColors>(QApplication::palette());
        window_ = std::make_unique<QMainWindow>();
        window_->resize(800, 600);
        window_->show();
        quick_open_ = std::make_unique<QuickOpen>(window_.get(), *colors_);
        chosen_ = std::make_unique<QSignalSpy>(quick_open_.get(), &QuickOpen::chosen);
        QObject::connect(quick_open_.get(), &QuickOpen::chosen, window_.get(),
                         [this](const core::Uri& uri) { opened_.push_back(uri); });
        failed_ = std::make_unique<QSignalSpy>(quick_open_.get(), &QuickOpen::failed);
    }
    void TearDown() override {
        listings_->hold(false);
        quick_open_.reset();
    }

    static QuickOpenEntry entry(const std::string& path) {
        const std::filesystem::path file(path);
        return {.uri = core::Uri::file(file.is_absolute() ? file : "/w" / file),
                .label = file.filename().string(),
                .description = file.parent_path().string(),
                .path = path};
    }
    void open(std::vector<std::string> recent = {}) {
        std::vector<QuickOpenEntry> entries;
        for (const std::string& path : recent) {
            entries.push_back(entry(path));
        }
        quick_open_->open(
            std::move(entries),
            QuickOpen::Files{
                "/w",
                [listings = listings_]() -> std::expected<std::unique_ptr<FileSource>, Error> {
                    {
                        const std::lock_guard lock(listings->mutex);
                        ++listings->started;
                        if (listings->start_error) {
                            return std::unexpected(
                                Error{ErrorCode::io_error, *listings->start_error});
                        }
                    }
                    return std::make_unique<FakeSource>(listings);
                }});
    }
    QLineEdit* field() const {
        return quick_open_->pick()->filter_field();
    }
    void type(const char* text) {
        field()->setText(text);
    }
    void press(Qt::Key key) {
        QTest::keyClick(field(), key);
    }
    // What the popup shows, as paths relative to the folder.
    Names shown() const {
        Names names;
        for (const std::size_t item : quick_open_->pick()->shown_items()) {
            names.push_back(
                quick_open_->uri_of(item)->file_path()->lexically_relative("/w").string());
        }
        return names;
    }
    std::string current() const {
        const auto active = quick_open_->pick()->active_item();
        return active ? shown().at(*active) : std::string();
    }
    // Holds the thread of the listing in a read, so that it cannot answer.
    void hold_listing() {
        listings_->hold(true);
        ASSERT_TRUE(QTest::qWaitFor([&] { return listings_->is_waiting(); }, 3000));
    }
    template <class Done> bool wait_until(Done done) {
        return QTest::qWaitFor(done, 5000);
    }
    bool wait_shown(const Names& names) {
        return wait_until([&] { return shown() == names; });
    }

    std::shared_ptr<Listings> listings_ = std::make_shared<Listings>();
    std::unique_ptr<WorkbenchColors> colors_;
    std::unique_ptr<QMainWindow> window_;
    std::unique_ptr<QuickOpen> quick_open_;
    std::unique_ptr<QSignalSpy> chosen_;
    std::unique_ptr<QSignalSpy> failed_;
    std::vector<core::Uri> opened_;
};

// The files of `paths` that match, as `core` ranks them.
Names ranked(const Names& paths, const std::string& text) {
    const core::PreparedQuery query = core::prepare_query(text);
    const auto item = [](const std::string& path) {
        const std::size_t slash = path.rfind('/');
        const std::string_view view = path;
        return slash == std::string::npos
                   ? core::FuzzyItem{view, {}, view}
                   : core::FuzzyItem{view.substr(slash + 1), view.substr(0, slash), view};
    };
    std::vector<std::pair<std::string, core::ItemScore>> hits;
    for (const std::string& path : paths) {
        if (auto score = core::score_item(item(path), query); score.score != 0) {
            hits.emplace_back(path, std::move(score));
        }
    }
    std::ranges::stable_sort(hits, [&](const auto& a, const auto& b) {
        return core::compare_items(item(a.first), a.second, item(b.first), b.second, query) < 0;
    });
    Names names;
    for (const auto& [path, score] : hits) {
        names.push_back(path);
    }
    return names;
}

} // namespace

TEST_F(QuickOpenTest, WithAnEmptyFilterItOffersTheEntriesInTheirOrderAndNoFiles) {
    open({"b/second.txt", "first.txt"});
    EXPECT_EQ(shown(), (Names{"b/second.txt", "first.txt"}));
    listings_->give({"zzz.txt", "first.txt"}, true);
    ASSERT_TRUE(wait_until([&] { return quick_open_->is_complete(); }));
    EXPECT_EQ(shown(), (Names{"b/second.txt", "first.txt"}));
}

TEST_F(QuickOpenTest, TheFilesThatMatchAreRankedAsCoreRanksThem) {
    const Names files = {
        "src/domain.txt",    "src/main.cpp",        "docs/maintenance.md",  "m/a/i/n.h",
        "src/ui/window.cpp", "tools/make_index.py", "include/main/api.hpp", "mainland/readme.txt"};
    open();
    listings_->give(files, true);
    for (const char* text : {"main", "mn", "src/ma", "cpp", "api main", "zzzz"}) {
        type(text);
        EXPECT_TRUE(wait_shown(ranked(files, text)) && quick_open_->is_complete())
            << text << ": " << ::testing::PrintToString(shown());
    }
    EXPECT_FALSE(ranked(files, "main").empty());
}

TEST_F(QuickOpenTest, TheMatchedCharactersOfTheNameAndTheDirectoryAreHighlighted) {
    open();
    listings_->give({"src/main.cpp"}, true);
    type("srcmain");
    ASSERT_TRUE(wait_shown({"src/main.cpp"}));
    const core::ItemScore expected =
        core::score_item({"main.cpp", "src", "src/main.cpp"}, core::prepare_query("srcmain"));
    ASSERT_FALSE(expected.label_match.empty());
    ASSERT_FALSE(expected.description_match.empty());
    EXPECT_EQ(quick_open_->pick()->label_highlights(0), expected.label_match);
    const auto& item = quick_open_->pick()->items().at(0);
    EXPECT_EQ(item.label, "main.cpp");
    EXPECT_EQ(item.description, "src");
    EXPECT_EQ(item.description_match, expected.description_match);
}

TEST_F(QuickOpenTest, TheEntriesThatMatchComeBeforeTheFilesAndAreNotOfferedTwice) {
    open({"deep/down/notes_main.txt", "/elsewhere/main.log", "other.txt"});
    listings_->give({"main.cpp", "deep/down/notes_main.txt", "src/main.h"}, true);
    type("main");
    ASSERT_TRUE(wait_until([&] { return quick_open_->is_complete(); }));
    const Names names = shown();
    ASSERT_EQ(names.size(), 4U) << ::testing::PrintToString(names);
    // The two entries first, ranked among themselves, then the files.
    EXPECT_TRUE(
        std::ranges::is_permutation(Names(names.begin(), names.begin() + 2),
                                    Names{"deep/down/notes_main.txt", "../elsewhere/main.log"}));
    EXPECT_EQ(Names(names.begin() + 2, names.end()), ranked({"main.cpp", "src/main.h"}, "main"));
}

TEST_F(QuickOpenTest, AtMost512AreOffered) {
    Names files;
    for (int i = 0; i < 2000; ++i) {
        files.push_back("dir/file" + std::to_string(i) + ".txt");
    }
    open({"file_recent.txt"});
    listings_->give(files, true);
    type("file");
    ASSERT_TRUE(wait_until([&] { return quick_open_->is_complete(); }));
    EXPECT_EQ(shown().size(), QuickOpen::max_results);
    EXPECT_EQ(shown().front(), "file_recent.txt");
}

TEST_F(QuickOpenTest, MatchesShowWhileTheListingRuns) {
    open();
    type("a");
    ASSERT_TRUE(wait_until([&] { return listings_->count_of(&Listings::started) == 1; }));
    listings_->give({"a1"});
    ASSERT_TRUE(wait_shown({"a1"}));
    EXPECT_FALSE(quick_open_->is_complete());
    listings_->give({"b", "a2"});
    ASSERT_TRUE(wait_shown({"a1", "a2"}));
    listings_->give({"a0"}, true);
    ASSERT_TRUE(wait_until([&] { return quick_open_->is_complete(); }));
    EXPECT_EQ(shown(), (Names{"a0", "a1", "a2"}));
}

TEST_F(QuickOpenTest, TheEmptyListSaysWhetherThereIsNothingOrItIsStillSearching) {
    const auto message = [&] {
        for (const QLabel* label : quick_open_->pick()->findChildren<QLabel*>()) {
            if (label->isVisible()) {
                return label->text().toStdString();
            }
        }
        return std::string();
    };
    open();
    EXPECT_EQ(message(), "No recently opened files");
    type("x");
    EXPECT_TRUE(wait_until([&] { return message() == "Searching…"; })) << message();
    listings_->give({}, true);
    EXPECT_TRUE(wait_until([&] { return message() == "No matching results"; })) << message();
    type("");
    EXPECT_TRUE(wait_until([&] { return message() == "No recently opened files"; })) << message();
}

TEST_F(QuickOpenTest, ARowThatTheUserMovedToStaysChosenWhileMoreFilesArrive) {
    open();
    type("a");
    listings_->give({"a2", "a3"});
    ASSERT_TRUE(wait_shown({"a2", "a3"}));
    EXPECT_EQ(current(), "a2");
    listings_->give({"a1"});
    // Without a move the first row stays the chosen one.
    ASSERT_TRUE(wait_shown({"a1", "a2", "a3"}));
    EXPECT_EQ(current(), "a1");
    press(Qt::Key_Down);
    press(Qt::Key_Down);
    EXPECT_EQ(current(), "a3");
    listings_->give({"a0"}, true);
    ASSERT_TRUE(wait_shown({"a0", "a1", "a2", "a3"}));
    EXPECT_EQ(current(), "a3");
}

TEST_F(QuickOpenTest, ResultsForAnOlderFilterNeverReplaceThoseForANewerOne) {
    open({"start"});
    listings_->give({"alpha", "abacus"});
    ASSERT_TRUE(wait_until([&] { return listings_->count_of(&Listings::started) == 1; }));
    QTest::qWait(150);
    // The results of "al" are on their way, and the thread is held before it hears of "ab".
    type("al");
    std::this_thread::sleep_for(milliseconds(300));
    hold_listing();
    type("ab");
    QApplication::processEvents();
    EXPECT_EQ(std::ranges::count(shown(), "alpha"), 0) << ::testing::PrintToString(shown());
    listings_->hold(false);
    EXPECT_TRUE(wait_shown({"abacus"}));
}

TEST_F(QuickOpenTest, EnterBeforeTheAnswerChoosesWhatTheTypedFilterFinds) {
    open({"recent.txt"});
    ASSERT_TRUE(wait_until([&] { return listings_->count_of(&Listings::started) == 1; }));
    hold_listing();
    listings_->give({"target.txt"}, true);
    type("target");
    press(Qt::Key_Return);
    QApplication::processEvents();
    EXPECT_TRUE(opened_.empty());
    EXPECT_TRUE(quick_open_->is_open());
    listings_->hold(false);
    ASSERT_TRUE(wait_until([&] { return !opened_.empty(); }));
    EXPECT_EQ(opened_, std::vector{core::Uri::file("/w/target.txt")});
    EXPECT_FALSE(quick_open_->is_open());
    EXPECT_FALSE(quick_open_->pick()->isVisible());
}

TEST_F(QuickOpenTest, TypingOnDropsAHeldEnter) {
    open();
    ASSERT_TRUE(wait_until([&] { return listings_->count_of(&Listings::started) == 1; }));
    hold_listing();
    listings_->give({"target.txt"}, true);
    type("tar");
    press(Qt::Key_Return);
    type("targ");
    listings_->hold(false);
    ASSERT_TRUE(wait_until([&] { return quick_open_->is_complete(); }));
    EXPECT_TRUE(opened_.empty());
    EXPECT_TRUE(quick_open_->is_open());
}

TEST_F(QuickOpenTest, EnterOnAnAnsweredFilterChoosesAtOnce) {
    open({"one", "two"});
    press(Qt::Key_Down);
    press(Qt::Key_Return);
    EXPECT_EQ(opened_, std::vector{core::Uri::file("/w/two")});
}

TEST_F(QuickOpenTest, ClosingAndChoosingEndTheListingAndEachOpeningListsAgain) {
    open();
    ASSERT_TRUE(wait_until([&] { return listings_->count_of(&Listings::started) == 1; }));
    press(Qt::Key_Escape);
    EXPECT_FALSE(quick_open_->is_open());
    EXPECT_EQ(listings_->count_of(&Listings::ended), 1);
    EXPECT_TRUE(opened_.empty());

    open({"a"});
    ASSERT_TRUE(wait_until([&] { return listings_->count_of(&Listings::started) == 2; }));
    press(Qt::Key_Return);
    EXPECT_EQ(listings_->count_of(&Listings::ended), 2);
    EXPECT_EQ(opened_.size(), 1U);

    open();
    type("new");
    listings_->give({"new.txt"}, true);
    EXPECT_TRUE(wait_shown({"new.txt"}));
}

TEST_F(QuickOpenTest, AListingThatFailsIsReportedOnceAndWhatItListedIsKept) {
    open();
    type("kept");
    listings_->give({"kept.txt"}, true, "a folder cannot be read");
    ASSERT_TRUE(wait_until([&] { return quick_open_->is_complete(); }));
    EXPECT_EQ(shown(), Names{"kept.txt"});
    type("kep");
    ASSERT_TRUE(wait_until([&] { return quick_open_->is_complete(); }));
    ASSERT_EQ(failed_->count(), 1);
    EXPECT_EQ(failed_->at(0).at(0).toString(), "Listing the files failed: a folder cannot be read");
}

TEST_F(QuickOpenTest, AListingThatCannotStartIsReportedAndTheEntriesStay) {
    listings_->start_error = "cannot start rg";
    open({"recent.txt"});
    ASSERT_TRUE(wait_until([&] { return failed_->count() == 1; }));
    EXPECT_EQ(failed_->at(0).at(0).toString(), "Listing the files failed: cannot start rg");
    type("rec");
    ASSERT_TRUE(wait_until([&] { return quick_open_->is_complete(); }));
    EXPECT_EQ(shown(), Names{"recent.txt"});
}

TEST_F(QuickOpenTest, WithoutAFolderOnlyTheEntriesAreMatched) {
    quick_open_->open({entry("alpha"), entry("beta")}, std::nullopt);
    type("bet");
    ASSERT_TRUE(wait_until([&] { return quick_open_->is_complete(); }));
    EXPECT_EQ(shown(), Names{"beta"});
    EXPECT_EQ(listings_->count_of(&Listings::started), 0);
}

TEST_F(QuickOpenTest, TypingDoesNotWaitForAHundredThousandFilesToBeMatched) {
    Names files;
    files.reserve(100000);
    for (int i = 0; i < 100000; ++i) {
        files.push_back("module_" + std::to_string(i % 1000) + "/component/file_name_" +
                        std::to_string(i) + ".cpp");
    }
    open();
    listings_->give(files, true);
    QElapsedTimer waiting;
    waiting.start();
    while (!quick_open_->is_complete() && waiting.elapsed() < 30000) {
        QTest::qWait(5);
    }
    ASSERT_TRUE(quick_open_->is_complete());

    qint64 slowest_key = 0;
    qint64 slowest_turn = 0;
    const auto press_timed = [&](char key) {
        QElapsedTimer timer;
        timer.start();
        QTest::keyClick(field(), key);
        slowest_key = std::max(slowest_key, timer.nsecsElapsed());
    };
    const std::string keys = "mo4file9";
    for (std::size_t i = 0; i < keys.size(); i += 2) {
        press_timed(keys[i]);
        // The next key comes while the files are being matched against the filter so far.
        std::this_thread::sleep_for(milliseconds(5));
        press_timed(keys[i + 1]);
        QElapsedTimer timer;
        // The turns of the event loop until the answer is in, which show it.
        waiting.restart();
        while (!quick_open_->is_complete() && waiting.elapsed() < 30000) {
            std::this_thread::sleep_for(milliseconds(1));
            timer.restart();
            QApplication::processEvents();
            slowest_turn = std::max(slowest_turn, timer.nsecsElapsed());
        }
        ASSERT_TRUE(quick_open_->is_complete());
    }
    EXPECT_EQ(shown().size(), QuickOpen::max_results);
    if (anvil::test::timing_is_meaningful) {
        EXPECT_LT(slowest_key, 16'000'000) << slowest_key / 1000 << " us for a key press";
        EXPECT_LT(slowest_turn, 16'000'000) << slowest_turn / 1000 << " us to show results";
    }
}
