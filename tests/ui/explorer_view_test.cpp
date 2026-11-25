#include "../timing.hpp"
#include "anvil/ui/explorer_view.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <QTreeView>
#include <algorithm>
#include <fstream>
#include <gtest/gtest.h>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <unistd.h>

using namespace anvil;
using namespace anvil::ui;
using anvil::test::timing_is_meaningful;
using core::FileTreeEntry;

namespace {

FileTreeEntry file(std::string name) {
    return {.name = std::move(name)};
}
FileTreeEntry dir(std::string name) {
    return {.name = std::move(name), .is_directory = true};
}

// A file system that lists what the test put in it and counts the listings.
class FakeFileSystem {
public:
    void set(const std::string& directory, std::vector<FileTreeEntry> entries) {
        contents_[directory] = std::move(entries);
    }
    void fail(const std::string& directory, std::string message) {
        failures_[directory] = std::move(message);
    }
    int listings(const std::string& directory) const {
        const auto found = listings_.find(directory);
        return found == listings_.end() ? 0 : found->second;
    }

    core::DirectoryLister lister() {
        return [this](const std::filesystem::path& directory)
                   -> std::expected<std::vector<FileTreeEntry>, Error> {
            const std::string key = directory.generic_string();
            ++listings_[key];
            if (const auto failure = failures_.find(key); failure != failures_.end()) {
                return std::unexpected(Error{ErrorCode::permission_denied, failure->second});
            }
            const auto found = contents_.find(key);
            if (found == contents_.end()) {
                return std::unexpected(Error{ErrorCode::not_found, "no such directory " + key});
            }
            return found->second;
        };
    }

private:
    std::map<std::string, std::vector<FileTreeEntry>> contents_;
    std::map<std::string, std::string> failures_;
    std::map<std::string, int> listings_;
};

// A watcher that the test drives: what it was asked to watch, the events it has queued and the
// loss it will report.
struct WatchState {
    std::filesystem::path folder;
    std::set<std::string> directories;
    std::vector<std::filesystem::path> events;
    std::optional<Error> loss;
    std::optional<Error> add_failure;
    bool alive = true;
};

class FakeWatch : public DirectoryWatch {
public:
    explicit FakeWatch(std::shared_ptr<WatchState> state) : state_(std::move(state)) {}
    ~FakeWatch() override {
        state_->alive = false;
    }

    std::expected<void, Error> add(const std::filesystem::path& directory) override {
        if (state_->add_failure) {
            return std::unexpected(*state_->add_failure);
        }
        state_->directories.insert(directory.generic_string());
        return {};
    }
    void remove(const std::filesystem::path& directory) override {
        state_->directories.erase(directory.generic_string());
    }
    std::expected<std::vector<std::filesystem::path>, Error> poll() override {
        if (state_->loss) {
            Error loss = *state_->loss;
            state_->loss.reset();
            return std::unexpected(std::move(loss));
        }
        return std::exchange(state_->events, {});
    }

private:
    std::shared_ptr<WatchState> state_;
};

class ExplorerViewTest : public ::testing::Test {
protected:
    // Lists at once, so that a test sees the entries after the event loop has run once.
    virtual std::unique_ptr<ListingRunner> make_runner() {
        return immediate_listing_runner();
    }

    DirectoryWatchFactory watch_factory() {
        return [this](const std::filesystem::path& folder)
                   -> std::expected<std::unique_ptr<DirectoryWatch>, Error> {
            if (start_failure_) {
                return std::unexpected(*start_failure_);
            }
            auto state = std::make_shared<WatchState>();
            state->folder = folder;
            watches_.push_back(state);
            return std::make_unique<FakeWatch>(state);
        };
    }

    void SetUp() override {
        files_.set("/w", {file("zeta.txt"), dir("src"), file("a10.txt"), file("a9.txt"),
                          dir("docs"), dir(".git"), dir("build")});
        files_.set("/w/src", {file("main.cpp"), dir("util"), file("app.cpp")});
        files_.set("/w/src/util", {file("strings.cpp")});
        files_.set("/w/docs", {});
        files_.set("/w/build", {file("out.o")});
        colors_ = std::make_unique<WorkbenchColors>(QApplication::palette());
        view_ = std::make_unique<ExplorerView>(*colors_, nullptr, files_.lister(), watch_factory(),
                                               make_runner());
        view_->resize(300, 400);
        view_->show();
        QApplication::processEvents();
    }

    void open_workspace() {
        view_->set_workspace(std::filesystem::path("/w"), "w");
        QApplication::processEvents();
    }

    QTreeView* tree() const {
        return view_->tree();
    }
    // Expands a directory and lets the listing, which is delivered through the event loop, arrive.
    void expand(const QModelIndex& index) {
        tree()->expand(index);
        QApplication::processEvents();
    }
    QModelIndex index_of(const char* path) const {
        return view_->model()->index_of(path);
    }
    std::vector<std::string> names_under(const QModelIndex& parent) const {
        std::vector<std::string> names;
        for (int row = 0; row < view_->model()->rowCount(parent); ++row) {
            names.push_back(view_->model()->index(row, 0, parent).data().toString().toStdString());
        }
        return names;
    }
    QPoint center_of(const QModelIndex& index) const {
        return tree()->visualRect(index).center();
    }
    // What a double click sends: the first click, then the second as a double click event.
    void double_click(const QModelIndex& index) {
        const QPoint position = center_of(index);
        QTest::mouseClick(tree()->viewport(), Qt::LeftButton, Qt::NoModifier, position);
        QTest::mouseDClick(tree()->viewport(), Qt::LeftButton, Qt::NoModifier, position);
    }
    void key(Qt::Key key_code) {
        QTest::keyClick(tree(), key_code);
    }
    QString current_name() const {
        return tree()->currentIndex().data().toString();
    }

    FakeFileSystem files_;
    std::vector<std::shared_ptr<WatchState>> watches_;
    std::optional<Error> start_failure_;
    std::unique_ptr<WorkbenchColors> colors_;
    std::unique_ptr<ExplorerView> view_;
};

} // namespace

using Names = std::vector<std::string>;

TEST_F(ExplorerViewTest, WithNoFolderItOffersToOpenOne) {
    EXPECT_FALSE(view_->shows_folder());
    EXPECT_EQ(view_->message()->text(), "You have not yet opened a folder.");
    EXPECT_TRUE(view_->message()->isVisible());
    EXPECT_FALSE(tree()->isVisible());
    ASSERT_NE(view_->open_folder_button(), nullptr);
    EXPECT_EQ(view_->open_folder_button()->text(), "Open Folder");

    QSignalSpy requested(view_.get(), &ExplorerView::open_folder_requested);
    QTest::mouseClick(view_->open_folder_button(), Qt::LeftButton);
    EXPECT_EQ(requested.count(), 1);
}

TEST_F(ExplorerViewTest, AFolderShowsItsNameAndItsEntriesWithFoldersFirst) {
    open_workspace();
    EXPECT_TRUE(view_->shows_folder());
    EXPECT_FALSE(view_->message()->isVisible());
    EXPECT_TRUE(tree()->isVisible());
    EXPECT_EQ(view_->folder_name(), "w");
    EXPECT_EQ(names_under({}), (Names{"build", "docs", "src", "a9.txt", "a10.txt", "zeta.txt"}));
    EXPECT_EQ(files_.listings("/w"), 1);
    EXPECT_EQ(files_.listings("/w/src"), 0);
}

TEST_F(ExplorerViewTest, ExpandingADirectoryListsItOnceAndShowsItsEntriesInOrder) {
    open_workspace();
    const QModelIndex src = index_of("/w/src");
    ASSERT_TRUE(src.isValid());
    EXPECT_EQ(files_.listings("/w/src"), 0);
    EXPECT_EQ(view_->model()->rowCount(src), 0);

    expand(src);
    EXPECT_EQ(files_.listings("/w/src"), 1);
    EXPECT_EQ(names_under(src), (Names{"util", "app.cpp", "main.cpp"}));

    tree()->collapse(src);
    expand(src);
    key(Qt::Key_Down);
    EXPECT_EQ(files_.listings("/w/src"), 1);
    EXPECT_EQ(files_.listings("/w/src/util"), 0);
}

TEST_F(ExplorerViewTest, EnterOnAFileActivatesItAndOnAFolderExpandsAndCollapsesIt) {
    open_workspace();
    QSignalSpy activated(view_.get(), &ExplorerView::file_activated);

    tree()->setCurrentIndex(index_of("/w/a9.txt"));
    key(Qt::Key_Return);
    ASSERT_EQ(activated.count(), 1);
    EXPECT_EQ(activated.takeFirst().at(0).toString(), "/w/a9.txt");
    tree()->setCurrentIndex(index_of("/w/zeta.txt"));
    key(Qt::Key_Enter);
    ASSERT_EQ(activated.count(), 1);
    EXPECT_EQ(activated.takeFirst().at(0).toString(), "/w/zeta.txt");

    const QModelIndex src = index_of("/w/src");
    tree()->setCurrentIndex(src);
    key(Qt::Key_Return);
    EXPECT_TRUE(tree()->isExpanded(src));
    EXPECT_EQ(activated.count(), 0);
    key(Qt::Key_Return);
    EXPECT_FALSE(tree()->isExpanded(src));
    EXPECT_EQ(activated.count(), 0);
}

TEST_F(ExplorerViewTest, ADoubleClickActivatesAFileAndTogglesAFolder) {
    open_workspace();
    QSignalSpy activated(view_.get(), &ExplorerView::file_activated);
    const QModelIndex a9 = index_of("/w/a9.txt");
    double_click(a9);
    ASSERT_EQ(activated.count(), 1);
    EXPECT_EQ(activated.takeFirst().at(0).toString(), "/w/a9.txt");

    const QModelIndex src = index_of("/w/src");
    double_click(src);
    EXPECT_TRUE(tree()->isExpanded(src));
    double_click(src);
    EXPECT_FALSE(tree()->isExpanded(src));
    EXPECT_EQ(activated.count(), 0);
}

TEST_F(ExplorerViewTest, ASingleClickSelectsAFileAndDoesNotOpenIt) {
    open_workspace();
    QSignalSpy activated(view_.get(), &ExplorerView::file_activated);
    const QModelIndex a9 = index_of("/w/a9.txt");
    QTest::mouseClick(tree()->viewport(), Qt::LeftButton, Qt::NoModifier, center_of(a9));
    EXPECT_EQ(tree()->currentIndex(), a9);
    EXPECT_EQ(activated.count(), 0);
}

TEST_F(ExplorerViewTest, TheKeysMoveAsATreeDoes) {
    open_workspace();
    tree()->setFocus();
    tree()->setCurrentIndex(index_of("/w/build"));
    EXPECT_EQ(current_name(), "build");

    key(Qt::Key_Down);
    EXPECT_EQ(current_name(), "docs");
    key(Qt::Key_Down);
    EXPECT_EQ(current_name(), "src");
    key(Qt::Key_Up);
    EXPECT_EQ(current_name(), "docs");

    key(Qt::Key_End);
    EXPECT_EQ(current_name(), "zeta.txt");
    key(Qt::Key_Home);
    EXPECT_EQ(current_name(), "build");
    key(Qt::Key_PageDown);
    EXPECT_EQ(current_name(), "zeta.txt");
    key(Qt::Key_PageUp);
    EXPECT_EQ(current_name(), "build");

    tree()->setCurrentIndex(index_of("/w/src"));
    key(Qt::Key_Right);
    EXPECT_TRUE(tree()->isExpanded(index_of("/w/src")));
    EXPECT_EQ(current_name(), "src");
    QApplication::processEvents();
    key(Qt::Key_Right);
    EXPECT_EQ(current_name(), "util");
    key(Qt::Key_Left);
    EXPECT_EQ(current_name(), "src");
    key(Qt::Key_Left);
    EXPECT_FALSE(tree()->isExpanded(index_of("/w/src")));
    EXPECT_EQ(current_name(), "src");

    expand(index_of("/w/src"));
    tree()->setCurrentIndex(index_of("/w/src/app.cpp"));
    key(Qt::Key_Left);
    EXPECT_EQ(current_name(), "src");
}

TEST_F(ExplorerViewTest, AnExcludedFolderIsHiddenAndShowsAgainWithoutClosingWhatIsOpen) {
    open_workspace();
    const QModelIndex src = index_of("/w/src");
    expand(src);
    expand(index_of("/w/src/util"));
    tree()->setCurrentIndex(index_of("/w/src/util/strings.cpp"));
    EXPECT_EQ(names_under({}), (Names{"build", "docs", "src", "a9.txt", "a10.txt", "zeta.txt"}));

    view_->set_excludes(
        core::FileExcludes{.patterns = {"**/build", "**/.git", "**/util"}, .problems = {}});
    EXPECT_EQ(names_under({}), (Names{"docs", "src", "a9.txt", "a10.txt", "zeta.txt"}));
    EXPECT_EQ(names_under(index_of("/w/src")), (Names{"app.cpp", "main.cpp"}));
    EXPECT_TRUE(tree()->isExpanded(index_of("/w/src")));

    view_->set_excludes(core::FileExcludes{.patterns = {"**/.git"}, .problems = {}});
    EXPECT_EQ(names_under({}), (Names{"build", "docs", "src", "a9.txt", "a10.txt", "zeta.txt"}));
    EXPECT_TRUE(tree()->isExpanded(index_of("/w/src")));
    EXPECT_EQ(names_under(index_of("/w/src")), (Names{"util", "app.cpp", "main.cpp"}));
    EXPECT_FALSE(tree()->isExpanded(index_of("/w/src/util")));
}

TEST_F(ExplorerViewTest, TheSelectionStaysWhenOtherRowsAppearAndDisappear) {
    open_workspace();
    tree()->setCurrentIndex(index_of("/w/a10.txt"));
    view_->set_excludes(core::FileExcludes{.patterns = {"**/build", "**/docs"}, .problems = {}});
    EXPECT_EQ(current_name(), "a10.txt");
    view_->set_excludes(core::FileExcludes{});
    EXPECT_EQ(current_name(), "a10.txt");
    EXPECT_EQ(names_under({}),
              (Names{".git", "build", "docs", "src", "a9.txt", "a10.txt", "zeta.txt"}));
}

TEST_F(ExplorerViewTest, ClosingTheFolderClearsTheTreeAndOpeningOneShowsItAgain) {
    open_workspace();
    expand(index_of("/w/src"));
    view_->set_workspace(std::nullopt, QString());
    EXPECT_FALSE(view_->shows_folder());
    EXPECT_EQ(view_->model()->rowCount(), 0);
    EXPECT_TRUE(view_->message()->isVisible());

    files_.set("/other", {file("only.txt")});
    view_->set_workspace(std::filesystem::path("/other"), "other");
    QApplication::processEvents();
    EXPECT_TRUE(view_->shows_folder());
    EXPECT_EQ(view_->folder_name(), "other");
    EXPECT_EQ(names_under({}), (Names{"only.txt"}));
    EXPECT_FALSE(tree()->isExpanded(QModelIndex()));
}

TEST_F(ExplorerViewTest, ADirectoryThatCannotBeReadHasNoEntriesAndTellsWhy) {
    files_.fail("/w/src", "permission denied");
    open_workspace();
    const QModelIndex src = index_of("/w/src");
    expand(src);
    EXPECT_EQ(view_->model()->rowCount(src), 0);
    EXPECT_EQ(src.data(ExplorerModel::ErrorRole).toString(), "permission denied");
    EXPECT_EQ(src.data(Qt::ToolTipRole).toString(), "permission denied");
    EXPECT_TRUE(index_of("/w/docs").data(ExplorerModel::ErrorRole).toString().isEmpty());
    EXPECT_EQ(files_.listings("/w/src"), 1);
}

TEST_F(ExplorerViewTest, TheTreeIsDrawnWithTheColorsOfTheTheme) {
    open_workspace();
    expand(index_of("/w/src"));
    tree()->setCurrentIndex(index_of("/w/src/app.cpp"));
    tree()->setFocus();
    QApplication::processEvents();
    const QImage image = view_->grab().toImage();
    ASSERT_GT(image.width(), 100);
    const QColor selection = colors_->color("list.inactiveSelectionBackground");
    const QRect row = tree()->visualRect(tree()->currentIndex());
    const QPoint inside = tree()->viewport()->mapTo(view_.get(), row.center());
    const QColor drawn = image.pixelColor(10, inside.y());
    EXPECT_TRUE(drawn == colors_->color("list.activeSelectionBackground") || drawn == selection)
        << drawn.name().toStdString();
    if (const char* directory = std::getenv("ANVIL_SCREENSHOT_DIR")) {
        image.save(QString::fromUtf8(directory) + "/explorer_view.png");
    }
}

// --- following the disk ---

class ExplorerFollowTest : public ExplorerViewTest {
protected:
    // Queues an event for each path and runs one round of the model.
    void events(std::initializer_list<const char*> paths) {
        for (const char* path : paths) {
            watches_.back()->events.emplace_back(path);
        }
        view_->model()->process_file_events();
    }
};

TEST_F(ExplorerFollowTest, TheFolderIsWatchedAndSoIsEachDirectoryThatIsRead) {
    open_workspace();
    ASSERT_EQ(watches_.size(), 1U);
    EXPECT_EQ(watches_[0]->folder, std::filesystem::path("/w"));
    EXPECT_EQ(watches_[0]->directories, (std::set<std::string>{"/w"}));
    EXPECT_TRUE(view_->model()->watching());

    expand(index_of("/w/src"));
    expand(index_of("/w/src/util"));
    EXPECT_EQ(watches_[0]->directories, (std::set<std::string>{"/w", "/w/src", "/w/src/util"}));
    EXPECT_EQ(watches_[0]->directories.count("/w/docs"), 0U) << "a directory that was not read";
}

TEST_F(ExplorerFollowTest, FilesThatAppearAndDisappearShowWithoutMovingTheRest) {
    open_workspace();
    expand(index_of("/w/src"));
    expand(index_of("/w/src/util"));
    tree()->setCurrentIndex(index_of("/w/src/util/strings.cpp"));
    ASSERT_EQ(files_.listings("/w/src"), 1);

    files_.set("/w/src", {file("main.cpp"), dir("util"), file("app.cpp"), file("new.cpp")});
    events({"/w/src/new.cpp"});
    EXPECT_EQ(names_under(index_of("/w/src")), (Names{"util", "app.cpp", "main.cpp", "new.cpp"}));
    EXPECT_EQ(files_.listings("/w/src"), 2);
    EXPECT_EQ(files_.listings("/w"), 1);
    EXPECT_EQ(files_.listings("/w/src/util"), 1);
    EXPECT_TRUE(tree()->isExpanded(index_of("/w/src")));
    EXPECT_TRUE(tree()->isExpanded(index_of("/w/src/util")));
    EXPECT_EQ(current_name(), "strings.cpp");

    files_.set("/w/src", {file("main.cpp"), dir("util")});
    events({"/w/src/new.cpp", "/w/src/app.cpp"});
    EXPECT_EQ(names_under(index_of("/w/src")), (Names{"util", "main.cpp"}));
    EXPECT_EQ(current_name(), "strings.cpp");
}

TEST_F(ExplorerFollowTest, EachDirectoryIsReadOnceNoMatterHowManyEventsNameIt) {
    open_workspace();
    expand(index_of("/w/src"));
    files_.set("/w/src", {file("a.cpp"), file("b.cpp"), file("c.cpp")});
    events({"/w/src/a.cpp", "/w/src/b.cpp", "/w/src/c.cpp", "/w/src/a.cpp"});
    EXPECT_EQ(files_.listings("/w/src"), 2);
    EXPECT_EQ(files_.listings("/w"), 1);
}

TEST_F(ExplorerFollowTest, ADirectoryThatWasNeverReadIsNotListed) {
    open_workspace();
    files_.set("/w/docs", {file("new.md")});
    events({"/w/docs/new.md", "/w/src/util/x.cpp", "/elsewhere/file.txt"});
    EXPECT_EQ(files_.listings("/w/docs"), 0);
    EXPECT_EQ(files_.listings("/w/src"), 0);
    EXPECT_EQ(files_.listings("/w/src/util"), 0);
    EXPECT_EQ(files_.listings("/w"), 1);
}

TEST_F(ExplorerFollowTest, AnEntryThatChangesBetweenFileAndFolderChangesItsRow) {
    open_workspace();
    files_.set("/w", {file("zeta.txt"), dir("src"), file("a10.txt"), file("a9.txt"), dir("docs"),
                      dir(".git"), dir("build"), dir("a9.txt-now-a-folder")});
    files_.set("/w/a9.txt-now-a-folder", {});
    events({"/w/a9.txt-now-a-folder"});
    EXPECT_TRUE(view_->model()->is_directory(index_of("/w/a9.txt-now-a-folder")));

    files_.set("/w", {file("zeta.txt"), dir("src"), file("a10.txt"), dir("a9.txt"), dir("docs"),
                      dir(".git"), dir("build")});
    events({"/w/a9.txt", "/w/a9.txt-now-a-folder"});
    EXPECT_TRUE(view_->model()->is_directory(index_of("/w/a9.txt")));
    EXPECT_EQ(names_under({}), (Names{"a9.txt", "build", "docs", "src", "a10.txt", "zeta.txt"}));
    EXPECT_FALSE(index_of("/w/a9.txt-now-a-folder").isValid());
}

TEST_F(ExplorerFollowTest, AFolderThatIsDeletedWhileExpandedDisappearsWithItsWatches) {
    open_workspace();
    expand(index_of("/w/src"));
    expand(index_of("/w/src/util"));
    ASSERT_EQ(watches_[0]->directories.size(), 3U);

    files_.set("/w", {file("zeta.txt"), dir("docs"), dir(".git"), dir("build"), file("a10.txt"),
                      file("a9.txt")});
    events({"/w/src", "/w/src/util/strings.cpp", "/w/src/util", "/w/src/main.cpp"});
    EXPECT_EQ(names_under({}), (Names{"build", "docs", "a9.txt", "a10.txt", "zeta.txt"}));
    EXPECT_FALSE(index_of("/w/src").isValid());
    EXPECT_EQ(watches_[0]->directories, (std::set<std::string>{"/w"}));
    EXPECT_EQ(files_.listings("/w/src"), 1) << "the events below a removed folder read nothing";
    EXPECT_EQ(files_.listings("/w/src/util"), 1);
}

TEST_F(ExplorerFollowTest, AFolderRenamedFromOutsideIsShownUnderItsNewNameCollapsed) {
    open_workspace();
    expand(index_of("/w/src"));
    files_.set("/w", {file("zeta.txt"), dir("source"), file("a10.txt"), file("a9.txt"), dir("docs"),
                      dir(".git"), dir("build")});
    files_.set("/w/source", {file("main.cpp")});
    events({"/w/src", "/w/source"});

    EXPECT_FALSE(index_of("/w/src").isValid());
    const QModelIndex renamed = index_of("/w/source");
    ASSERT_TRUE(renamed.isValid());
    EXPECT_FALSE(tree()->isExpanded(renamed));
    EXPECT_EQ(view_->model()->rowCount(renamed), 0);
    EXPECT_EQ(files_.listings("/w/source"), 0);
}

TEST_F(ExplorerFollowTest, TheSelectionMovesToTheNeighborOfARowThatDisappears) {
    open_workspace();
    expand(index_of("/w/src"));
    files_.set("/w/src", {dir("util"), file("app.cpp"), file("main.cpp"), file("zzz.cpp")});

    tree()->setCurrentIndex(index_of("/w/src/app.cpp"));
    files_.set("/w/src", {dir("util"), file("main.cpp"), file("zzz.cpp")});
    events({"/w/src/app.cpp"});
    EXPECT_EQ(current_name(), "main.cpp");
    EXPECT_TRUE(tree()->selectionModel()->isSelected(tree()->currentIndex()));

    tree()->setCurrentIndex(index_of("/w/src/zzz.cpp"));
    files_.set("/w/src", {dir("util"), file("main.cpp")});
    events({"/w/src/zzz.cpp"});
    EXPECT_EQ(current_name(), "main.cpp");

    tree()->setCurrentIndex(index_of("/w/src/main.cpp"));
    files_.set("/w/src", {});
    events({"/w/src/main.cpp", "/w/src/util"});
    EXPECT_EQ(current_name(), "src");
}

TEST_F(ExplorerFollowTest, ASelectionInsideAFolderThatDisappearsMovesToTheNeighborOfTheFolder) {
    open_workspace();
    expand(index_of("/w/src"));
    expand(index_of("/w/src/util"));
    tree()->setCurrentIndex(index_of("/w/src/util/strings.cpp"));
    ASSERT_EQ(current_name(), "strings.cpp");

    files_.set("/w", {file("zeta.txt"), file("a10.txt"), file("a9.txt"), dir("docs"), dir(".git"),
                      dir("build")});
    events({"/w/src"});
    EXPECT_EQ(current_name(), "a9.txt") << "the row that took the place of src";
    EXPECT_TRUE(tree()->selectionModel()->isSelected(tree()->currentIndex()));
}

TEST_F(ExplorerFollowTest, TheSelectionDoesNotMoveWhenAnotherRowDisappears) {
    open_workspace();
    tree()->setCurrentIndex(index_of("/w/zeta.txt"));
    files_.set("/w", {file("zeta.txt"), dir("src"), file("a10.txt"), dir("docs"), dir(".git"),
                      dir("build")});
    events({"/w/a9.txt"});
    EXPECT_EQ(current_name(), "zeta.txt");
}

TEST_F(ExplorerFollowTest, LostEventsAreReportedOnceAndEveryOpenDirectoryIsReadAgain) {
    open_workspace();
    expand(index_of("/w/src"));
    expand(index_of("/w/build"));
    QSignalSpy problems(view_.get(), &ExplorerView::watch_problem);

    files_.set("/w/src", {file("moved.cpp")});
    files_.set("/w/build", {});
    watches_[0]->loss = Error{ErrorCode::limit_exceeded, "the queue overflowed"};
    view_->model()->process_file_events();

    ASSERT_EQ(problems.count(), 1);
    EXPECT_TRUE(problems.at(0).at(0).toString().contains("the queue overflowed"));
    EXPECT_EQ(names_under(index_of("/w/src")), (Names{"moved.cpp"}));
    EXPECT_EQ(names_under(index_of("/w/build")), (Names{}));
    EXPECT_EQ(files_.listings("/w"), 2);
    EXPECT_EQ(files_.listings("/w/src"), 2);
    EXPECT_EQ(files_.listings("/w/build"), 2);
    EXPECT_EQ(files_.listings("/w/docs"), 0);

    watches_[0]->loss = Error{ErrorCode::limit_exceeded, "the queue overflowed"};
    view_->model()->process_file_events();
    EXPECT_EQ(problems.count(), 1) << "the same problem is not said again";
    EXPECT_EQ(files_.listings("/w/src"), 3) << "but the directories are read again";
}

TEST_F(ExplorerFollowTest, AWatchThatCannotStartIsReportedOnceAndTheTreeStillShows) {
    start_failure_ = Error{ErrorCode::limit_exceeded, "the system limit of watches is reached"};
    QSignalSpy problems(view_.get(), &ExplorerView::watch_problem);
    open_workspace();
    EXPECT_FALSE(view_->model()->watching());
    ASSERT_EQ(problems.count(), 1);
    EXPECT_TRUE(problems.at(0).at(0).toString().contains("the system limit of watches"));
    EXPECT_EQ(names_under({}), (Names{"build", "docs", "src", "a9.txt", "a10.txt", "zeta.txt"}));

    expand(index_of("/w/src"));
    view_->model()->process_file_events();
    EXPECT_EQ(problems.count(), 1);
}

TEST_F(ExplorerFollowTest, AWatchOfAPlatformThatCannotWatchIsNotReported) {
    start_failure_ = Error{ErrorCode::unsupported, "not supported"};
    QSignalSpy problems(view_.get(), &ExplorerView::watch_problem);
    open_workspace();
    EXPECT_FALSE(view_->model()->watching());
    EXPECT_EQ(problems.count(), 0);
}

TEST_F(ExplorerFollowTest, ADirectoryThatCannotBeAddedIsReportedOnceAndTheOthersAreWatched) {
    open_workspace();
    QSignalSpy problems(view_.get(), &ExplorerView::watch_problem);
    watches_[0]->add_failure = Error{ErrorCode::limit_exceeded, "too many watches"};
    expand(index_of("/w/src"));
    expand(index_of("/w/docs"));
    EXPECT_EQ(problems.count(), 2) << "one for each directory, which differ";
    EXPECT_EQ(names_under(index_of("/w/src")), (Names{"util", "app.cpp", "main.cpp"}));
    EXPECT_EQ(watches_[0]->directories, (std::set<std::string>{"/w"}));
}

TEST_F(ExplorerFollowTest, TheWatchEndsWhenTheFolderChangesOrClosesAndStartsAgainForTheNewOne) {
    open_workspace();
    ASSERT_EQ(watches_.size(), 1U);
    ASSERT_TRUE(watches_[0]->alive);

    view_->set_workspace(std::nullopt, QString());
    EXPECT_FALSE(watches_[0]->alive);
    EXPECT_FALSE(view_->model()->watching());

    files_.set("/other", {file("only.txt")});
    view_->set_workspace(std::filesystem::path("/other"), "other");
    QApplication::processEvents();
    ASSERT_EQ(watches_.size(), 2U);
    EXPECT_EQ(watches_[1]->folder, std::filesystem::path("/other"));
    EXPECT_TRUE(watches_[1]->alive);
    EXPECT_EQ(watches_[1]->directories, (std::set<std::string>{"/other"}));
    EXPECT_FALSE(watches_[0]->alive);

    view_->set_workspace(std::filesystem::path("/w"), "w");
    QApplication::processEvents();
    ASSERT_EQ(watches_.size(), 3U);
    EXPECT_FALSE(watches_[1]->alive);
}

TEST_F(ExplorerFollowTest, ABurstOfTenThousandEventsReadsEachAffectedDirectoryOnce) {
    constexpr int directories = 100;
    constexpr int files_each = 100;
    std::vector<FileTreeEntry> top;
    for (int d = 0; d < directories; ++d) {
        const std::string name = "d" + std::to_string(d);
        top.push_back(dir(name));
        files_.set("/w/" + name, {});
    }
    files_.set("/w", top);
    open_workspace();
    for (int d = 0; d < directories; ++d) {
        expand(index_of(("/w/d" + std::to_string(d)).c_str()));
    }

    for (int d = 0; d < directories; ++d) {
        const std::string name = "d" + std::to_string(d);
        std::vector<FileTreeEntry> entries;
        for (int f = 0; f < files_each; ++f) {
            entries.push_back(file("f" + std::to_string(f) + ".txt"));
            watches_[0]->events.emplace_back("/w/" + name + "/f" + std::to_string(f) + ".txt");
        }
        files_.set("/w/" + name, entries);
    }
    ASSERT_EQ(watches_[0]->events.size(), 10000U);

    QElapsedTimer round;
    round.start();
    view_->model()->process_file_events();
    const qint64 blocked = round.elapsed();

    for (int d = 0; d < directories; ++d) {
        const std::string name = "/w/d" + std::to_string(d);
        EXPECT_EQ(files_.listings(name), 2) << name;
        EXPECT_EQ(view_->model()->rowCount(index_of(name.c_str())), files_each) << name;
    }
    EXPECT_EQ(files_.listings("/w"), 1);
    if (timing_is_meaningful) {
        EXPECT_LT(blocked, 1000) << "one round of ten thousand events took " << blocked << " ms";
    }
}

TEST_F(ExplorerFollowTest, RowsThatAppearTogetherAreInsertedAsRuns) {
    open_workspace();
    expand(index_of("/w/src"));
    QSignalSpy inserted(view_->model(), &QAbstractItemModel::rowsInserted);
    QSignalSpy removed(view_->model(), &QAbstractItemModel::rowsRemoved);
    std::vector<FileTreeEntry> entries = {dir("util"), file("app.cpp"), file("main.cpp")};
    for (int i = 0; i < 500; ++i) {
        entries.push_back(file("z" + std::to_string(1000 + i) + ".txt"));
    }
    files_.set("/w/src", entries);
    events({"/w/src/z1000.txt"});
    EXPECT_EQ(inserted.count(), 1);
    EXPECT_EQ(view_->model()->rowCount(index_of("/w/src")), 503);

    files_.set("/w/src", {dir("util"), file("main.cpp")});
    events({"/w/src/z1000.txt"});
    EXPECT_EQ(removed.count(), 2) << "app.cpp, and the run of the new files";
    EXPECT_EQ(names_under(index_of("/w/src")), (Names{"util", "main.cpp"}));
}

// --- reading a directory on a worker ---

// A runner that keeps what it is asked to run, so that a test decides when a listing arrives.
struct RunnerState {
    struct Pending {
        std::stop_token stop;
        ListingRunner::Task task;
        ListingRunner::Done done;
    };
    std::vector<Pending> pending;
};

class ManualRunner : public ListingRunner {
public:
    explicit ManualRunner(std::shared_ptr<RunnerState> state) : state_(std::move(state)) {}
    void run(std::stop_token stop, Task task, Done done) override {
        state_->pending.push_back({stop, std::move(task), std::move(done)});
    }

private:
    std::shared_ptr<RunnerState> state_;
};

class ExplorerLoadTest : public ExplorerViewTest {
protected:
    std::unique_ptr<ListingRunner> make_runner() override {
        runner_ = std::make_shared<RunnerState>();
        return std::make_unique<ManualRunner>(runner_);
    }

    // Runs the oldest listing that waits, on this thread, and hands the result to the model.
    void arrive() {
        ASSERT_FALSE(runner_->pending.empty());
        RunnerState::Pending next = std::move(runner_->pending.front());
        runner_->pending.erase(runner_->pending.begin());
        next.done(next.task(next.stop));
    }

    // Opens the folder and lets its entries arrive and the view lay them out, so that an expansion
    // is seen by the view.
    void open_loaded_workspace() {
        open_workspace();
        arrive();
        QApplication::processEvents();
    }

    static std::vector<FileTreeEntry> many(int count) {
        std::vector<FileTreeEntry> entries;
        for (int i = 0; i < count; ++i) {
            entries.push_back(file("f" + std::to_string(100000 + i) + ".txt"));
        }
        return entries;
    }

    std::shared_ptr<RunnerState> runner_;
};

TEST_F(ExplorerLoadTest, TheFolderShowsLoadingUntilItsEntriesArrive) {
    open_workspace();
    ASSERT_EQ(runner_->pending.size(), 1U);
    EXPECT_EQ(files_.listings("/w"), 0) << "nothing was listed on the calling thread";
    ASSERT_EQ(view_->model()->rowCount(), 1);
    EXPECT_TRUE(view_->model()->loading());
    const QModelIndex loading = view_->model()->index(0, 0);
    EXPECT_EQ(loading.data().toString(), "Loading…");
    EXPECT_TRUE(loading.data(ExplorerModel::PlaceholderRole).toBool());
    EXPECT_FALSE(view_->model()->is_directory(loading));
    EXPECT_EQ(view_->model()->flags(loading), Qt::NoItemFlags) << "it cannot be selected";

    arrive();
    EXPECT_FALSE(view_->model()->loading());
    EXPECT_EQ(files_.listings("/w"), 1);
    EXPECT_EQ(names_under({}), (Names{"build", "docs", "src", "a9.txt", "a10.txt", "zeta.txt"}));
    EXPECT_FALSE(index_of("/w/Loading…").isValid());
}

TEST_F(ExplorerLoadTest, ADirectoryThatIsExpandedShowsLoadingAndThenItsEntries) {
    open_loaded_workspace();
    const QModelIndex src = index_of("/w/src");
    tree()->expand(src);
    ASSERT_EQ(runner_->pending.size(), 1U);
    EXPECT_EQ(files_.listings("/w/src"), 0);
    EXPECT_EQ(names_under(src), (Names{"Loading…"}));
    EXPECT_TRUE(view_->model()->is_loading(src));
    EXPECT_EQ(watches_[0]->directories.count("/w/src"), 1U) << "watched from the start of the read";

    arrive();
    EXPECT_EQ(names_under(src), (Names{"util", "app.cpp", "main.cpp"}));
    EXPECT_FALSE(view_->model()->is_loading(src));
    EXPECT_FALSE(view_->model()->loading());
    EXPECT_EQ(files_.listings("/w/src"), 1);
}

TEST_F(ExplorerLoadTest, TheEntriesComeFiveHundredAtATimeEachInAnEventOfItsOwn) {
    files_.set("/w/src", many(1300));
    open_loaded_workspace();
    const QModelIndex src = index_of("/w/src");
    QSignalSpy inserted(view_->model(), &QAbstractItemModel::rowsInserted);
    tree()->expand(src);
    ASSERT_EQ(inserted.count(), 1) << "the Loading… row";
    arrive();

    EXPECT_EQ(view_->model()->rowCount(src), 500) << "the first batch comes with the listing";
    EXPECT_TRUE(view_->model()->loading());
    EXPECT_EQ(view_->model()->index(0, 0, src).data().toString(), "f100000.txt");
    EXPECT_EQ(view_->model()->index(499, 0, src).data().toString(), "f100499.txt");

    for (int guard = 0; view_->model()->loading() && guard < 10; ++guard) {
        QApplication::processEvents();
    }
    EXPECT_EQ(view_->model()->rowCount(src), 1300);
    EXPECT_FALSE(view_->model()->loading());

    std::vector<int> sizes;
    for (int i = 1; i < inserted.count(); ++i) {
        sizes.push_back(inserted.at(i).at(2).toInt() - inserted.at(i).at(1).toInt() + 1);
    }
    EXPECT_EQ(sizes, (std::vector<int>{500, 500, 300}));
    EXPECT_EQ(view_->model()->index(1299, 0, src).data().toString(), "f101299.txt");
}

TEST_F(ExplorerLoadTest, CollapsingADirectoryThatIsLoadingStopsItAndDropsWhatArrivesLater) {
    open_loaded_workspace();
    const QModelIndex src = index_of("/w/src");
    tree()->expand(src);
    ASSERT_EQ(runner_->pending.size(), 1U);
    const std::stop_token token = runner_->pending[0].stop;
    EXPECT_FALSE(token.stop_requested());

    tree()->collapse(src);
    EXPECT_TRUE(token.stop_requested());
    EXPECT_EQ(view_->model()->rowCount(src), 0) << "not even the Loading… row is left";
    EXPECT_FALSE(view_->model()->loading());
    EXPECT_EQ(watches_[0]->directories.count("/w/src"), 0U);

    arrive();
    EXPECT_EQ(view_->model()->rowCount(src), 0) << "the listing that arrives later is dropped";
    EXPECT_FALSE(view_->model()->loading());

    tree()->expand(src);
    ASSERT_EQ(runner_->pending.size(), 1U) << "expanding again reads it again";
    arrive();
    EXPECT_EQ(names_under(src), (Names{"util", "app.cpp", "main.cpp"}));
}

TEST_F(ExplorerLoadTest, CollapsingADirectoryThatIsFillingLeavesNoRowsBehind) {
    files_.set("/w/src", many(1300));
    open_loaded_workspace();
    const QModelIndex src = index_of("/w/src");
    tree()->expand(src);
    arrive();
    ASSERT_EQ(view_->model()->rowCount(src), 500);

    tree()->collapse(src);
    EXPECT_EQ(view_->model()->rowCount(src), 0);
    EXPECT_FALSE(view_->model()->loading());
    for (int i = 0; i < 5; ++i) {
        QApplication::processEvents();
    }
    EXPECT_EQ(view_->model()->rowCount(src), 0) << "the batches that were posted insert nothing";

    tree()->expand(src);
    ASSERT_EQ(runner_->pending.size(), 1U) << "the next expansion lists again";
    arrive();
    EXPECT_EQ(view_->model()->rowCount(src), 500);
    EXPECT_EQ(files_.listings("/w/src"), 2);
}

TEST_F(ExplorerLoadTest, ClosingTheFolderStopsEveryListingAndDropsWhatArrivesLater) {
    open_loaded_workspace();
    tree()->expand(index_of("/w/src"));
    tree()->expand(index_of("/w/docs"));
    ASSERT_EQ(runner_->pending.size(), 2U);
    const std::stop_token first = runner_->pending[0].stop;
    const std::stop_token second = runner_->pending[1].stop;

    view_->set_workspace(std::nullopt, QString());
    EXPECT_TRUE(first.stop_requested());
    EXPECT_TRUE(second.stop_requested());
    EXPECT_FALSE(view_->model()->loading());
    arrive();
    arrive();
    EXPECT_EQ(view_->model()->rowCount(), 0);
    EXPECT_FALSE(view_->shows_folder());
}

TEST_F(ExplorerLoadTest, AFolderThatIsOpenedWhileTheLastOneLoadsIsNotMixedWithIt) {
    files_.set("/other", {file("only.txt")});
    open_workspace();
    ASSERT_EQ(runner_->pending.size(), 1U);
    view_->set_workspace(std::filesystem::path("/other"), "other");
    ASSERT_EQ(runner_->pending.size(), 2U);

    arrive();
    EXPECT_EQ(view_->model()->rowCount(), 1);
    EXPECT_TRUE(view_->model()->index(0, 0).data(ExplorerModel::PlaceholderRole).toBool())
        << "the listing of the folder that was closed is dropped";
    arrive();
    EXPECT_EQ(names_under({}), (Names{"only.txt"}));
}

TEST_F(ExplorerLoadTest, TheLoadingRowIsNotActivated) {
    open_loaded_workspace();
    const QModelIndex src = index_of("/w/src");
    tree()->expand(src);
    const QModelIndex loading = view_->model()->index(0, 0, src);
    ASSERT_TRUE(loading.data(ExplorerModel::PlaceholderRole).toBool());

    QSignalSpy activated(view_.get(), &ExplorerView::file_activated);
    QTest::mouseDClick(tree()->viewport(), Qt::LeftButton, Qt::NoModifier,
                       tree()->visualRect(loading).center());
    tree()->setCurrentIndex(loading);
    key(Qt::Key_Return);
    EXPECT_EQ(activated.count(), 0);
    EXPECT_FALSE(tree()->selectionModel()->isSelected(loading));
}

TEST_F(ExplorerLoadTest, ADirectoryThatCannotBeListedEndsLoadingWithItsError) {
    files_.fail("/w/src", "permission denied");
    open_loaded_workspace();
    const QModelIndex src = index_of("/w/src");
    tree()->expand(src);
    arrive();
    EXPECT_EQ(view_->model()->rowCount(src), 0);
    EXPECT_EQ(src.data(ExplorerModel::ErrorRole).toString(), "permission denied");
    EXPECT_FALSE(view_->model()->loading());
}

TEST_F(ExplorerLoadTest, ChangesThatEventsReportWhileADirectoryLoadsAreReadAfterwards) {
    open_loaded_workspace();
    const QModelIndex src = index_of("/w/src");
    tree()->expand(src);
    files_.set("/w/src", {file("main.cpp"), dir("util"), file("app.cpp"), file("new.cpp")});
    watches_[0]->events.emplace_back("/w/src/new.cpp");
    view_->model()->process_file_events();
    EXPECT_EQ(files_.listings("/w/src"), 0) << "a directory that is loading is not read again yet";

    arrive();
    EXPECT_EQ(names_under(src), (Names{"util", "app.cpp", "main.cpp", "new.cpp"}));
    EXPECT_FALSE(view_->model()->loading());
}

TEST_F(ExplorerLoadTest, NewExcludesApplyToADirectoryThatLoadsWithTheOldOnes) {
    open_loaded_workspace();
    const QModelIndex src = index_of("/w/src");
    tree()->expand(src);
    view_->set_excludes(core::FileExcludes{.patterns = {"**/util"}, .problems = {}});
    arrive();
    EXPECT_EQ(names_under(src), (Names{"app.cpp", "main.cpp"}));
}

TEST_F(ExplorerLoadTest, ADirectoryThatDisappearsWhileItLoadsIsNotTouchedWhenItsListingArrives) {
    open_loaded_workspace();
    tree()->expand(index_of("/w/src"));
    ASSERT_EQ(runner_->pending.size(), 1U);
    const std::stop_token token = runner_->pending[0].stop;

    files_.set("/w", {file("zeta.txt"), dir("docs")});
    watches_[0]->events.emplace_back("/w/src");
    view_->model()->process_file_events();
    EXPECT_FALSE(index_of("/w/src").isValid());
    EXPECT_TRUE(token.stop_requested());
    EXPECT_FALSE(view_->model()->loading());

    arrive();
    EXPECT_EQ(names_under({}), (Names{"docs", "zeta.txt"}));
}

TEST_F(ExplorerLoadTest, TheLoadingRowIsDrawnInTheColorOfDescriptions) {
    open_loaded_workspace();
    tree()->expand(index_of("/w/src"));
    QApplication::processEvents();
    const QImage image = view_->grab().toImage();
    ASSERT_GT(image.width(), 100);
    if (const char* directory = std::getenv("ANVIL_SCREENSHOT_DIR")) {
        image.save(QString::fromUtf8(directory) + "/explorer_loading.png");
    }
}

// --- following the disk, with the real watcher and file system ---

namespace {

class ExplorerDiskTest : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::weakly_canonical(std::filesystem::temp_directory_path()) /
                ("anvil-explorer-test-" + std::to_string(::getpid()));
        std::filesystem::remove_all(root_);
        std::filesystem::create_directories(root_ / "sub");
        std::filesystem::create_directories(root_ / "other");
        colors_ = std::make_unique<WorkbenchColors>(QApplication::palette());
        core::DirectoryLister real = platform_directory_lister();
        view_ = std::make_unique<ExplorerView>(
            *colors_, nullptr,
            [this, real](const std::filesystem::path& directory) {
                {
                    const std::lock_guard lock(listings_mutex_);
                    ++listings_[directory.generic_string()];
                }
                return real(directory);
            },
            platform_directory_watch());
        view_->resize(300, 400);
        view_->show();
        view_->set_workspace(root_, "project");
        QApplication::processEvents();
        ASSERT_TRUE(settle());
    }
    void TearDown() override {
        view_.reset();
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    static void touch(const std::filesystem::path& path) {
        std::ofstream(path) << "x";
    }

    // Runs the event loop until `done` holds or ten seconds pass.
    template <class Done> bool wait_until(Done done) {
        QElapsedTimer timer;
        timer.start();
        while (!done() && timer.elapsed() < 10000) {
            QTest::qWait(20);
        }
        return done();
    }

    // Until no directory is being read.
    bool settle() {
        return wait_until([&] { return !view_->model()->loading(); });
    }

    // How often the directory was listed, which the worker threads count.
    int listed(const std::filesystem::path& directory) {
        const std::lock_guard lock(listings_mutex_);
        const auto found = listings_.find(directory.generic_string());
        return found == listings_.end() ? 0 : found->second;
    }

    std::vector<std::string> names_under(const QModelIndex& parent) const {
        std::vector<std::string> names;
        for (int row = 0; row < view_->model()->rowCount(parent); ++row) {
            names.push_back(view_->model()->index(row, 0, parent).data().toString().toStdString());
        }
        return names;
    }
    QModelIndex index_of(const std::filesystem::path& path) const {
        return view_->model()->index_of(path);
    }

    std::filesystem::path root_;
    std::mutex listings_mutex_;
    std::map<std::string, int> listings_;
    std::unique_ptr<WorkbenchColors> colors_;
    std::unique_ptr<ExplorerView> view_;
};

} // namespace

TEST_F(ExplorerDiskTest, FilesCreatedDeletedAndRenamedAppearInTheDirectoriesThatAreOpen) {
    ASSERT_TRUE(view_->model()->watching());
    view_->tree()->expand(index_of(root_ / "sub"));

    touch(root_ / "top.txt");
    touch(root_ / "sub" / "inner.txt");
    EXPECT_TRUE(wait_until([&] {
        return names_under({}) == std::vector<std::string>{"other", "sub", "top.txt"} &&
               names_under(index_of(root_ / "sub")) == std::vector<std::string>{"inner.txt"};
    }));

    std::filesystem::rename(root_ / "sub" / "inner.txt", root_ / "sub" / "renamed.txt");
    EXPECT_TRUE(wait_until([&] {
        return names_under(index_of(root_ / "sub")) == std::vector<std::string>{"renamed.txt"};
    }));

    std::filesystem::remove(root_ / "top.txt");
    std::filesystem::create_directory(root_ / "made");
    EXPECT_TRUE(wait_until(
        [&] { return names_under({}) == std::vector<std::string>{"made", "other", "sub"}; }));
    EXPECT_TRUE(view_->tree()->isExpanded(index_of(root_ / "sub")));
}

TEST_F(ExplorerDiskTest, ADirectoryThatIsNotOpenIsNotListedWhenSomethingChangesInIt) {
    touch(root_ / "other" / "unseen.txt");
    touch(root_ / "marker.txt");
    ASSERT_TRUE(wait_until([&] { return index_of(root_ / "marker.txt").isValid(); }));
    EXPECT_EQ(listed(root_ / "other"), 0);
    EXPECT_EQ(listed(root_ / "sub"), 0);
    EXPECT_EQ(listed(root_), 2) << "the folder, and once for the marker";
}

TEST_F(ExplorerDiskTest, AFolderThatIsDeletedWhileOpenDisappearsAndItsSelectionMoves) {
    std::filesystem::create_directories(root_ / "sub" / "deep");
    touch(root_ / "sub" / "deep" / "f.txt");
    view_->tree()->expand(index_of(root_ / "sub"));
    ASSERT_TRUE(wait_until([&] { return index_of(root_ / "sub" / "deep").isValid(); }));
    view_->tree()->expand(index_of(root_ / "sub" / "deep"));
    ASSERT_TRUE(wait_until([&] { return index_of(root_ / "sub" / "deep" / "f.txt").isValid(); }));
    view_->tree()->setCurrentIndex(index_of(root_ / "sub" / "deep" / "f.txt"));
    ASSERT_EQ(view_->tree()->currentIndex().data().toString(), "f.txt");

    std::filesystem::remove_all(root_ / "sub");
    ASSERT_TRUE(wait_until([&] { return !index_of(root_ / "sub").isValid(); }));
    EXPECT_EQ(names_under({}), (std::vector<std::string>{"other"}));
    EXPECT_EQ(view_->tree()->currentIndex().data().toString(), "other");
}

TEST_F(ExplorerDiskTest, ABurstOfTenThousandNewFilesIsOneRoundAndDoesNotStallTheWindow) {
    view_->tree()->expand(index_of(root_ / "sub"));
    ASSERT_TRUE(settle());
    const int listed_before = listed(root_ / "sub");

    constexpr int count = 10000;
    for (int i = 0; i < count; ++i) {
        touch(root_ / "sub" / ("file" + std::to_string(i) + ".txt"));
    }

    // The longest time between two runs of a timer that is due every millisecond is the longest
    // time that the event loop was blocked.
    QElapsedTimer since_last;
    qint64 longest_block = 0;
    QTimer ticker;
    ticker.setInterval(1);
    QObject::connect(&ticker, &QTimer::timeout, [&] {
        if (since_last.isValid()) {
            longest_block = std::max(longest_block, since_last.elapsed());
        }
        since_last.restart();
    });
    ticker.start();
    since_last.start();
    const bool arrived =
        wait_until([&] { return view_->model()->rowCount(index_of(root_ / "sub")) == count; });
    // The block that brought the rows ends with the next tick.
    QTest::qWait(30);
    ticker.stop();

    ASSERT_TRUE(arrived);
    ASSERT_GE(listed_before, 1) << "the directory was open before the files came";
    EXPECT_LE(listed(root_ / "sub") - listed_before, 3)
        << "the directory is read a few times at most, not once for each event";
    if (timing_is_meaningful) {
        EXPECT_LT(longest_block, 1000)
            << "the event loop was blocked for " << longest_block << " ms";
    }
}

TEST_F(ExplorerDiskTest, OneRoundOfABurstOfTenThousandNewFilesIsFast) {
    view_->tree()->expand(index_of(root_ / "sub"));
    ASSERT_TRUE(settle());
    for (int i = 0; i < 10000; ++i) {
        touch(root_ / "sub" / ("file" + std::to_string(i) + ".txt"));
    }
    // Nothing runs the model's timer here, so the events wait until they are asked for.
    QTest::qSleep(800);
    QElapsedTimer round;
    round.start();
    view_->model()->process_file_events();
    const qint64 took = round.elapsed();
    std::cout << "one round of the burst: " << took
              << " ms, rows=" << view_->model()->rowCount(index_of(root_ / "sub")) << "\n";
    EXPECT_EQ(view_->model()->rowCount(index_of(root_ / "sub")), 10000);
}

TEST_F(ExplorerDiskTest, ExpandingADirectoryOfFiftyThousandFilesKeepsTheEventLoopResponsive) {
    constexpr int count = 50000;
    for (int i = 0; i < count; ++i) {
        touch(root_ / "sub" / ("file" + std::to_string(i) + ".txt"));
    }
    const QModelIndex sub = index_of(root_ / "sub");

    // The longest time between two runs of a timer that is due every millisecond is the longest
    // time that the event loop was blocked.
    QElapsedTimer since_last;
    qint64 longest_block = 0;
    QTimer ticker;
    ticker.setInterval(1);
    QObject::connect(&ticker, &QTimer::timeout, [&] {
        if (since_last.isValid()) {
            longest_block = std::max(longest_block, since_last.elapsed());
        }
        since_last.restart();
    });
    ticker.start();
    since_last.start();

    QSignalSpy inserted(view_->model(), &QAbstractItemModel::rowsInserted);
    view_->tree()->expand(sub);
    EXPECT_EQ(names_under(sub), (std::vector<std::string>{"Loading…"}))
        << "the row that stands for the entries until they arrive";
    EXPECT_TRUE(view_->model()->is_loading(sub));
    const bool arrived = settle();
    // The block that brought the last batch ends with the next tick.
    QTest::qWait(30);
    ticker.stop();

    ASSERT_TRUE(arrived);
    EXPECT_EQ(view_->model()->rowCount(sub), count);

    // The entries came in steps of at most `batch_size`, each in an event of its own. A step of
    // 50,000 rows takes less than 100 ms on a fast disk and machine, so the time alone does not
    // tell it from 100 steps of 500.
    int largest_step = 0;
    int steps = 0;
    for (int i = 1; i < inserted.count(); ++i) {
        largest_step =
            std::max(largest_step, inserted.at(i).at(2).toInt() - inserted.at(i).at(1).toInt() + 1);
        ++steps;
    }
    EXPECT_EQ(largest_step, ExplorerModel::batch_size);
    EXPECT_EQ(steps, count / ExplorerModel::batch_size);

    EXPECT_EQ(view_->model()->index(0, 0, sub).data().toString(), "file0.txt");
    EXPECT_EQ(view_->model()->index(count - 1, 0, sub).data().toString(), "file49999.txt");
    if (timing_is_meaningful) {
        EXPECT_LT(longest_block, 100)
            << "the event loop was blocked for " << longest_block << " ms";
    }
    std::cout << "longest block of the event loop while 50,000 entries were read and added: "
              << longest_block << " ms\n";
}

TEST_F(ExplorerDiskTest, CollapsingADirectoryThatIsBeingReadLeavesNoRowsBehind) {
    for (int i = 0; i < 5000; ++i) {
        touch(root_ / "sub" / ("file" + std::to_string(i) + ".txt"));
    }
    const QModelIndex sub = index_of(root_ / "sub");

    view_->tree()->expand(sub);
    view_->tree()->collapse(sub);
    EXPECT_EQ(view_->model()->rowCount(sub), 0);
    EXPECT_FALSE(view_->model()->loading());

    // What the worker finds is dropped when it arrives.
    QTest::qWait(500);
    EXPECT_EQ(view_->model()->rowCount(sub), 0);

    view_->tree()->expand(sub);
    ASSERT_TRUE(settle());
    EXPECT_EQ(view_->model()->rowCount(sub), 5000) << "and expanding it again shows the entries";
}
