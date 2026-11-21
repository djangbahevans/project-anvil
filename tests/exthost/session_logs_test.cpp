#include "anvil/exthost/session_logs.hpp"

#include <algorithm>
#include <fstream>
#include <gtest/gtest.h>
#include <unistd.h>

using namespace anvil::exthost;
namespace fs = std::filesystem;
using std::chrono::seconds;

namespace {

class SessionLogsTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = fs::temp_directory_path() / ("anvil-session-logs-" + std::to_string(::getpid())) /
                "logs";
        fs::remove_all(root_.parent_path());
    }
    void TearDown() override {
        fs::remove_all(root_.parent_path());
    }

    // The names of the directories under the root, in order.
    std::vector<std::string> directories() const {
        std::vector<std::string> names;
        for (const auto& entry : fs::directory_iterator(root_)) {
            if (entry.is_directory()) {
                names.push_back(entry.path().filename().string());
            }
        }
        std::sort(names.begin(), names.end());
        return names;
    }

    // A session name `offset` seconds after a fixed time.
    static std::string name_at(int offset, std::int64_t pid = 100) {
        const std::chrono::sys_seconds base = std::chrono::sys_days{
            std::chrono::year{2026} / std::chrono::month{11} / std::chrono::day{21}};
        return session_logs_name(base + seconds(offset), pid);
    }

    fs::path root_;
};

} // namespace

TEST(SessionLogsName, IsAUtcTimestampAndTheProcessId) {
    const std::chrono::sys_seconds time =
        std::chrono::sys_days{std::chrono::year{2026} / std::chrono::month{11} /
                              std::chrono::day{21}} +
        std::chrono::hours(14) + std::chrono::minutes(33) + seconds(58);
    EXPECT_EQ(session_logs_name(time, 1234), "20261121T143358Z-1234");
}

TEST(SessionLogsName, SortsInTimeOrder) {
    const auto early = std::chrono::sys_days{std::chrono::year{2026} / std::chrono::month{2} /
                                             std::chrono::day{3}};
    const auto late = std::chrono::sys_days{std::chrono::year{2026} / std::chrono::month{11} /
                                            std::chrono::day{21}};
    EXPECT_LT(session_logs_name(early, 99999), session_logs_name(late, 1));
}

TEST(SessionLogsName, RecognizesOnlyItsOwnNames) {
    EXPECT_TRUE(is_session_logs_name("20261121T143358Z-1234"));
    EXPECT_TRUE(is_session_logs_name("20261121T143358Z-1"));
    EXPECT_TRUE(is_session_logs_name("20261121T143358Z-1234-2"));
    for (const char* other :
         {"", "logs", "exthost.log", "20261121T143358-1234", "20261121143358Z-1234",
          "20261121T143358Z-", "20261121T143358Z-12a", "x20261121T143358Z-1",
          "20261121T143358Z-1234 ", "20261121T143358Z-1234-", "20261121T143358Z-1234-a",
          "20261121T143358Z-1234-2-3"}) {
        EXPECT_FALSE(is_session_logs_name(other)) << '"' << other << '"';
    }
}

TEST_F(SessionLogsTest, CreatesTheDirectoryAndItsParents) {
    const auto path = create_session_logs(root_, name_at(0));
    ASSERT_TRUE(path.has_value()) << path.error().message;
    EXPECT_EQ(*path, root_ / name_at(0));
    EXPECT_TRUE(fs::is_directory(*path));
}

TEST_F(SessionLogsTest, AnExistingDirectoryOfTheSameNameIsReused) {
    ASSERT_TRUE(create_session_logs(root_, name_at(0)).has_value());
    std::ofstream(root_ / name_at(0) / "exthost.log") << "kept\n";
    ASSERT_TRUE(create_session_logs(root_, name_at(0)).has_value());
    EXPECT_TRUE(fs::exists(root_ / name_at(0) / "exthost.log"));
}

TEST_F(SessionLogsTest, KeepsTheNewestSessionsIncludingTheCurrentOne) {
    for (int i = 0; i < 15; ++i) {
        fs::create_directories(root_ / name_at(i));
    }
    const auto path = create_session_logs(root_, name_at(20), 10);
    ASSERT_TRUE(path.has_value()) << path.error().message;

    std::vector<std::string> expected;
    for (int i = 6; i < 15; ++i) {
        expected.push_back(name_at(i));
    }
    expected.push_back(name_at(20));
    EXPECT_EQ(directories(), expected);
}

TEST_F(SessionLogsTest, TheCurrentDirectoryIsKeptEvenWhenItIsTheOldest) {
    for (int i = 10; i < 20; ++i) {
        fs::create_directories(root_ / name_at(i));
    }
    ASSERT_TRUE(create_session_logs(root_, name_at(1), 3).has_value());
    EXPECT_EQ(directories(), (std::vector<std::string>{name_at(1), name_at(18), name_at(19)}));
}

TEST_F(SessionLogsTest, ALimitOfZeroOrOneKeepsOnlyTheCurrentSession) {
    for (const std::size_t keep : {std::size_t{0}, std::size_t{1}}) {
        fs::remove_all(root_);
        for (int i = 0; i < 4; ++i) {
            fs::create_directories(root_ / name_at(i));
        }
        ASSERT_TRUE(create_session_logs(root_, name_at(9), keep).has_value());
        EXPECT_EQ(directories(), (std::vector<std::string>{name_at(9)})) << keep;
    }
}

TEST_F(SessionLogsTest, EntriesThatAreNotSessionDirectoriesAreLeftAlone) {
    fs::create_directories(root_ / "notes");
    fs::create_directories(root_ / "2026-bad-name");
    std::ofstream(root_ / "exthost.log") << "old log\n";
    // A file named like a session is not a session directory.
    std::ofstream(root_ / name_at(1)) << "not a directory\n";
    for (int i = 2; i < 8; ++i) {
        fs::create_directories(root_ / name_at(i));
    }
    ASSERT_TRUE(create_session_logs(root_, name_at(9), 2).has_value());

    EXPECT_TRUE(fs::is_directory(root_ / "notes"));
    EXPECT_TRUE(fs::is_directory(root_ / "2026-bad-name"));
    EXPECT_TRUE(fs::is_regular_file(root_ / "exthost.log"));
    EXPECT_TRUE(fs::is_regular_file(root_ / name_at(1)));
    const auto names = directories();
    EXPECT_EQ(std::count_if(names.begin(), names.end(), is_session_logs_name), 2);
}

TEST_F(SessionLogsTest, ItFailsWhenTheDirectoryCannotBeCreated) {
    fs::create_directories(root_.parent_path());
    std::ofstream(root_) << "a file where the logs directory should be\n";
    const auto path = create_session_logs(root_, name_at(0));
    ASSERT_FALSE(path.has_value());
    EXPECT_NE(path.error().message.find(root_.string()), std::string::npos) << path.error().message;
}

// A host that is started again in the same process within one second must not share, and so
// replay, the directory of the one before it.
TEST_F(SessionLogsTest, SessionsStartedInTheSameSecondByTheSameProcessGetDirectoriesOfTheirOwn) {
    const std::chrono::sys_seconds time = std::chrono::sys_days{
        std::chrono::year{2026} / std::chrono::month{11} / std::chrono::day{21}};
    std::vector<fs::path> paths;
    for (int i = 0; i < 3; ++i) {
        const auto path = create_session_logs(root_, time, 1234);
        ASSERT_TRUE(path.has_value()) << path.error().message;
        paths.push_back(*path);
    }
    EXPECT_EQ(paths[0].filename(), "20261121T000000Z-1234");
    EXPECT_EQ(paths[1].filename(), "20261121T000000Z-1234-1");
    EXPECT_EQ(paths[2].filename(), "20261121T000000Z-1234-2");
    EXPECT_EQ(directories().size(), 3u);
    // They sort in the order they were created.
    EXPECT_TRUE(std::is_sorted(paths.begin(), paths.end()));
}

TEST_F(SessionLogsTest, TheDefaultNameUsesTheClockAndTheProcessId) {
    const auto path = create_session_logs(root_);
    ASSERT_TRUE(path.has_value()) << path.error().message;
    EXPECT_TRUE(is_session_logs_name(path->filename().string())) << path->filename();
    EXPECT_NE(path->filename().string().find("-" + std::to_string(::getpid())), std::string::npos);
}
