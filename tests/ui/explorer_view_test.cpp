#include "../timing.hpp"
#include "anvil/core/log.hpp"
#include "anvil/ui/explorer_view.hpp"
#include "anvil/ui/resources.hpp"
#include "anvil/ui/scroll_bar.hpp"
#include "anvil/ui/theme_store.hpp"

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QContextMenuEvent>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
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

namespace {

// The operations on a real folder, with the trash and the question before a delete replaced.
class ExplorerOperationsTest : public ::testing::Test {
protected:
    struct Question {
        QString message;
        QString detail;
        QString button;
    };

    void SetUp() override {
        root_ = std::filesystem::weakly_canonical(std::filesystem::temp_directory_path()) /
                ("anvil-explorer-ops-" + std::to_string(::getpid()));
        std::filesystem::remove_all(root_);
        std::filesystem::create_directories(root_ / "src");
        std::filesystem::create_directories(root_ / "docs");
        write(root_ / "a.txt");
        write(root_ / "b.md");
        write(root_ / "src" / "main.cpp");
        colors_ = std::make_unique<WorkbenchColors>(QApplication::palette());
        ExplorerActions actions;
        actions.move_to_trash = [this](const std::filesystem::path& path) {
            trashed_.push_back(path);
            if (trash_fails_) {
                return false;
            }
            std::filesystem::remove_all(path);
            return true;
        };
        actions.confirm = [this](QWidget*, const QString& message, const QString& detail,
                                 const QString& button) {
            questions_.push_back({message, detail, button});
            if (while_asking_) {
                while_asking_();
            }
            return answer_;
        };
        actions.confirm_move = [this](QWidget*, const QString& message, const QString& detail) {
            move_questions_.push_back({message, detail, QString()});
            if (while_asking_to_move_) {
                while_asking_to_move_();
            }
            return move_answer_;
        };
        view_ = std::make_unique<ExplorerView>(*colors_, nullptr, platform_directory_lister(),
                                               DirectoryWatchFactory{}, immediate_listing_runner(),
                                               std::move(actions));
        view_->resize(300, 400);
        view_->show();
        view_->set_workspace(root_, "project");
        settle();
        created_ = std::make_unique<QSignalSpy>(view_.get(), &ExplorerView::created);
        renamed_ = std::make_unique<QSignalSpy>(view_.get(), &ExplorerView::renamed);
        deleted_ = std::make_unique<QSignalSpy>(view_.get(), &ExplorerView::deleted);
        failed_ = std::make_unique<QSignalSpy>(view_.get(), &ExplorerView::operation_failed);
    }
    void TearDown() override {
        view_.reset();
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    static void write(const std::filesystem::path& path) {
        std::ofstream(path) << "x";
    }
    void settle() {
        for (int i = 0; i < 50; ++i) {
            QApplication::processEvents();
        }
    }
    void select(const std::filesystem::path& path) {
        const QModelIndex row = view_->model()->index_of(path);
        ASSERT_TRUE(row.isValid()) << path;
        view_->tree()->setCurrentIndex(row);
        view_->tree()->setFocus();
    }
    std::vector<std::string> names_under(const QModelIndex& parent) const {
        std::vector<std::string> names;
        for (int row = 0; row < view_->model()->rowCount(parent); ++row) {
            names.push_back(view_->model()->index(row, 0, parent).data().toString().toStdString());
        }
        return names;
    }
    bool pending_under(const QModelIndex& parent) const {
        return view_->model()->rowCount(parent) > 0 &&
               view_->model()->index(0, 0, parent).data(ExplorerModel::PendingRole).toBool();
    }
    // Types `name` in the field and presses Enter.
    void enter(const QString& name) {
        ASSERT_TRUE(view_->is_editing());
        view_->name_editor()->setText(name);
        QTest::keyClick(view_->name_editor(), Qt::Key_Return);
        settle();
    }
    std::string current_name() const {
        return view_->tree()->currentIndex().data().toString().toStdString();
    }

    std::filesystem::path root_;
    std::unique_ptr<WorkbenchColors> colors_;
    std::unique_ptr<ExplorerView> view_;
    std::unique_ptr<QSignalSpy> created_, renamed_, deleted_, failed_;
    std::vector<std::filesystem::path> trashed_;
    std::vector<Question> questions_;
    std::function<void()> while_asking_;
    bool trash_fails_ = false;
    bool answer_ = true;
    std::vector<Question> move_questions_;
    std::function<void()> while_asking_to_move_;
    MoveAnswer move_answer_{.move = true};
};

} // namespace

TEST_F(ExplorerOperationsTest, ANewFileInTheFolderOfTheWorkspaceIsNamedInARowAtTheTop) {
    view_->tree()->clearSelection();
    view_->tree()->setCurrentIndex(QModelIndex());
    view_->new_file();
    settle();
    ASSERT_TRUE(view_->is_editing());
    EXPECT_TRUE(pending_under({}));
    EXPECT_TRUE(view_->name_editor()->hasFocus());
    EXPECT_FALSE(view_->model()->index(0, 0).data(ExplorerModel::IsDirectoryRole).toBool());

    enter("new.txt");
    EXPECT_FALSE(view_->is_editing());
    EXPECT_TRUE(std::filesystem::is_regular_file(root_ / "new.txt"));
    EXPECT_EQ(names_under({}),
              (std::vector<std::string>{"docs", "src", "a.txt", "b.md", "new.txt"}));
    EXPECT_EQ(current_name(), "new.txt");
    ASSERT_EQ(created_->count(), 1);
    EXPECT_EQ(created_->at(0).at(0).toString().toStdString(), (root_ / "new.txt").string());
    EXPECT_FALSE(created_->at(0).at(1).toBool());
}

TEST_F(ExplorerOperationsTest, ANewFolderGoesIntoTheSelectedFolderAndOpensIt) {
    select(root_ / "src");
    view_->new_folder();
    settle();
    ASSERT_TRUE(view_->is_editing());
    const QModelIndex src = view_->model()->index_of(root_ / "src");
    EXPECT_TRUE(view_->tree()->isExpanded(src));
    EXPECT_TRUE(pending_under(src));
    EXPECT_TRUE(view_->model()->index(0, 0, src).data(ExplorerModel::IsDirectoryRole).toBool());

    enter("inner");
    EXPECT_TRUE(std::filesystem::is_directory(root_ / "src" / "inner"));
    EXPECT_EQ(names_under(src), (std::vector<std::string>{"inner", "main.cpp"}));
    EXPECT_EQ(current_name(), "inner");
    ASSERT_EQ(created_->count(), 1);
    EXPECT_TRUE(created_->at(0).at(1).toBool());
}

TEST_F(ExplorerOperationsTest, ANewFileGoesIntoTheFolderOfTheSelectedFile) {
    view_->tree()->expand(view_->model()->index_of(root_ / "src"));
    settle();
    select(root_ / "src" / "main.cpp");
    view_->new_file();
    settle();
    ASSERT_TRUE(view_->is_editing());
    EXPECT_TRUE(pending_under(view_->model()->index_of(root_ / "src")));
    enter("util.cpp");
    EXPECT_TRUE(std::filesystem::exists(root_ / "src" / "util.cpp"));
}

TEST_F(ExplorerOperationsTest, ANewFileGoesIntoAFolderThatWasNeverOpenedOnceItIsRead) {
    select(root_ / "docs");
    ASSERT_FALSE(view_->tree()->isExpanded(view_->model()->index_of(root_ / "docs")));
    view_->new_file();
    settle();
    ASSERT_TRUE(view_->is_editing());
    enter("readme.md");
    EXPECT_TRUE(std::filesystem::exists(root_ / "docs" / "readme.md"));
}

TEST_F(ExplorerOperationsTest, EscapeDropsTheRowOfANewItemAndCreatesNothing) {
    view_->new_file();
    settle();
    ASSERT_TRUE(view_->is_editing());
    view_->name_editor()->setText("never.txt");
    QTest::keyClick(view_->name_editor(), Qt::Key_Escape);
    settle();
    EXPECT_FALSE(view_->is_editing());
    EXPECT_FALSE(pending_under({}));
    EXPECT_FALSE(std::filesystem::exists(root_ / "never.txt"));
    EXPECT_EQ(created_->count(), 0);
}

TEST_F(ExplorerOperationsTest, ANameThatIsEmptyHasASeparatorOrIsTakenIsRefusedWithTheReason) {
    view_->new_file();
    settle();
    ASSERT_TRUE(view_->is_editing());
    struct Case {
        QString name;
        QString problem;
    };
    const Case cases[] = {
        {"", "A file or folder name must be provided."},
        {"   ", "A file or folder name must be provided."},
        {"/abs", "A file or folder name cannot start with a slash."},
        {"x/y", "is not valid as a file or folder name"},
        {"x\\y", "is not valid as a file or folder name"},
        {"..", "is not valid as a file or folder name"},
        {"a.txt", "A file or folder \"a.txt\" already exists at this location."},
        {"src", "A file or folder \"src\" already exists at this location."},
    };
    for (const Case& test : cases) {
        enter(test.name);
        ASSERT_TRUE(view_->is_editing()) << test.name.toStdString();
        EXPECT_TRUE(view_->name_problem().contains(test.problem))
            << test.name.toStdString() << ": " << view_->name_problem().toStdString();
        EXPECT_EQ(names_under({}).size(), 5u) << "the pending row and the four entries";
    }
    EXPECT_EQ(created_->count(), 0);
    EXPECT_EQ(failed_->count(), 0);

    // The reason shows while typing, and goes with the name that is wrong.
    view_->name_editor()->setText("fine.txt");
    EXPECT_TRUE(view_->name_problem().isEmpty());
    view_->name_editor()->setText("a.txt");
    EXPECT_FALSE(view_->name_problem().isEmpty());
}

TEST_F(ExplorerOperationsTest, ARenameEditsTheNameWithoutItsExtensionSelected) {
    select(root_ / "a.txt");
    view_->rename_selected();
    settle();
    ASSERT_TRUE(view_->is_editing());
    EXPECT_EQ(view_->name_editor()->text(), "a.txt");
    EXPECT_EQ(view_->name_editor()->selectedText(), "a");

    enter("renamed.txt");
    EXPECT_FALSE(std::filesystem::exists(root_ / "a.txt"));
    EXPECT_TRUE(std::filesystem::exists(root_ / "renamed.txt"));
    EXPECT_EQ(current_name(), "renamed.txt");
    ASSERT_EQ(renamed_->count(), 1);
    EXPECT_EQ(renamed_->at(0).at(0).toString().toStdString(), (root_ / "a.txt").string());
    EXPECT_EQ(renamed_->at(0).at(1).toString().toStdString(), (root_ / "renamed.txt").string());
}

TEST_F(ExplorerOperationsTest, ARenameOfAFolderSelectsItsWholeNameAndMovesItsContent) {
    select(root_ / "src");
    view_->rename_selected();
    settle();
    ASSERT_TRUE(view_->is_editing());
    EXPECT_EQ(view_->name_editor()->selectedText(), "src");
    enter("lib");
    EXPECT_TRUE(std::filesystem::exists(root_ / "lib" / "main.cpp"));
    EXPECT_EQ(renamed_->count(), 1);
}

TEST_F(ExplorerOperationsTest, ARenameToATakenNameIsRefusedAndTheSameNameChangesNothing) {
    select(root_ / "a.txt");
    view_->rename_selected();
    settle();
    enter("b.md");
    EXPECT_TRUE(view_->is_editing());
    EXPECT_TRUE(view_->name_problem().contains("already exists"));
    EXPECT_TRUE(std::filesystem::exists(root_ / "a.txt"));

    enter("a.txt");
    EXPECT_FALSE(view_->is_editing());
    EXPECT_EQ(renamed_->count(), 0);
    EXPECT_TRUE(std::filesystem::exists(root_ / "a.txt"));
}

TEST_F(ExplorerOperationsTest, RenamingWithNothingSelectedDoesNothing) {
    view_->tree()->clearSelection();
    view_->tree()->setCurrentIndex(QModelIndex());
    view_->rename_selected();
    EXPECT_FALSE(view_->is_editing());
}

TEST_F(ExplorerOperationsTest, ADeleteAsksThenMovesTheItemToTheTrash) {
    select(root_ / "a.txt");
    view_->trash_selected();
    ASSERT_EQ(questions_.size(), 1u);
    EXPECT_EQ(questions_[0].message, "Are you sure you want to delete 'a.txt'?");
    EXPECT_EQ(questions_[0].detail, "You can restore this file from the Trash.");
    EXPECT_EQ(questions_[0].button, "Move to Trash");
    EXPECT_EQ(trashed_, (std::vector<std::filesystem::path>{root_ / "a.txt"}));
    EXPECT_FALSE(std::filesystem::exists(root_ / "a.txt"));
    EXPECT_EQ(names_under({}), (std::vector<std::string>{"docs", "src", "b.md"}));
    ASSERT_EQ(deleted_->count(), 1);
}

TEST_F(ExplorerOperationsTest, ADeleteThatIsRefusedChangesNothing) {
    answer_ = false;
    select(root_ / "a.txt");
    view_->trash_selected();
    EXPECT_EQ(questions_.size(), 1u);
    EXPECT_TRUE(trashed_.empty());
    EXPECT_TRUE(std::filesystem::exists(root_ / "a.txt"));
    EXPECT_EQ(deleted_->count(), 0);
}

TEST_F(ExplorerOperationsTest, ADeleteAsksNothingWhenTheSettingIsOff) {
    view_->set_confirm_delete(false);
    select(root_ / "a.txt");
    view_->trash_selected();
    EXPECT_TRUE(questions_.empty());
    EXPECT_FALSE(std::filesystem::exists(root_ / "a.txt"));
}

TEST_F(ExplorerOperationsTest, ShiftDeleteRemovesForGoodAndAFolderIsRemovedWithItsContent) {
    select(root_ / "src");
    view_->delete_selected();
    ASSERT_EQ(questions_.size(), 1u);
    EXPECT_EQ(questions_[0].message,
              "Are you sure you want to permanently delete 'src' and its contents?");
    EXPECT_EQ(questions_[0].detail, "This action is irreversible!");
    EXPECT_EQ(questions_[0].button, "Delete");
    EXPECT_TRUE(trashed_.empty());
    EXPECT_FALSE(std::filesystem::exists(root_ / "src"));
    EXPECT_EQ(deleted_->count(), 1);
}

TEST_F(ExplorerOperationsTest, WithoutATrashAMoveToTrashDeletesForGood) {
    view_->set_trash_enabled(false);
    select(root_ / "b.md");
    view_->trash_selected();
    ASSERT_EQ(questions_.size(), 1u);
    EXPECT_EQ(questions_[0].message, "Are you sure you want to permanently delete 'b.md'?");
    EXPECT_TRUE(trashed_.empty());
    EXPECT_FALSE(std::filesystem::exists(root_ / "b.md"));
}

TEST_F(ExplorerOperationsTest, ATrashThatFailsIsReportedAndTheItemStays) {
    trash_fails_ = true;
    select(root_ / "a.txt");
    view_->trash_selected();
    EXPECT_TRUE(std::filesystem::exists(root_ / "a.txt"));
    EXPECT_EQ(deleted_->count(), 0);
    ASSERT_EQ(failed_->count(), 1);
    EXPECT_TRUE(failed_->at(0).at(0).toString().contains("a.txt"));
    EXPECT_EQ(names_under({}).size(), 4u);
}

TEST_F(ExplorerOperationsTest, ADeleteActsOnTheItemThatWasSelectedWhenItWasAsked) {
    select(root_ / "a.txt");
    while_asking_ = [this] { select(root_ / "b.md"); };
    view_->trash_selected();
    EXPECT_FALSE(std::filesystem::exists(root_ / "a.txt"));
    EXPECT_TRUE(std::filesystem::exists(root_ / "b.md"));
    ASSERT_EQ(deleted_->count(), 1);
    EXPECT_EQ(deleted_->at(0).at(0).value<std::vector<std::filesystem::path>>(),
              (std::vector<std::filesystem::path>{root_ / "a.txt"}));
}

TEST_F(ExplorerOperationsTest, ADeleteWithNothingSelectedDoesNothing) {
    view_->tree()->clearSelection();
    view_->tree()->setCurrentIndex(QModelIndex());
    view_->trash_selected();
    view_->delete_selected();
    EXPECT_TRUE(questions_.empty());
}

TEST_F(ExplorerOperationsTest, AFailureOfTheSystemWhileCreatingIsReportedAndEndsTheEditing) {
    select(root_ / "src");
    view_->new_file();
    settle();
    ASSERT_TRUE(view_->is_editing());
    std::filesystem::remove_all(root_ / "src");
    enter("late.txt");
    EXPECT_FALSE(view_->is_editing());
    EXPECT_EQ(created_->count(), 0);
    EXPECT_EQ(failed_->count(), 1);
}

TEST_F(ExplorerOperationsTest, TheContextMenuIsAskedForWithTheRowUnderTheMouseSelected) {
    QSignalSpy asked(view_.get(), &ExplorerView::context_menu_requested);
    const QModelIndex row = view_->model()->index_of(root_ / "b.md");
    const QPoint position = view_->tree()->visualRect(row).center();
    QContextMenuEvent event(QContextMenuEvent::Mouse, position,
                            view_->tree()->viewport()->mapToGlobal(position));
    QApplication::sendEvent(view_->tree()->viewport(), &event);
    ASSERT_EQ(asked.count(), 1);
    EXPECT_EQ(current_name(), "b.md");
    EXPECT_FALSE(view_->selection_is_directory());

    const QPoint below(10, view_->tree()->viewport()->height() - 5);
    QContextMenuEvent empty(QContextMenuEvent::Mouse, below,
                            view_->tree()->viewport()->mapToGlobal(below));
    QApplication::sendEvent(view_->tree()->viewport(), &empty);
    EXPECT_EQ(asked.count(), 2);
    EXPECT_TRUE(view_->selected_paths().empty());
    EXPECT_TRUE(view_->selection_is_directory());
}

TEST_F(ExplorerOperationsTest, ThePendingRowSurvivesTheDirectoryBeingReadAgain) {
    view_->new_folder();
    settle();
    ASSERT_TRUE(view_->is_editing());
    write(root_ / "appeared.txt");
    view_->model()->reload(root_);
    settle();
    EXPECT_TRUE(pending_under({}));
    EXPECT_TRUE(view_->is_editing());
    EXPECT_EQ(names_under({}).size(), 6u);
    EXPECT_EQ(names_under({}).front(), "");
}

TEST_F(ExplorerOperationsTest, TheReasonShowsUnderTheFieldAndTheTreeIsDrawnWhileNaming) {
    select(root_ / "src");
    view_->new_file();
    settle();
    ASSERT_TRUE(view_->is_editing());
    view_->name_editor()->setText("main.cpp");
    QApplication::processEvents();
    ASSERT_FALSE(view_->name_problem().isEmpty());

    QLabel* reason = nullptr;
    for (QLabel* label : view_->tree()->viewport()->findChildren<QLabel*>()) {
        reason = label;
    }
    ASSERT_NE(reason, nullptr);
    EXPECT_TRUE(reason->isVisible());
    EXPECT_GT(reason->geometry().top(), view_->name_editor()->geometry().bottom());
    EXPECT_EQ(reason->x(), view_->name_editor()->x());
    EXPECT_LE(reason->geometry().right(), view_->tree()->viewport()->width());

    const QImage image = view_->grab().toImage();
    ASSERT_GT(image.width(), 100);
    if (const char* directory = std::getenv("ANVIL_SCREENSHOT_DIR")) {
        image.save(QString::fromUtf8(directory) + "/explorer_naming.png");
    }
}

// --- drag and drop ---

namespace {

class ExplorerDragTest : public ExplorerOperationsTest {
protected:
    void SetUp() override {
        ExplorerOperationsTest::SetUp();
        write(root_ / "docs" / "guide.md");
        view_->model()->reload(root_ / "docs");
    }

    QModelIndex row_of(const std::filesystem::path& path) const {
        return view_->model()->index_of(path);
    }
    // A point on the label of the row.
    QPoint on(const std::filesystem::path& path) const {
        const QRect rect = view_->tree()->visualRect(row_of(path));
        // On the name: a press at the left of a folder is on its twistie.
        return {rect.left() + 26, rect.center().y()};
    }
    QPoint below_the_rows() const {
        return {40, view_->tree()->viewport()->height() - 3};
    }
    void open(const std::filesystem::path& folder) {
        view_->tree()->expand(row_of(folder));
        settle();
    }

    void mouse(QEvent::Type type, const QPoint& position, Qt::MouseButton button,
               Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers = {}) {
        QWidget* viewport = view_->tree()->viewport();
        QMouseEvent event(type, position, viewport->mapToGlobal(position), button, buttons,
                          modifiers);
        QApplication::sendEvent(viewport, &event);
    }
    void press(const QPoint& position, Qt::KeyboardModifiers modifiers = {}) {
        mouse(QEvent::MouseButtonPress, position, Qt::LeftButton, Qt::LeftButton, modifiers);
    }
    void move(const QPoint& position, Qt::KeyboardModifiers modifiers = {}) {
        mouse(QEvent::MouseMove, position, Qt::NoButton, Qt::LeftButton, modifiers);
    }
    void release(const QPoint& position, Qt::KeyboardModifiers modifiers = {}) {
        mouse(QEvent::MouseButtonRelease, position, Qt::LeftButton, Qt::NoButton, modifiers);
    }
    // Presses on a row and moves far enough that it is a drag, to `to`.
    void begin_drag(const std::filesystem::path& from, const QPoint& to,
                    Qt::KeyboardModifiers modifiers = {}) {
        const QPoint start = on(from);
        press(start);
        move(start + QPoint(QApplication::startDragDistance() + 1, 0));
        move(to, modifiers);
    }
    void drag(const std::filesystem::path& from, const QPoint& to,
              Qt::KeyboardModifiers modifiers = {}) {
        begin_drag(from, to, modifiers);
        release(to, modifiers);
        settle();
    }
    void also_select(const std::filesystem::path& path) {
        view_->tree()->selectionModel()->select(row_of(path), QItemSelectionModel::Select |
                                                                  QItemSelectionModel::Rows);
    }
    std::vector<std::string> renames() const {
        std::vector<std::string> result;
        for (const QList<QVariant>& call : *renamed_) {
            result.push_back(call.at(0).toString().toStdString() + " -> " +
                             call.at(1).toString().toStdString());
        }
        return result;
    }
    std::string renaming(const char* from, const char* to) const {
        return (root_ / from).string() + " -> " + (root_ / to).string();
    }
    static std::string content(const std::filesystem::path& path) {
        std::ifstream file(path);
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }
};

} // namespace

TEST_F(ExplorerDragTest, DraggingAFileOntoAFolderMovesIt) {
    drag(root_ / "a.txt", on(root_ / "docs"));
    EXPECT_FALSE(std::filesystem::exists(root_ / "a.txt"));
    EXPECT_TRUE(std::filesystem::exists(root_ / "docs" / "a.txt"));
    EXPECT_EQ(renames(), (std::vector<std::string>{renaming("a.txt", "docs/a.txt")}));
    EXPECT_FALSE(row_of(root_ / "a.txt").isValid());
    EXPECT_TRUE(questions_.empty());
    EXPECT_FALSE(view_->drop_target().has_value());
}

TEST_F(ExplorerDragTest, WithCtrlTheItemIsCopiedAndStays) {
    drag(root_ / "a.txt", on(root_ / "docs"), Qt::ControlModifier);
    EXPECT_TRUE(std::filesystem::exists(root_ / "a.txt"));
    EXPECT_TRUE(std::filesystem::exists(root_ / "docs" / "a.txt"));
    EXPECT_EQ(renamed_->count(), 0);
    EXPECT_EQ(deleted_->count(), 0);
}

TEST_F(ExplorerDragTest, AFolderIsCopiedWithItsContent) {
    drag(root_ / "src", on(root_ / "docs"), Qt::ControlModifier);
    EXPECT_TRUE(std::filesystem::exists(root_ / "src" / "main.cpp"));
    EXPECT_TRUE(std::filesystem::exists(root_ / "docs" / "src" / "main.cpp"));
}

TEST_F(ExplorerDragTest, ADropOnAFileGoesIntoItsFolderAndOneBelowTheRowsIntoTheWorkspaceFolder) {
    open(root_ / "src");
    drag(root_ / "a.txt", on(root_ / "src" / "main.cpp"));
    EXPECT_TRUE(std::filesystem::exists(root_ / "src" / "a.txt"));

    drag(root_ / "src" / "main.cpp", below_the_rows());
    EXPECT_TRUE(std::filesystem::exists(root_ / "main.cpp"));
    EXPECT_EQ(view_->tree()->currentIndex().data().toString(), "main.cpp");
}

TEST_F(ExplorerDragTest, AFolderIsNotDroppedOntoItselfOrBelowAndNothingMovesWhereItIs) {
    std::filesystem::create_directories(root_ / "src" / "inner");
    open(root_ / "src");
    view_->model()->reload(root_ / "src");
    settle();
    for (const std::filesystem::path& target : {root_ / "src", root_ / "src" / "inner"}) {
        begin_drag(root_ / "src", on(target));
        EXPECT_FALSE(view_->drop_target().has_value()) << target;
        EXPECT_EQ(view_->tree()->viewport()->cursor().shape(), Qt::ForbiddenCursor) << target;
        release(on(target));
        settle();
    }
    // Where the item is already, and outside the view.
    drag(root_ / "a.txt", below_the_rows());
    drag(root_ / "a.txt", on(root_ / "b.md"), Qt::ControlModifier);
    drag(root_ / "a.txt", QPoint(-20, 10));
    EXPECT_TRUE(std::filesystem::exists(root_ / "a.txt"));
    EXPECT_TRUE(std::filesystem::exists(root_ / "src" / "inner"));
    EXPECT_EQ(renamed_->count(), 0);
    EXPECT_EQ(failed_->count(), 0);
    EXPECT_TRUE(questions_.empty());
}

TEST_F(ExplorerDragTest, SeveralSelectedItemsAreDraggedTogether) {
    select(root_ / "a.txt");
    also_select(root_ / "b.md");
    drag(root_ / "b.md", on(root_ / "docs"));
    EXPECT_TRUE(std::filesystem::exists(root_ / "docs" / "a.txt"));
    EXPECT_TRUE(std::filesystem::exists(root_ / "docs" / "b.md"));
    EXPECT_EQ(renamed_->count(), 2);
}

TEST_F(ExplorerDragTest, AnItemBelowASelectedFolderGoesWithTheFolder) {
    open(root_ / "src");
    select(root_ / "src");
    also_select(root_ / "src" / "main.cpp");
    drag(root_ / "src", on(root_ / "docs"));
    EXPECT_TRUE(std::filesystem::exists(root_ / "docs" / "src" / "main.cpp"));
    EXPECT_FALSE(std::filesystem::exists(root_ / "docs" / "main.cpp"));
    EXPECT_EQ(renames(), (std::vector<std::string>{renaming("src", "docs/src")}));
    EXPECT_EQ(failed_->count(), 0);
}

TEST_F(ExplorerDragTest, DraggingARowThatIsNotSelectedDragsOnlyIt) {
    select(root_ / "a.txt");
    drag(root_ / "b.md", on(root_ / "docs"));
    EXPECT_TRUE(std::filesystem::exists(root_ / "a.txt"));
    EXPECT_TRUE(std::filesystem::exists(root_ / "docs" / "b.md"));
}

TEST_F(ExplorerDragTest, ATakenNameAsksBeforeItIsReplaced) {
    std::ofstream(root_ / "docs" / "a.txt") << "old";
    answer_ = false;
    drag(root_ / "a.txt", on(root_ / "docs"));
    ASSERT_EQ(questions_.size(), 1u);
    EXPECT_EQ(questions_[0].message,
              "A file or folder with the name 'a.txt' already exists in the destination folder. "
              "Do you want to replace it?");
    EXPECT_EQ(questions_[0].detail, "This action is irreversible!");
    EXPECT_EQ(questions_[0].button, "Replace");
    EXPECT_EQ(content(root_ / "docs" / "a.txt"), "old");
    EXPECT_TRUE(std::filesystem::exists(root_ / "a.txt"));
    EXPECT_EQ(renamed_->count(), 0);
    EXPECT_EQ(deleted_->count(), 0);

    answer_ = true;
    std::vector<std::string> order;
    QObject::connect(view_.get(), &ExplorerView::deleted, view_.get(),
                     [&] { order.push_back("deleted"); });
    QObject::connect(view_.get(), &ExplorerView::renamed, view_.get(),
                     [&] { order.push_back("renamed"); });
    drag(root_ / "a.txt", on(root_ / "docs"));
    EXPECT_EQ(content(root_ / "docs" / "a.txt"), "x");
    EXPECT_FALSE(std::filesystem::exists(root_ / "a.txt"));
    // The documents of the replaced file go before those of the moved one take its place.
    EXPECT_EQ(order, (std::vector<std::string>{"deleted", "renamed"}));
    ASSERT_EQ(deleted_->count(), 1);
    EXPECT_EQ(deleted_->at(0).at(0).value<std::vector<std::filesystem::path>>(),
              (std::vector<std::filesystem::path>{root_ / "docs" / "a.txt"}));
}

TEST_F(ExplorerDragTest, ACopyOntoATakenNameAsksToo) {
    std::ofstream(root_ / "docs" / "a.txt") << "old";
    drag(root_ / "a.txt", on(root_ / "docs"), Qt::ControlModifier);
    EXPECT_EQ(questions_.size(), 1u);
    EXPECT_EQ(content(root_ / "docs" / "a.txt"), "x");
    EXPECT_TRUE(std::filesystem::exists(root_ / "a.txt"));
}

TEST_F(ExplorerDragTest, AReplaceActsOnWhatWasDraggedWhenItWasAsked) {
    std::ofstream(root_ / "docs" / "a.txt") << "old";
    while_asking_ = [this] { select(root_ / "b.md"); };
    drag(root_ / "a.txt", on(root_ / "docs"));
    EXPECT_FALSE(std::filesystem::exists(root_ / "a.txt"));
    EXPECT_TRUE(std::filesystem::exists(root_ / "b.md"));
    EXPECT_EQ(content(root_ / "docs" / "a.txt"), "x");
}

TEST_F(ExplorerDragTest, AFolderThatHoldsTheItemIsNotReplacedByIt) {
    std::filesystem::create_directories(root_ / "p" / "p");
    write(root_ / "p" / "p" / "deep.txt");
    write(root_ / "p" / "sibling.txt");
    view_->model()->reload(root_);
    settle();
    open(root_ / "p");
    drag(root_ / "p" / "p", below_the_rows());
    EXPECT_TRUE(std::filesystem::exists(root_ / "p" / "p" / "deep.txt"));
    EXPECT_TRUE(std::filesystem::exists(root_ / "p" / "sibling.txt"));
    EXPECT_TRUE(questions_.empty());
    EXPECT_EQ(failed_->count(), 1);
}

TEST_F(ExplorerDragTest, AMoveDoesNotReplaceADocumentWithUnsavedChanges) {
    std::ofstream(root_ / "docs" / "a.txt") << "old";
    view_->set_unsaved_changes_check(
        [this](const std::filesystem::path& path) { return path == root_ / "docs" / "a.txt"; });
    drag(root_ / "a.txt", on(root_ / "docs"));
    EXPECT_TRUE(questions_.empty());
    EXPECT_EQ(content(root_ / "docs" / "a.txt"), "old");
    EXPECT_TRUE(std::filesystem::exists(root_ / "a.txt"));
    ASSERT_EQ(failed_->count(), 1);
    EXPECT_TRUE(failed_->at(0).at(0).toString().contains("unsaved changes"));

    // A copy leaves documents alone, so it asks as for any other file.
    drag(root_ / "a.txt", on(root_ / "docs"), Qt::ControlModifier);
    EXPECT_EQ(questions_.size(), 1u);
    EXPECT_EQ(content(root_ / "docs" / "a.txt"), "x");
}

TEST_F(ExplorerDragTest, ADocumentChangedWhileTheQuestionIsOpenIsNotReplaced) {
    std::ofstream(root_ / "docs" / "a.txt") << "old";
    bool unsaved = false;
    view_->set_unsaved_changes_check([&](const std::filesystem::path&) { return unsaved; });
    while_asking_ = [&] { unsaved = true; };
    drag(root_ / "a.txt", on(root_ / "docs"));
    EXPECT_EQ(questions_.size(), 1u);
    EXPECT_EQ(content(root_ / "docs" / "a.txt"), "old");
    EXPECT_TRUE(std::filesystem::exists(root_ / "a.txt"));
}

TEST_F(ExplorerDragTest, AMoveThatFailsIsReportedAndTheRestAreMoved) {
    select(root_ / "a.txt");
    also_select(root_ / "b.md");
    begin_drag(root_ / "a.txt", on(root_ / "docs"));
    std::filesystem::remove(root_ / "a.txt");
    release(on(root_ / "docs"));
    settle();
    EXPECT_TRUE(std::filesystem::exists(root_ / "docs" / "b.md"));
    ASSERT_EQ(failed_->count(), 1);
    EXPECT_TRUE(failed_->at(0).at(0).toString().contains("a.txt"));
}

TEST_F(ExplorerDragTest, AShortMovementIsAClickAndNotADrag) {
    const QPoint start = on(root_ / "a.txt");
    press(start);
    move(start + QPoint(1, 0));
    EXPECT_FALSE(view_->drop_target().has_value());
    release(on(root_ / "docs"));
    settle();
    EXPECT_TRUE(std::filesystem::exists(root_ / "a.txt"));
    EXPECT_EQ(view_->selected_paths(), (std::vector<std::filesystem::path>{root_ / "a.txt"}));
}

TEST_F(ExplorerDragTest, APressOnTheTwistieDoesNotDragTheFolder) {
    const QRect src = view_->tree()->visualRect(row_of(root_ / "src"));
    press(QPoint(src.left() + 8, src.center().y()));
    move(on(root_ / "docs"));
    release(on(root_ / "docs"));
    settle();
    EXPECT_TRUE(std::filesystem::exists(root_ / "src" / "main.cpp"));
    EXPECT_FALSE(std::filesystem::exists(root_ / "docs" / "src"));
}

TEST_F(ExplorerDragTest, EscapeEndsTheDrag) {
    begin_drag(root_ / "a.txt", on(root_ / "docs"));
    ASSERT_TRUE(view_->drop_target().has_value());
    QTest::keyClick(view_->tree(), Qt::Key_Escape);
    EXPECT_FALSE(view_->drop_target().has_value());
    EXPECT_FALSE(view_->tree()->viewport()->testAttribute(Qt::WA_SetCursor));
    move(on(root_ / "docs"));
    release(on(root_ / "docs"));
    settle();
    EXPECT_TRUE(std::filesystem::exists(root_ / "a.txt"));
}

TEST_F(ExplorerDragTest, TheFolderUnderThePointerIsHighlightedAndTheCursorTellsWhatHappens) {
    // A palette color can differ from the pixel that it paints by a step of rounding.
    const auto highlighted = [&](const QColor& drawn) {
        const QColor highlight = colors_->color("list.dropBackground");
        return std::abs(drawn.red() - highlight.red()) <= 1 &&
               std::abs(drawn.green() - highlight.green()) <= 1 &&
               std::abs(drawn.blue() - highlight.blue()) <= 1;
    };
    begin_drag(root_ / "a.txt", on(root_ / "docs"));
    EXPECT_EQ(view_->drop_target(), std::optional(root_ / "docs"));
    EXPECT_EQ(view_->tree()->viewport()->cursor().shape(), Qt::DragMoveCursor);
    const auto drawn_at = [&](const QPoint& position) {
        return view_->tree()->viewport()->grab().toImage().pixelColor(position);
    };
    const QPoint right_of_docs(view_->tree()->viewport()->width() - 4, on(root_ / "docs").y());
    EXPECT_TRUE(highlighted(drawn_at(right_of_docs)));

    move(on(root_ / "docs"), Qt::ControlModifier);
    EXPECT_EQ(view_->tree()->viewport()->cursor().shape(), Qt::DragCopyCursor);

    // The workspace folder is highlighted below its rows too.
    open(root_ / "src");
    move(on(root_ / "src" / "main.cpp"));
    move(below_the_rows());
    EXPECT_EQ(view_->drop_target(), std::nullopt) << "a.txt is in the workspace folder already";
    release(below_the_rows());
    begin_drag(root_ / "src" / "main.cpp", below_the_rows());
    EXPECT_EQ(view_->drop_target(), std::optional(root_));
    EXPECT_TRUE(highlighted(drawn_at(below_the_rows())));
    if (const char* directory = std::getenv("ANVIL_SCREENSHOT_DIR")) {
        view_->grab().toImage().save(QString::fromUtf8(directory) + "/explorer_dragging.png");
    }
    QTest::keyClick(view_->tree(), Qt::Key_Escape);
    EXPECT_FALSE(highlighted(drawn_at(below_the_rows())));
}

TEST_F(ExplorerDragTest, AClosedFolderOpensWhenTheDragRestsOnIt) {
    view_->set_hover_expand_delay(20);
    const QModelIndex docs = row_of(root_ / "docs");
    ASSERT_FALSE(view_->tree()->isExpanded(docs));
    begin_drag(root_ / "a.txt", on(root_ / "docs"));
    EXPECT_FALSE(view_->tree()->isExpanded(docs));
    EXPECT_TRUE(QTest::qWaitFor([&] { return view_->tree()->isExpanded(docs); }, 2000));
    release(on(root_ / "docs"));
}

TEST_F(ExplorerDragTest, AFolderThatTheDragOnlyPassesStaysClosed) {
    view_->set_hover_expand_delay(50);
    const QModelIndex docs = row_of(root_ / "docs");
    begin_drag(root_ / "a.txt", on(root_ / "docs"));
    move(on(root_ / "b.md"));
    QTest::qWait(150);
    EXPECT_FALSE(view_->tree()->isExpanded(docs));
    release(QPoint(-20, 10));
}

TEST_F(ExplorerDragTest, ARightClickOnASelectedRowKeepsTheSelection) {
    select(root_ / "a.txt");
    also_select(root_ / "b.md");
    const QPoint position = on(root_ / "b.md");
    QContextMenuEvent event(QContextMenuEvent::Mouse, position,
                            view_->tree()->viewport()->mapToGlobal(position));
    QApplication::sendEvent(view_->tree()->viewport(), &event);
    EXPECT_EQ(view_->selected_paths(),
              (std::vector<std::filesystem::path>{root_ / "a.txt", root_ / "b.md"}));
    EXPECT_EQ(view_->focused_path(), std::optional(root_ / "b.md"));
}

TEST_F(ExplorerDragTest, ADeleteOfSeveralItemsDeletesEachOnce) {
    open(root_ / "src");
    select(root_ / "src");
    also_select(root_ / "src" / "main.cpp");
    also_select(root_ / "b.md");
    view_->trash_selected();
    ASSERT_EQ(questions_.size(), 1u);
    EXPECT_EQ(questions_[0].message,
              "Are you sure you want to delete the following 2 files/directories and their "
              "contents?");
    EXPECT_EQ(trashed_, (std::vector<std::filesystem::path>{root_ / "b.md", root_ / "src"}));
    EXPECT_EQ(failed_->count(), 0);
}

// --- typing to find ---

class ExplorerFindTest : public ExplorerViewTest {
protected:
    void SetUp() override {
        ExplorerViewTest::SetUp();
        interval_ = QApplication::keyboardInputInterval();
        QApplication::setKeyboardInputInterval(100);
        open_workspace();
    }
    void TearDown() override {
        QApplication::setKeyboardInputInterval(interval_);
    }
    // Lets the typed text reset.
    static void pause() {
        QTest::qWait(150);
    }
    void type(const char* text) {
        QTest::keyClicks(tree(), QString::fromLatin1(text));
    }
    int interval_ = 0;
};

TEST_F(ExplorerFindTest, TypingSelectsTheRowThatStartsWithTheLettersIgnoringCase) {
    type("Z");
    EXPECT_EQ(current_name(), "zeta.txt");
    EXPECT_TRUE(tree()->selectionModel()->isSelected(tree()->currentIndex()));
    EXPECT_EQ(tree()->selectionModel()->selectedRows().size(), 1);

    pause();
    type("a1");
    EXPECT_EQ(current_name(), "a10.txt");
    pause();
    type("D");
    EXPECT_EQ(current_name(), "docs");
}

TEST_F(ExplorerFindTest, TheTypedTextStartsAgainAfterAPause) {
    type("a1");
    EXPECT_EQ(current_name(), "a10.txt");
    pause();
    type("z");
    EXPECT_EQ(current_name(), "zeta.txt") << "not a search for a1z";
}

TEST_F(ExplorerFindTest, OnlyTheRowsThatShowAreSearched) {
    type("m");
    EXPECT_NE(current_name(), "main.cpp") << "src is closed";
    pause();
    expand(index_of("/w/src"));
    type("m");
    EXPECT_EQ(current_name(), "main.cpp");
    EXPECT_EQ(files_.listings("/w/src/util"), 0) << "searching reads nothing";
}

class ExplorerFindLoadingTest : public ExplorerLoadTest {};

TEST_F(ExplorerFindLoadingTest, TheLoadingRowIsNotFoundAndTheEntriesAreFoundOnceTheyArrive) {
    open_loaded_workspace();
    tree()->expand(index_of("/w/src"));
    ASSERT_EQ(view_->model()->rowCount(index_of("/w/src")), 1);
    tree()->setCurrentIndex(index_of("/w/src"));
    QTest::keyClick(tree(), 'l');
    EXPECT_EQ(current_name(), "src");

    arrive();
    QApplication::processEvents();
    QTest::qWait(QApplication::keyboardInputInterval() + 50);
    QTest::keyClick(tree(), 'm');
    EXPECT_EQ(current_name(), "main.cpp");
}

namespace {

class ExplorerExpandedTest : public ExplorerViewTest {
protected:
    // Lets every listing arrive, also those that a listing leads to.
    void settle() {
        for (int i = 0; i < 20; ++i) {
            QApplication::processEvents();
        }
        ASSERT_FALSE(view_->model()->loading());
    }
    bool is_expanded(const char* path) const {
        const QModelIndex index = index_of(path);
        return index.isValid() && tree()->isExpanded(index);
    }
};

} // namespace

TEST_F(ExplorerExpandedTest, TheExpandedFoldersAreRelativeSortedAndIncludeThoseBelowACollapsedOne) {
    EXPECT_TRUE(view_->expanded_folders().empty());
    open_workspace();
    EXPECT_TRUE(view_->expanded_folders().empty());
    expand(index_of("/w/src"));
    expand(index_of("/w/src/util"));
    expand(index_of("/w/docs"));
    EXPECT_EQ(view_->expanded_folders(), (Names{"docs", "src", "src/util"}));
    tree()->collapse(index_of("/w/src"));
    EXPECT_EQ(view_->expanded_folders(), (Names{"docs", "src/util"}));
}

TEST_F(ExplorerExpandedTest, ARestoreExpandsLevelByLevelAndListsEachDirectoryOnce) {
    view_->set_workspace(std::filesystem::path("/w"), "w");
    view_->restore_expanded({"src", "src/util"});
    // Nothing has arrived, so nothing was reached.
    EXPECT_EQ(view_->expanded_folders(), (Names{"src", "src/util"}));
    settle();
    EXPECT_TRUE(is_expanded("/w/src"));
    EXPECT_TRUE(is_expanded("/w/src/util"));
    EXPECT_FALSE(is_expanded("/w/docs"));
    EXPECT_EQ(files_.listings("/w"), 1);
    EXPECT_EQ(files_.listings("/w/src"), 1);
    EXPECT_EQ(files_.listings("/w/src/util"), 1);
    EXPECT_EQ(files_.listings("/w/docs"), 0);
    EXPECT_EQ(view_->expanded_folders(), (Names{"src", "src/util"}));
}

TEST_F(ExplorerExpandedTest, AFolderBelowACollapsedOneIsExpandedWhenThatOneIs) {
    open_workspace();
    settle();
    view_->restore_expanded({"src/util"});
    settle();
    EXPECT_EQ(files_.listings("/w/src"), 0);
    EXPECT_EQ(view_->expanded_folders(), Names{"src/util"});

    expand(index_of("/w/src"));
    settle();
    EXPECT_TRUE(is_expanded("/w/src/util"));
    EXPECT_EQ(files_.listings("/w/src/util"), 1);
    EXPECT_EQ(view_->expanded_folders(), (Names{"src", "src/util"}));
}

TEST_F(ExplorerExpandedTest, ARestoreReachesIntoFoldersThatHaveBeenReadAlready) {
    open_workspace();
    expand(index_of("/w/src"));
    view_->restore_expanded({"src/util"});
    settle();
    EXPECT_TRUE(is_expanded("/w/src/util"));
}

TEST_F(ExplorerExpandedTest, AFolderThatTheUserCollapsesStaysCollapsed) {
    view_->set_workspace(std::filesystem::path("/w"), "w");
    view_->restore_expanded({"src"});
    settle();
    tree()->collapse(index_of("/w/src"));
    expand(index_of("/w/docs"));
    settle();
    EXPECT_FALSE(is_expanded("/w/src"));
    EXPECT_EQ(view_->expanded_folders(), Names{"docs"});
}

TEST_F(ExplorerExpandedTest, GoneFoldersAreDroppedOnceTheFolderAboveIsRead) {
    QSignalSpy changed(view_.get(), &ExplorerView::expanded_folders_changed);
    view_->set_workspace(std::filesystem::path("/w"), "w");
    view_->restore_expanded({"gone", "gone/deeper", "src/nothing", "zeta.txt"});
    settle();
    // `src` has not been read, so what is saved below it stays.
    EXPECT_EQ(view_->expanded_folders(), Names{"src/nothing"});
    EXPECT_GE(changed.size(), 1);
    expand(index_of("/w/src"));
    settle();
    EXPECT_EQ(view_->expanded_folders(), Names{"src"});
}

TEST_F(ExplorerExpandedTest, SavedPathsThatAreNotBelowTheFolderAreIgnored) {
    view_->set_workspace(std::filesystem::path("/w"), "w");
    view_->restore_expanded(
        {"", ".", "..", "../x", "/w/src", "./src", "src/", "src/../docs", "docs"});
    EXPECT_EQ(view_->expanded_folders(), Names{"docs"});
}

TEST_F(ExplorerExpandedTest, AnotherFolderForgetsWhatWasToBeRestored) {
    view_->set_workspace(std::filesystem::path("/w"), "w");
    view_->restore_expanded({"src/util"});
    view_->set_workspace(std::filesystem::path("/w/docs"), "docs");
    // At once: a listing of the new folder would drop what it does not have anyway.
    EXPECT_TRUE(view_->expanded_folders().empty());
    view_->set_workspace(std::nullopt, QString());
    view_->restore_expanded({"src"});
    EXPECT_TRUE(view_->expanded_folders().empty());
}

TEST_F(ExplorerExpandedTest, ExpandingAndCollapsingAreReported) {
    open_workspace();
    QSignalSpy changed(view_.get(), &ExplorerView::expanded_folders_changed);
    expand(index_of("/w/src"));
    EXPECT_EQ(changed.size(), 1);
    tree()->collapse(index_of("/w/src"));
    EXPECT_EQ(changed.size(), 2);
}

TEST_F(ExplorerExpandedTest, AFolderThatIsThereWithoutARowStaysSaved) {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("anvil-expanded-" + std::to_string(::getpid()));
    std::filesystem::create_directories(root / ".git" / "objects");
    std::filesystem::create_directories(root / "src");
    std::ofstream(root / "was-a-folder") << "now a file";
    ExplorerView view(*colors_, nullptr, platform_directory_lister(), watch_factory(),
                      immediate_listing_runner());
    view.show();
    view.set_workspace(root, "root");
    view.restore_expanded({".git", ".git/objects", "src", "was-a-folder"});
    for (int i = 0; i < 20; ++i) {
        QApplication::processEvents();
    }
    EXPECT_FALSE(view.model()->index_of(root / ".git").isValid());
    EXPECT_EQ(view.expanded_folders(), (Names{".git", ".git/objects", "src"}));
    std::filesystem::remove_all(root);
}

TEST_F(ExplorerDragTest, AMoveAsksBeforeAnythingChanges) {
    bool untouched = false;
    while_asking_to_move_ = [&] {
        untouched = std::filesystem::exists(root_ / "a.txt") &&
                    !std::filesystem::exists(root_ / "docs" / "a.txt") && renamed_->count() == 0;
    };
    drag(root_ / "a.txt", on(root_ / "docs"));
    ASSERT_EQ(move_questions_.size(), 1U);
    EXPECT_EQ(move_questions_[0].message, "Are you sure you want to move 'a.txt' into 'docs'?");
    EXPECT_EQ(move_questions_[0].detail, "");
    EXPECT_TRUE(untouched);
    EXPECT_TRUE(std::filesystem::exists(root_ / "docs" / "a.txt"));
}

TEST_F(ExplorerDragTest, DecliningAMoveChangesNothingAndAsksNothingElse) {
    write(root_ / "docs" / "a.txt");
    move_answer_ = {};
    QSignalSpy turned_off(view_.get(), &ExplorerView::move_confirmation_turned_off);
    drag(root_ / "a.txt", on(root_ / "docs"));
    EXPECT_EQ(move_questions_.size(), 1U);
    EXPECT_TRUE(questions_.empty());
    EXPECT_TRUE(std::filesystem::exists(root_ / "a.txt"));
    EXPECT_EQ(renamed_->count(), 0);
    EXPECT_EQ(deleted_->count(), 0);
    EXPECT_EQ(failed_->count(), 0);
    EXPECT_EQ(turned_off.count(), 0);
}

TEST_F(ExplorerDragTest, SeveralItemsAreNamedAndTheWorkspaceFolderByTheNameOfTheWorkspace) {
    select(root_ / "a.txt");
    also_select(root_ / "b.md");
    drag(root_ / "a.txt", on(root_ / "docs"));
    ASSERT_EQ(move_questions_.size(), 1U);
    EXPECT_EQ(move_questions_[0].message,
              "Are you sure you want to move the following 2 files into 'docs'?");
    EXPECT_EQ(move_questions_[0].detail, "a.txt\nb.md");

    open(root_ / "src");
    drag(root_ / "src" / "main.cpp", below_the_rows());
    ASSERT_EQ(move_questions_.size(), 2U);
    EXPECT_EQ(move_questions_[1].message,
              "Are you sure you want to move 'main.cpp' into 'project'?");
}

TEST_F(ExplorerDragTest, AtMostTenNamesAreListed) {
    for (const std::size_t count : {11U, 13U}) {
        for (std::size_t i = 0; i < count; ++i) {
            write(root_ / ("file" + std::to_string(100 + i)));
        }
        view_->model()->reload(root_);
        settle();
        view_->tree()->clearSelection();
        select(root_ / "file100");
        for (std::size_t i = 1; i < count; ++i) {
            also_select(root_ / ("file" + std::to_string(100 + i)));
        }
        move_answer_ = {};
        move_questions_.clear();
        view_->tree()->scrollTo(row_of(root_ / "file100"));
        drag(root_ / "file100", on(root_ / "src"));
        ASSERT_EQ(move_questions_.size(), 1U);
        const QStringList lines = move_questions_[0].detail.split('\n');
        ASSERT_EQ(lines.size(), 11);
        EXPECT_EQ(lines.front(), "file100");
        EXPECT_EQ(lines[9], "file109");
        EXPECT_EQ(lines.back(), count == 11U ? "...1 additional file not shown"
                                             : "...3 additional files not shown");
    }
}

TEST_F(ExplorerDragTest, TheQuestionBeforeAReplaceFollowsTheOneBeforeTheMove) {
    write(root_ / "docs" / "a.txt");
    std::vector<std::string> asked;
    while_asking_to_move_ = [&] { asked.push_back("move"); };
    while_asking_ = [&] { asked.push_back("replace"); };
    drag(root_ / "a.txt", on(root_ / "docs"));
    EXPECT_EQ(asked, (std::vector<std::string>{"move", "replace"}));
}

TEST_F(ExplorerDragTest, ACopyDoesNotAskBeforeMoving) {
    drag(root_ / "a.txt", on(root_ / "docs"), Qt::ControlModifier);
    EXPECT_TRUE(move_questions_.empty());
    EXPECT_TRUE(std::filesystem::exists(root_ / "docs" / "a.txt"));
}

TEST_F(ExplorerDragTest, ADropThatIsRefusedOrHasNothingToMoveDoesNotAsk) {
    drag(root_ / "a.txt", below_the_rows()); // where it is already
    drag(root_ / "src", on(root_ / "src"));  // onto itself
    // A name that is taken by a document with unsaved changes.
    write(root_ / "docs" / "b.md");
    view_->set_unsaved_changes_check(
        [&](const std::filesystem::path& path) { return path == root_ / "docs" / "b.md"; });
    drag(root_ / "b.md", on(root_ / "docs"));
    EXPECT_EQ(failed_->count(), 1);
    // A folder that holds the item and has its name.
    std::filesystem::create_directories(root_ / "docs" / "docs");
    view_->model()->reload(root_ / "docs");
    settle();
    open(root_ / "docs");
    drag(root_ / "docs" / "docs", below_the_rows());
    EXPECT_EQ(failed_->count(), 2);
    EXPECT_TRUE(move_questions_.empty());
}

TEST_F(ExplorerDragTest, TheQuestionNamesOnlyTheItemsThatWouldMove) {
    open(root_ / "docs");
    select(root_ / "a.txt");
    also_select(root_ / "docs" / "guide.md");
    drag(root_ / "a.txt", on(root_ / "docs"));
    ASSERT_EQ(move_questions_.size(), 1U);
    EXPECT_EQ(move_questions_[0].message, "Are you sure you want to move 'a.txt' into 'docs'?");
}

TEST_F(ExplorerDragTest, WithoutTheSettingAMoveDoesNotAsk) {
    view_->set_confirm_drag_and_drop(false);
    drag(root_ / "a.txt", on(root_ / "docs"));
    EXPECT_TRUE(move_questions_.empty());
    EXPECT_TRUE(std::filesystem::exists(root_ / "docs" / "a.txt"));
}

TEST_F(ExplorerDragTest, AMoveActsOnWhatWasDraggedWhateverIsSelectedByTheTimeItIsAnswered) {
    while_asking_to_move_ = [&] {
        view_->tree()->setCurrentIndex(row_of(root_ / "b.md"));
        view_->tree()->collapseAll();
    };
    drag(root_ / "a.txt", on(root_ / "docs"));
    EXPECT_TRUE(std::filesystem::exists(root_ / "docs" / "a.txt"));
    EXPECT_TRUE(std::filesystem::exists(root_ / "b.md"));
    EXPECT_FALSE(std::filesystem::exists(root_ / "docs" / "b.md"));
}

TEST_F(ExplorerDragTest, OnlyAMoveWithTheBoxCheckedTurnsTheQuestionOff) {
    QSignalSpy turned_off(view_.get(), &ExplorerView::move_confirmation_turned_off);
    drag(root_ / "a.txt", on(root_ / "docs"));
    EXPECT_EQ(turned_off.count(), 0);
    move_answer_ = {.move = false, .do_not_ask_again = true};
    drag(root_ / "b.md", on(root_ / "docs"));
    EXPECT_EQ(turned_off.count(), 0);
    move_answer_ = {.move = true, .do_not_ask_again = true};
    drag(root_ / "b.md", on(root_ / "docs"));
    EXPECT_EQ(turned_off.count(), 1);
}

// The message box itself, answered from the event loop that it runs.
TEST(ExplorerMoveQuestionTest, TheBoxHasAMoveButtonCancelAndDoNotAskAgain) {
    const ExplorerActions actions = default_explorer_actions();
    for (const bool move : {true, false}) {
        for (const bool again : {true, false}) {
            QString message;
            QString detail;
            QStringList buttons;
            QString box_label;
            QTimer::singleShot(0, [&] {
                auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
                ASSERT_NE(box, nullptr);
                message = box->text();
                detail = box->informativeText();
                QAbstractButton* accept = nullptr;
                for (QAbstractButton* button : box->buttons()) {
                    const QString text = button->text().remove('&');
                    buttons.push_back(text);
                    if (text == "Move") {
                        accept = button;
                    }
                }
                ASSERT_NE(box->checkBox(), nullptr);
                box_label = box->checkBox()->text();
                box->checkBox()->setChecked(again);
                if (const char* directory = std::getenv("ANVIL_SCREENSHOT_DIR")) {
                    box->grab().save(QString::fromUtf8(directory) + "/explorer_move_question.png");
                }
                if (move && accept) {
                    accept->click();
                } else {
                    box->reject();
                }
            });
            const MoveAnswer answer = actions.confirm_move(nullptr, "Move it?", "a\nb");
            EXPECT_EQ(answer.move, move);
            EXPECT_EQ(answer.do_not_ask_again, again);
            EXPECT_EQ(message, "Move it?");
            EXPECT_EQ(detail, "a\nb");
            EXPECT_TRUE(buttons.contains("Move")) << buttons.join(',').toStdString();
            EXPECT_EQ(buttons.size(), 2);
            EXPECT_EQ(box_label, "Do not ask me again");
        }
    }
}

// --- file icons ---

namespace {

class ExplorerIconTest : public ExplorerViewTest {
protected:
    void SetUp() override {
        ExplorerViewTest::SetUp();
        files_.set("/w", {dir("src"), file("readme"), file("notes.zzz")});
        files_.set("/w/src", {file("main.zzz")});
        theme(core::ThemeKind::dark);
        open_workspace();
    }
    void theme(core::ThemeKind kind) {
        WorkbenchColors::Colors colors;
        for (const char* key :
             {"sideBar.background", "list.hoverBackground", "list.activeSelectionBackground",
              "list.inactiveSelectionBackground"}) {
            colors[key] = QColor(0, 0, 0);
        }
        for (const char* key :
             {"sideBar.foreground", "list.hoverForeground", "list.activeSelectionForeground",
              "list.inactiveSelectionForeground"}) {
            colors[key] = QColor(0, 255, 0);
        }
        colors_->set_theme(colors, nullptr, kind);
    }

    // The columns of the row, in view coordinates, whose pixels pass: the first and the last.
    template <class Pass> std::pair<int, int> columns(const QModelIndex& row, Pass pass) const {
        QCoreApplication::processEvents();
        const QImage image = tree()->viewport()->grab().toImage();
        const qreal ratio = image.devicePixelRatio();
        const QRect rect = tree()->visualRect(row);
        std::pair<int, int> found{-1, -1};
        for (int x = 0; x < image.width(); ++x) {
            for (int y = static_cast<int>(rect.top() * ratio);
                 y < static_cast<int>(rect.bottom() * ratio) && y < image.height(); ++y) {
                if (pass(image.pixelColor(x, y))) {
                    const int column = static_cast<int>(x / ratio);
                    if (found.first < 0) {
                        found.first = column;
                    }
                    found.second = column;
                    break;
                }
            }
        }
        return found;
    }
    static bool label(const QColor& color) {
        return color.green() > 150 && color.red() < 60 && color.blue() < 60;
    }
    static bool lit(const QColor& color) {
        return color.red() + color.green() + color.blue() > 120;
    }
    // How much more blue than red there is in what is drawn in the column before the label. An
    // average, because the edges of a glyph can have fringes of other colors.
    int blueness(const QModelIndex& row) const {
        QCoreApplication::processEvents();
        const QImage image = tree()->viewport()->grab().toImage();
        const qreal ratio = image.devicePixelRatio();
        const QRect rect = tree()->visualRect(row);
        long sum = 0;
        long count = 0;
        for (int x = static_cast<int>(rect.left() * ratio);
             x < static_cast<int>((rect.left() + 18) * ratio); ++x) {
            for (int y = static_cast<int>(rect.top() * ratio);
                 y < static_cast<int>(rect.bottom() * ratio); ++y) {
                if (const QColor color = image.pixelColor(x, y); lit(color)) {
                    sum += color.blue() - color.red();
                    ++count;
                }
            }
        }
        return count == 0 ? 0 : static_cast<int>(sum / count);
    }
};

} // namespace

TEST_F(ExplorerIconTest, AFileHasTheIconOfItsNameAndAFolderHasNone) {
    expand(index_of("/w/src"));
    EXPECT_EQ(view_->file_icon(index_of("/w/readme")), seti_file_icon("readme", "").dark);
    EXPECT_EQ(view_->file_icon(index_of("/w/notes.zzz")), seti_default_icon().dark);
    EXPECT_EQ(view_->file_icon(index_of("/w/src")), std::nullopt);
    EXPECT_EQ(view_->file_icon(QModelIndex()), std::nullopt);
}

TEST_F(ExplorerIconTest, TheLanguageIsAskedForOnlyWhereTheNameDoesNotDecide) {
    std::vector<std::filesystem::path> asked;
    view_->set_file_language([&](const std::filesystem::path& path) {
        asked.push_back(path);
        return std::string("python");
    });
    EXPECT_EQ(view_->file_icon(index_of("/w/readme")), seti_file_icon("readme", "").dark);
    asked.clear();
    EXPECT_EQ(view_->file_icon(index_of("/w/readme")), seti_file_icon("readme", "").dark);
    EXPECT_TRUE(asked.empty());
    EXPECT_EQ(view_->file_icon(index_of("/w/notes.zzz")), seti_icon("python").dark);
    EXPECT_EQ(asked, (std::vector<std::filesystem::path>{"/w/notes.zzz"}));
}

TEST_F(ExplorerIconTest, TheIconsAreThoseOfTheKindOfTheTheme) {
    const QModelIndex row = index_of("/w/readme");
    const SetiIcon wanted = seti_file_icon("readme", "");
    theme(core::ThemeKind::light);
    EXPECT_EQ(view_->file_icon(row), wanted.light);
    theme(core::ThemeKind::high_contrast_light);
    EXPECT_EQ(view_->file_icon(row), wanted.dark);
}

TEST_F(ExplorerIconTest, TheIconIsWhereAFolderHasItsTwistieAndTheNamesLineUp) {
    const QModelIndex folder = index_of("/w/src");
    const QModelIndex readme_row = index_of("/w/readme");
    // The icon of `readme` is blue, and the default one and the twistie are not.
    EXPECT_GT(blueness(readme_row), 60);
    EXPECT_LT(std::abs(blueness(index_of("/w/notes.zzz"))), 30);
    // The first level: 8 px, as in VS Code, then 16 px of the column and 6 px, then the name, for
    // a file as for a folder.
    EXPECT_GE(columns(readme_row, lit).first, 8);
    EXPECT_LT(columns(readme_row, lit).first, 16);
    EXPECT_NEAR(columns(readme_row, label).first, 30, 2);
    EXPECT_GE(columns(folder, label).first, 8);
    EXPECT_EQ(tree()->visualRect(readme_row).left(), 8);
    EXPECT_EQ(tree()->visualRect(folder).left(), tree()->visualRect(readme_row).left());
}

TEST_F(ExplorerIconTest, TheIconGrowsWithTheZoom) {
    // The columns of the icon: what is drawn before the name begins.
    const auto width = [&] {
        const QModelIndex row = index_of("/w/readme");
        const int text = columns(row, label).first;
        const QImage image = tree()->viewport()->grab().toImage();
        const qreal ratio = image.devicePixelRatio();
        const QRect rect = tree()->visualRect(row);
        int first = -1;
        int last = -1;
        for (int x = 0; x < static_cast<int>((text - 1) * ratio); ++x) {
            for (int y = static_cast<int>(rect.top() * ratio);
                 y < static_cast<int>(rect.bottom() * ratio); ++y) {
                if (lit(image.pixelColor(x, y))) {
                    first = first < 0 ? x : first;
                    last = x;
                }
            }
        }
        return last - first;
    };
    const int before = width();
    ASSERT_GT(before, 5);
    Zoom zoom;
    zoom.set_level(4);
    view_->set_zoom(&zoom);
    QApplication::processEvents();
    EXPECT_GT(width(), before * 3 / 2);
    view_->set_zoom(nullptr);
}

TEST_F(ExplorerIconTest, APressOnTheTwistieOpensAndClosesTheFolderAndNothingElse) {
    const QModelIndex folder = index_of("/w/src");
    tree()->setCurrentIndex(index_of("/w/readme"));
    const auto at = [&](int from_item) {
        const QRect rect = tree()->visualRect(folder);
        return QPoint(rect.left() + from_item, rect.center().y());
    };
    // Across the twistie, and the space before it.
    for (const int x : {-6, 1, 8, 15}) {
        SCOPED_TRACE(x);
        ASSERT_FALSE(tree()->isExpanded(folder));
        QTest::mouseClick(tree()->viewport(), Qt::LeftButton, Qt::NoModifier, at(x));
        EXPECT_TRUE(tree()->isExpanded(folder));
        QTest::mouseClick(tree()->viewport(), Qt::LeftButton, Qt::NoModifier, at(x));
        EXPECT_FALSE(tree()->isExpanded(folder));
        EXPECT_EQ(current_name(), "readme");
    }
    // Two presses of a double click are two presses.
    QTest::mousePress(tree()->viewport(), Qt::LeftButton, Qt::NoModifier, at(8));
    QTest::mouseRelease(tree()->viewport(), Qt::LeftButton, Qt::NoModifier, at(8));
    QMouseEvent twice(QEvent::MouseButtonDblClick, at(8), tree()->viewport()->mapToGlobal(at(8)),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tree()->viewport(), &twice);
    EXPECT_FALSE(tree()->isExpanded(folder));
    // The name of the folder is not the twistie.
    QTest::mouseClick(tree()->viewport(), Qt::LeftButton, Qt::NoModifier, at(30));
    EXPECT_FALSE(tree()->isExpanded(folder));
    EXPECT_EQ(current_name(), "src");
}

TEST_F(ExplorerIconTest, APressOnTheIconSelectsTheFile) {
    const QModelIndex readme_row = index_of("/w/readme");
    const QRect rect = tree()->visualRect(readme_row);
    QTest::mouseClick(tree()->viewport(), Qt::LeftButton, Qt::NoModifier,
                      QPoint(rect.left() + 8, rect.center().y()));
    EXPECT_EQ(current_name(), "readme");
}

TEST_F(ExplorerDragTest, ADragThatStartsOnTheIconOfAFileMovesIt) {
    const QRect rect = view_->tree()->visualRect(row_of(root_ / "a.txt"));
    const QPoint start(rect.left() + 8, rect.center().y());
    const QPoint to = on(root_ / "docs");
    press(start);
    move(start + QPoint(0, QApplication::startDragDistance() + 1));
    move(to);
    release(to);
    settle();
    EXPECT_TRUE(std::filesystem::exists(root_ / "docs" / "a.txt"));
}

TEST_F(ExplorerDragTest, ARenamedFileHasTheIconOfItsNewName) {
    const auto theme_icon = [&](const std::filesystem::path& path) {
        return view_->file_icon(row_of(path));
    };
    const bool light = colors_->kind() == core::ThemeKind::light;
    const SetiIcon readme = seti_file_icon("readme", "");
    ASSERT_EQ(theme_icon(root_ / "a.txt"),
              light ? seti_default_icon().light : seti_default_icon().dark);
    view_->tree()->setCurrentIndex(row_of(root_ / "a.txt"));
    view_->rename_selected();
    ASSERT_TRUE(view_->is_editing());
    view_->name_editor()->setText("readme");
    QTest::keyClick(view_->name_editor(), Qt::Key_Return);
    settle();
    EXPECT_EQ(theme_icon(root_ / "readme"), light ? readme.light : readme.dark);
}

TEST_F(ExplorerViewTest, TheTreeHasASlimScrollBarInTheColorsOfTheSideBar) {
    std::vector<core::FileTreeEntry> many;
    for (int i = 0; i < 80; ++i) {
        many.push_back(file(("file" + std::to_string(i) + ".txt").c_str()));
    }
    files_.set("/w", many);
    colors_->set_colors({{"sideBar.background", QColor("#102030")},
                         {"scrollbarSlider.background", QColor("#ff0000")}});
    open_workspace();
    auto* bar = qobject_cast<ui::ScrollBar*>(tree()->verticalScrollBar());
    ASSERT_NE(bar, nullptr);
    ASSERT_TRUE(QTest::qWaitFor([&] { return bar->isVisible(); }));
    EXPECT_EQ(bar->width(), ui::ScrollBar::list_width);
    // The bar takes its own room beside the rows, and no more.
    EXPECT_EQ(tree()->viewport()->width() + bar->width(), tree()->width());
    QEvent left(QEvent::Leave);
    QApplication::sendEvent(bar, &left);
    const auto pixel = [&](int y) { return bar->grab().toImage().pixelColor(5, y); };
    EXPECT_EQ(pixel(bar->slider_rect().center().y()), QColor("#ff0000"));
    EXPECT_EQ(pixel(bar->height() - 1), QColor("#102030"));

    colors_->set_colors({{"sideBar.background", QColor("#405060")},
                         {"scrollbarSlider.background", QColor("#00ff00")}});
    EXPECT_EQ(pixel(bar->slider_rect().center().y()), QColor("#00ff00"));
    EXPECT_EQ(pixel(bar->height() - 1), QColor("#405060"));

    if (const char* directory = std::getenv("ANVIL_SCREENSHOT_DIR")) {
        ui::ThemeStore store(*colors_, ui::themes_directory(),
                             std::make_shared<core::RecordingLogger>());
        for (const auto* theme : {"Dark 2026", "Light+"}) {
            ASSERT_TRUE(store.select(theme));
            QApplication::processEvents();
            view_->grab().save(QString::fromUtf8(directory) + "/scroll_bar_explorer_" + theme +
                               ".png");
        }
    }
}
