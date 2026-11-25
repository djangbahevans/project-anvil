#include "anvil/ui/explorer_view.hpp"

#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>
#include <QTreeView>
#include <gtest/gtest.h>
#include <map>
#include <memory>

using namespace anvil;
using namespace anvil::ui;
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

class ExplorerViewTest : public ::testing::Test {
protected:
    void SetUp() override {
        files_.set("/w", {file("zeta.txt"), dir("src"), file("a10.txt"), file("a9.txt"),
                          dir("docs"), dir(".git"), dir("build")});
        files_.set("/w/src", {file("main.cpp"), dir("util"), file("app.cpp")});
        files_.set("/w/src/util", {file("strings.cpp")});
        files_.set("/w/docs", {});
        files_.set("/w/build", {file("out.o")});
        colors_ = std::make_unique<WorkbenchColors>(QApplication::palette());
        view_ = std::make_unique<ExplorerView>(*colors_, nullptr, files_.lister());
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

    tree()->expand(src);
    EXPECT_EQ(files_.listings("/w/src"), 1);
    EXPECT_EQ(names_under(src), (Names{"util", "app.cpp", "main.cpp"}));

    tree()->collapse(src);
    tree()->expand(src);
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
    key(Qt::Key_Right);
    EXPECT_EQ(current_name(), "util");
    key(Qt::Key_Left);
    EXPECT_EQ(current_name(), "src");
    key(Qt::Key_Left);
    EXPECT_FALSE(tree()->isExpanded(index_of("/w/src")));
    EXPECT_EQ(current_name(), "src");

    tree()->expand(index_of("/w/src"));
    tree()->setCurrentIndex(index_of("/w/src/app.cpp"));
    key(Qt::Key_Left);
    EXPECT_EQ(current_name(), "src");
}

TEST_F(ExplorerViewTest, AnExcludedFolderIsHiddenAndShowsAgainWithoutClosingWhatIsOpen) {
    open_workspace();
    const QModelIndex src = index_of("/w/src");
    tree()->expand(src);
    tree()->expand(index_of("/w/src/util"));
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
    tree()->expand(index_of("/w/src"));
    view_->set_workspace(std::nullopt, QString());
    EXPECT_FALSE(view_->shows_folder());
    EXPECT_EQ(view_->model()->rowCount(), 0);
    EXPECT_TRUE(view_->message()->isVisible());

    files_.set("/other", {file("only.txt")});
    view_->set_workspace(std::filesystem::path("/other"), "other");
    EXPECT_TRUE(view_->shows_folder());
    EXPECT_EQ(view_->folder_name(), "other");
    EXPECT_EQ(names_under({}), (Names{"only.txt"}));
    EXPECT_FALSE(tree()->isExpanded(QModelIndex()));
}

TEST_F(ExplorerViewTest, ADirectoryThatCannotBeReadHasNoEntriesAndTellsWhy) {
    files_.fail("/w/src", "permission denied");
    open_workspace();
    const QModelIndex src = index_of("/w/src");
    tree()->expand(src);
    EXPECT_EQ(view_->model()->rowCount(src), 0);
    EXPECT_EQ(src.data(ExplorerModel::ErrorRole).toString(), "permission denied");
    EXPECT_EQ(src.data(Qt::ToolTipRole).toString(), "permission denied");
    EXPECT_TRUE(index_of("/w/docs").data(ExplorerModel::ErrorRole).toString().isEmpty());
    EXPECT_EQ(files_.listings("/w/src"), 1);
}

TEST_F(ExplorerViewTest, TheTreeIsDrawnWithTheColorsOfTheTheme) {
    open_workspace();
    tree()->expand(index_of("/w/src"));
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
