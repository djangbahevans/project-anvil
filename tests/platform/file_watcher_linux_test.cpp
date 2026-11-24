#include "anvil/platform/file_watcher.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <unistd.h>
#include <vector>

using namespace anvil;
using namespace anvil::platform;
using std::chrono::milliseconds;
namespace fs = std::filesystem;

namespace {

constexpr FileEventKind created = FileEventKind::created;
constexpr FileEventKind changed = FileEventKind::changed;
constexpr FileEventKind deleted = FileEventKind::deleted;

class FileWatcherTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = fs::weakly_canonical(fs::temp_directory_path()) /
                ("anvil-file-watcher-test-" + std::to_string(::getpid()));
        fs::remove_all(root_);
        fs::create_directories(root_ / "watched");
        watched_ = root_ / "watched";
    }
    void TearDown() override {
        watcher_.reset();
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    void start(WatchOptions options = {}, const fs::path& path = {}) {
        auto started = FileWatcher::start(path.empty() ? watched_ : path, options);
        ASSERT_TRUE(started.has_value()) << started.error().message;
        watcher_ = std::move(*started);
    }

    static void write(const fs::path& path, const std::string& text) {
        std::ofstream(path, std::ios::app) << text;
    }

    // Polls until `done` holds for the events collected so far, or the deadline passes.
    template <class Done> bool wait_for(Done done) {
        const auto deadline = std::chrono::steady_clock::now() + milliseconds(10000);
        while (!done(events_) && std::chrono::steady_clock::now() < deadline) {
            const auto polled = watcher_->poll(milliseconds(100));
            if (!polled) {
                error_ = polled.error();
                continue;
            }
            events_.insert(events_.end(), polled->begin(), polled->end());
        }
        return done(events_);
    }

    bool wait_for_event(FileEventKind kind, const fs::path& path) {
        return wait_for(
            [&](const std::vector<FileEvent>& events) { return has(events, kind, path); });
    }

    static bool has(const std::vector<FileEvent>& events, FileEventKind kind,
                    const fs::path& path) {
        return std::ranges::find(events, FileEvent{kind, path}) != events.end();
    }

    static std::size_t count(const std::vector<FileEvent>& events, const fs::path& path) {
        return static_cast<std::size_t>(std::ranges::count(events, path, &FileEvent::path));
    }

    // Takes what is delivered within `timeout`, which is nothing when the test expects silence.
    std::vector<FileEvent> take_for(milliseconds timeout) {
        std::vector<FileEvent> taken;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto polled = watcher_->poll(milliseconds(20));
            if (polled) {
                taken.insert(taken.end(), polled->begin(), polled->end());
            }
        }
        return taken;
    }

    fs::path root_;
    fs::path watched_;
    std::unique_ptr<FileWatcher> watcher_;
    std::vector<FileEvent> events_;
    std::optional<Error> error_;
};

} // namespace

TEST_F(FileWatcherTest, ReportsAFileThatIsCreatedChangedAndDeleted) {
    start();
    const fs::path file = watched_ / "a.txt";

    write(file, "one");
    ASSERT_TRUE(wait_for_event(created, file));
    EXPECT_FALSE(has(events_, changed, file)) << "the write of a new file is part of its creation";

    events_.clear();
    write(file, "two");
    ASSERT_TRUE(wait_for_event(changed, file));

    events_.clear();
    fs::remove(file);
    ASSERT_TRUE(wait_for_event(deleted, file));
}

TEST_F(FileWatcherTest, ARenamedFileIsDeletedAndCreated) {
    write(watched_ / "old.txt", "x");
    start();

    fs::rename(watched_ / "old.txt", watched_ / "new.txt");
    ASSERT_TRUE(wait_for_event(created, watched_ / "new.txt"));
    EXPECT_TRUE(has(events_, deleted, watched_ / "old.txt"));
}

TEST_F(FileWatcherTest, ReportsNothingForWhatExistsWhenTheWatchStarts) {
    write(watched_ / "a.txt", "x");
    fs::create_directories(watched_ / "sub");
    write(watched_ / "sub" / "b.txt", "x");
    start();

    EXPECT_TRUE(take_for(milliseconds(300)).empty());
}

TEST_F(FileWatcherTest, AFileInANewDirectoryIsReported) {
    start();
    const fs::path directory = watched_ / "sub";

    fs::create_directory(directory);
    ASSERT_TRUE(wait_for_event(created, directory));

    write(directory / "late.txt", "x");
    EXPECT_TRUE(wait_for_event(created, directory / "late.txt"))
        << "a directory that is created is watched at once";

    fs::create_directory(directory / "deeper");
    ASSERT_TRUE(wait_for_event(created, directory / "deeper"));
    write(directory / "deeper" / "x.txt", "x");
    EXPECT_TRUE(wait_for_event(created, directory / "deeper" / "x.txt"));
}

TEST_F(FileWatcherTest, WhatAMovedInDirectoryHoldsIsCreated) {
    start();
    fs::create_directories(root_ / "elsewhere" / "inner");
    write(root_ / "elsewhere" / "a.txt", "x");
    write(root_ / "elsewhere" / "inner" / "b.txt", "x");

    fs::rename(root_ / "elsewhere", watched_ / "arrived");
    const fs::path arrived = watched_ / "arrived";
    ASSERT_TRUE(wait_for_event(created, arrived / "inner" / "b.txt"));
    EXPECT_TRUE(has(events_, created, arrived));
    EXPECT_TRUE(has(events_, created, arrived / "a.txt"));
    EXPECT_TRUE(has(events_, created, arrived / "inner"));
}

TEST_F(FileWatcherTest, ABurstOfWritesForOnePathIsOneEvent) {
    write(watched_ / "a.txt", "x");
    write(watched_ / "b.txt", "x");
    start({.latency = milliseconds(300)});

    // Alternating files keeps the system from joining the events itself.
    for (int i = 0; i < 20; ++i) {
        write(watched_ / "a.txt", "y");
        write(watched_ / "b.txt", "y");
    }
    ASSERT_TRUE(wait_for([&](const std::vector<FileEvent>& events) {
        return has(events, changed, watched_ / "a.txt") && has(events, changed, watched_ / "b.txt");
    }));
    const auto more = take_for(milliseconds(400));
    events_.insert(events_.end(), more.begin(), more.end());

    EXPECT_EQ(events_.size(), 2u);
    EXPECT_EQ(count(events_, watched_ / "a.txt"), 1u);
    EXPECT_EQ(count(events_, watched_ / "b.txt"), 1u);
}

TEST_F(FileWatcherTest, ACreatedFileThatIsDeletedInTheSameBurstIsNotReported) {
    start({.latency = milliseconds(300)});

    write(watched_ / "gone.txt", "x");
    fs::remove(watched_ / "gone.txt");
    write(watched_ / "kept.txt", "x");

    ASSERT_TRUE(wait_for_event(created, watched_ / "kept.txt"));
    EXPECT_EQ(count(events_, watched_ / "gone.txt"), 0u);
}

TEST_F(FileWatcherTest, AFileThatIsDeletedAndCreatedAgainInOneBurstHasChanged) {
    write(watched_ / "a.txt", "x");
    start({.latency = milliseconds(300)});

    fs::remove(watched_ / "a.txt");
    write(watched_ / "a.txt", "new");

    ASSERT_TRUE(wait_for_event(changed, watched_ / "a.txt"));
    EXPECT_EQ(count(events_, watched_ / "a.txt"), 1u);
}

TEST_F(FileWatcherTest, ExcludedPathsAreNeitherWatchedNorReported) {
    fs::create_directories(watched_ / "node_modules" / "pkg");
    start({.excludes = {"**/node_modules/**", "*.tmp"}});

    write(watched_ / "node_modules" / "pkg" / "index.js", "x");
    fs::create_directories(watched_ / "src" / "node_modules");
    write(watched_ / "src" / "node_modules" / "dep.js", "x");
    write(watched_ / "scratch.tmp", "x");
    write(watched_ / "seen.txt", "x");

    ASSERT_TRUE(wait_for_event(created, watched_ / "seen.txt"));
    const auto more = take_for(milliseconds(300));
    events_.insert(events_.end(), more.begin(), more.end());
    for (const FileEvent& event : events_) {
        const std::string path = event.path.string();
        EXPECT_EQ(path.find("node_modules/"), std::string::npos) << path;
        EXPECT_EQ(path.find(".tmp"), std::string::npos) << path;
    }
}

TEST_F(FileWatcherTest, WithoutRecursionOnlyTheDirectoryItselfIsWatched) {
    fs::create_directories(watched_ / "sub");
    start({.recursive = false});

    write(watched_ / "sub" / "inner.txt", "x");
    fs::create_directories(watched_ / "made");
    write(watched_ / "top.txt", "x");

    ASSERT_TRUE(wait_for_event(created, watched_ / "top.txt"));
    EXPECT_TRUE(has(events_, created, watched_ / "made"));
    write(watched_ / "made" / "inner.txt", "x");
    const auto more = take_for(milliseconds(300));
    events_.insert(events_.end(), more.begin(), more.end());
    EXPECT_FALSE(has(events_, created, watched_ / "sub" / "inner.txt"));
    EXPECT_FALSE(has(events_, created, watched_ / "made" / "inner.txt"));
}

TEST_F(FileWatcherTest, AStoppedWatchReportsNothingMore) {
    start();
    watcher_->stop();

    write(watched_ / "a.txt", "x");
    const auto polled = watcher_->poll(milliseconds(200));
    ASSERT_TRUE(polled.has_value());
    EXPECT_TRUE(polled->empty());
    watcher_->stop();
}

TEST_F(FileWatcherTest, ADeletedDirectoryEndsItsWatchAndANewOneIsWatchedAgain) {
    fs::create_directories(watched_ / "sub");
    write(watched_ / "sub" / "a.txt", "x");
    start();

    fs::remove_all(watched_ / "sub");
    ASSERT_TRUE(wait_for_event(deleted, watched_ / "sub"));
    EXPECT_TRUE(has(events_, deleted, watched_ / "sub" / "a.txt"));

    events_.clear();
    fs::create_directories(watched_ / "sub");
    ASSERT_TRUE(wait_for_event(created, watched_ / "sub"));
    write(watched_ / "sub" / "b.txt", "x");
    EXPECT_TRUE(wait_for_event(created, watched_ / "sub" / "b.txt"));
}

TEST_F(FileWatcherTest, ARenamedDirectoryIsWatchedUnderItsNewName) {
    fs::create_directories(watched_ / "before");
    start();

    fs::rename(watched_ / "before", watched_ / "after");
    ASSERT_TRUE(wait_for_event(created, watched_ / "after"));
    EXPECT_TRUE(has(events_, deleted, watched_ / "before"));

    events_.clear();
    write(watched_ / "after" / "a.txt", "x");
    ASSERT_TRUE(wait_for_event(created, watched_ / "after" / "a.txt"));
    EXPECT_FALSE(has(events_, created, watched_ / "before" / "a.txt"));
}

TEST_F(FileWatcherTest, ADirectoryThatMovesOutOfTheTreeIsNotWatchedAnymore) {
    fs::create_directories(watched_ / "sub" / "inner");
    start();

    fs::rename(watched_ / "sub", root_ / "outside");
    ASSERT_TRUE(wait_for_event(deleted, watched_ / "sub"));

    write(root_ / "outside" / "x.txt", "x");
    write(root_ / "outside" / "inner" / "y.txt", "x");
    write(watched_ / "marker.txt", "x");
    ASSERT_TRUE(wait_for_event(created, watched_ / "marker.txt"));
    const auto more = take_for(milliseconds(200));
    events_.insert(events_.end(), more.begin(), more.end());
    EXPECT_EQ(events_.size(), 2u) << "the moved directory and the marker";
}

TEST_F(FileWatcherTest, PollReturnsWhenTheBurstIsDueAndNotAtTheEndOfTheTimeout) {
    start({.latency = milliseconds(50)});

    write(watched_ / "a.txt", "x");
    const auto begin = std::chrono::steady_clock::now();
    std::vector<FileEvent> events;
    while (events.empty() && std::chrono::steady_clock::now() - begin < milliseconds(10000)) {
        const auto polled = watcher_->poll(milliseconds(5000));
        ASSERT_TRUE(polled.has_value());
        events = *polled;
    }
    EXPECT_EQ(events.size(), 1u);
    EXPECT_LT(std::chrono::steady_clock::now() - begin, milliseconds(2000));
}

TEST_F(FileWatcherTest, ADeletedWatchedDirectoryIsReported) {
    start();

    fs::remove_all(watched_);
    ASSERT_TRUE(wait_for_event(deleted, watched_));
}

TEST_F(FileWatcherTest, AFileCanBeWatched) {
    const fs::path file = watched_ / "one.txt";
    write(file, "x");
    write(watched_ / "other.txt", "x");
    start({}, file);

    write(watched_ / "other.txt", "y");
    write(file, "y");
    ASSERT_TRUE(wait_for_event(changed, file));
    EXPECT_EQ(events_.size(), 1u);

    fs::remove(file);
    ASSERT_TRUE(wait_for_event(deleted, file));
}

TEST_F(FileWatcherTest, APathThatDoesNotExistCannotBeWatched) {
    const auto started = FileWatcher::start(watched_ / "missing");
    ASSERT_FALSE(started.has_value());
    EXPECT_EQ(started.error().code, ErrorCode::not_found);
}

TEST_F(FileWatcherTest, TooManyDirectoriesFailTheStart) {
    fs::create_directories(watched_ / "a");
    fs::create_directories(watched_ / "b");
    fs::create_directories(watched_ / "c");

    const auto started = FileWatcher::start(watched_, {.max_directories = 3});
    ASSERT_FALSE(started.has_value());
    EXPECT_EQ(started.error().code, ErrorCode::limit_exceeded);
}

TEST_F(FileWatcherTest, ADirectoryBeyondTheLimitIsReportedAsAnErrorAndTheWatchGoesOn) {
    start({.max_directories = 1});

    fs::create_directory(watched_ / "sub");
    write(watched_ / "top.txt", "x");
    ASSERT_TRUE(wait_for([&](const std::vector<FileEvent>& events) {
        return has(events, created, watched_ / "top.txt");
    }));
    ASSERT_TRUE(has(events_, created, watched_ / "sub"));

    const auto problem = watcher_->poll(milliseconds(1000));
    ASSERT_FALSE(problem.has_value());
    EXPECT_EQ(problem.error().code, ErrorCode::limit_exceeded);
    const auto again = watcher_->poll(milliseconds(50));
    ASSERT_TRUE(again.has_value()) << "the error is returned once";

    write(watched_ / "later.txt", "x");
    EXPECT_TRUE(wait_for_event(created, watched_ / "later.txt"));
}
