#include "anvil/platform/editor_defaults.hpp"
#include "anvil/platform/file.hpp"
#include "anvil/ui/main_window.hpp"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalSpy>
#include <QSplitter>
#include <QSplitterHandle>
#include <QTest>
#include <QTimer>
#include <chrono>
#include <functional>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <unistd.h>

using namespace anvil;
using anvil::ui::EditorGroupWidget;
using anvil::ui::MainWindow;
using anvil::ui::Part;
using anvil::ui::StatusBar;
using anvil::ui::StatusBarSide;
using anvil::ui::TabStrip;
using nlohmann::json;

namespace {

class MainWindowTest : public ::testing::Test {
protected:
    void SetUp() override {
        scratch_ = std::filesystem::temp_directory_path() /
                   ("anvil-window-test-" + std::to_string(::getpid()));
        std::filesystem::create_directories(scratch_);
    }
    void TearDown() override {
        window_.reset();
        std::filesystem::remove_all(scratch_);
    }

    void create_window(bool with_extension_host = false, bool confirm_unsaved_changes = false) {
        ui::MainWindowOptions options;
        options.confirm_unsaved_changes = confirm_unsaved_changes;
        options.settings_file = settings_file_;
        options.keybindings_file = keybindings_file_;
        options.themes_directory = themes_directory_;
        if (with_extension_host) {
            const std::filesystem::path vscode_dir = ANVIL_VSCODE_DIR;
            const std::filesystem::path node = ANVIL_NODE_EXECUTABLE;
            if (node.empty() ||
                !std::filesystem::exists(vscode_dir / "out" / "bootstrap-fork.js")) {
                GTEST_SKIP() << "needs Node.js and the transpiled VS Code checkout "
                                "(scripts/fetch-vscode.sh)";
            }
            options.extension_host = ui::ExtensionHostBridge::Config{
                node, vscode_dir, extension_dirs_, scratch_ / "host"};
        }
        window_ = std::make_unique<MainWindow>(std::move(options));
        window_->show();
        QApplication::processEvents();
        if (with_extension_host) {
            ASSERT_TRUE(window_->bridge().running());
            ASSERT_TRUE(wait_until(
                [&] { return window_->bridge().host()->state() != exthost::HostState::starting; }));
            ASSERT_EQ(window_->bridge().host()->state(), exthost::HostState::ready)
                << window_->bridge().host()->failure();
        }
    }

    std::filesystem::path write(const std::string& name, std::string_view bytes) {
        const auto path = scratch_ / name;
        EXPECT_TRUE(platform::write_file(path, bytes).has_value());
        return path;
    }

    void type(const QString& text) {
        for (const QChar c : text) {
            QKeyEvent press(QEvent::KeyPress, c.unicode(), Qt::NoModifier, QString(c));
            QApplication::sendEvent(window_->view(), &press);
        }
    }

    // Waits for the event loop, which services the extension host, until `done` holds.
    template <class Done> bool wait_until(Done done) {
        for (int i = 0; i < 3000 && !done(); ++i) {
            QTest::qWait(10);
        }
        return done();
    }

    // Runs an extension command through the extension host and returns its result.
    json run(const std::string& command, const json& args = json::array()) {
        std::optional<exthost::CallResult> result;
        window_->bridge().host()->commands().execute(
            command, args.dump(), [&](exthost::CallResult r) { result = std::move(r); });
        EXPECT_TRUE(wait_until([&] { return result.has_value(); })) << "no result from " << command;
        if (!result || !result->has_value()) {
            ADD_FAILURE() << command << " failed: " << (result ? result->error() : "timed out");
            return nullptr;
        }
        const std::optional<std::string>& value = **result;
        return value ? json::parse(*value) : json(nullptr);
    }

    QString label_text(const QString& fragment) const {
        const StatusBar& bar = window_->status_bar();
        for (const auto side : {StatusBarSide::left, StatusBarSide::right}) {
            for (const std::string& id : bar.item_ids(side)) {
                if (bar.item(id)->text.contains(fragment)) {
                    return bar.item(id)->text;
                }
            }
        }
        return {};
    }

    // Presses `button` in the next modal message box the window opens, after running `while_open`
    // with the box on screen; `prompt_shown_` tells whether one opened.
    void answer_next_prompt(QMessageBox::StandardButton button,
                            std::function<void()> while_open = {}) {
        prompt_shown_ = false;
        auto* timer = new QTimer(window_.get());
        timer->setInterval(5);
        QObject::connect(
            timer, &QTimer::timeout, timer,
            [this, timer, button, while_open = std::move(while_open)] {
                if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                    prompt_shown_ = true;
                    timer->deleteLater();
                    if (while_open) {
                        while_open();
                    }
                    box->button(button)->click();
                }
            });
        timer->start();
    }

    void send_mouse(QEvent::Type type, const QPoint& position, Qt::MouseButton button,
                    Qt::MouseButtons buttons) {
        TabStrip* strip = window_->tab_strip();
        QMouseEvent event(type, position, strip->mapToGlobal(position), button, buttons, {});
        QApplication::sendEvent(strip, &event);
    }

    // What the user does with the mouse on the tab strip.
    void click_tab(int index) {
        const QPoint at = window_->tab_strip()->tab_rect(index).center() - QPoint(10, 0);
        send_mouse(QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
        send_mouse(QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
    }
    void click_close_button(int index) {
        const QPoint at = window_->tab_strip()->close_button_rect(index).center();
        send_mouse(QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
        send_mouse(QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton);
    }
    void drag_tab(int from, int to) {
        window_->show();
        QApplication::processEvents();
        TabStrip* strip = window_->tab_strip();
        const QPoint start = strip->tab_rect(from).center() - QPoint(10, 0);
        const int direction = to > from ? 1 : -1;
        send_mouse(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
        send_mouse(QEvent::MouseMove, start + QPoint(direction * 12, 0), Qt::NoButton,
                   Qt::LeftButton);
        const int target = strip->tab_rect(to).center().x() + (direction > 0 ? 5 : -20);
        send_mouse(QEvent::MouseMove, QPoint(target, start.y()), Qt::NoButton, Qt::LeftButton);
        send_mouse(QEvent::MouseButtonRelease, QPoint(target, start.y()), Qt::LeftButton,
                   Qt::NoButton);
    }

    // Chooses an item of the context menu of a tab, as the user does with a right click.
    void choose_from_tab_menu(int index, const QString& text) {
        const auto menu = window_->tab_strip()->build_context_menu(index);
        for (QAction* action : menu->actions()) {
            if (action->property("anvil.title").toString() == text || action->text() == text) {
                ASSERT_TRUE(action->isEnabled()) << text.toStdString();
                action->trigger();
                return;
            }
        }
        ADD_FAILURE() << "no menu item " << text.toStdString();
    }

    static QAction* find_command_action(QMenu* menu, const std::string& command) {
        for (QAction* action : menu->actions()) {
            if (action->data().toString().toStdString() == command) {
                return action;
            }
            if (action->menu()) {
                if (QAction* found = find_command_action(action->menu(), command)) {
                    return found;
                }
            }
        }
        return nullptr;
    }

    // Opens the menus as the user does, which fills them, and finds the item that runs `command`.
    // Opening a menu again replaces its items, so the result is good until the next call.
    QAction* menu_action(const std::string& command) {
        for (QAction* top : window_->menuBar()->actions()) {
            if (QMenu* menu = top->menu()) {
                emit menu->aboutToShow();
                if (QAction* found = find_command_action(menu, command)) {
                    return found;
                }
            }
        }
        return nullptr;
    }

    std::vector<QString> tab_titles() const {
        std::vector<QString> titles;
        for (int i = 0; i < window_->tab_strip()->count(); ++i) {
            titles.push_back(window_->tab_strip()->tab(i).title);
        }
        return titles;
    }

    std::string current_text() const {
        return window_->current_document()->text().to_string();
    }

    std::filesystem::path scratch_;
    std::filesystem::path settings_file_;
    std::filesystem::path keybindings_file_;
    std::vector<std::filesystem::path> extension_dirs_ = {ANVIL_DOCUMENTS_EXTENSION};
    // Unset: the themes that ship with the editor.
    std::optional<std::filesystem::path> themes_directory_;
    std::unique_ptr<MainWindow> window_;
    bool prompt_shown_ = false;
};

} // namespace

TEST_F(MainWindowTest, StartsWithAnEmptyUntitledDocument) {
    create_window();
    EXPECT_EQ(window_->open_document_count(), 1u);
    EXPECT_EQ(window_->current_uri().scheme, "untitled");
    EXPECT_TRUE(window_->windowTitle().contains("Untitled-1"));
    EXPECT_EQ(current_text(), "");
    EXPECT_EQ(label_text("Ln "), "Ln 1, Col 1");
}

TEST_F(MainWindowTest, OpensAFileAndShowsItsText) {
    create_window();
    const auto path = write("notes.txt", "first\nsecond");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(path.string())));
    EXPECT_EQ(current_text(), "first\nsecond");
    EXPECT_TRUE(window_->windowTitle().contains("notes.txt"));
    EXPECT_FALSE(window_->isWindowModified());
    EXPECT_EQ(window_->view()->document(), window_->current_document());
}

TEST_F(MainWindowTest, TypingMarksTheWindowModifiedAndSaveWritesTheFile) {
    create_window();
    const auto path = write("a.txt", "abc");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(path.string())));
    type("X");
    EXPECT_TRUE(window_->isWindowModified());
    EXPECT_EQ(label_text("Ln "), "Ln 1, Col 2");

    ASSERT_TRUE(window_->save());
    EXPECT_FALSE(window_->isWindowModified());
    EXPECT_EQ(*platform::read_file(path), "Xabc");
}

TEST_F(MainWindowTest, UndoBackToTheSavedStateIsClean) {
    create_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "abc").string())));
    type("X");
    ASSERT_TRUE(window_->isWindowModified());
    window_->view()->undo();
    EXPECT_FALSE(window_->isWindowModified());
}

TEST_F(MainWindowTest, FilesKeepTheirLineEndingsAndByteOrderMarkWhenSaved) {
    create_window();
    const auto path = write("crlf.txt", "\xEF\xBB\xBF"
                                        "a\r\nb");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(path.string())));
    QKeyEvent end(QEvent::KeyPress, Qt::Key_End, Qt::ControlModifier);
    QApplication::sendEvent(window_->view(), &end);
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(window_->view(), &enter);
    type("c");
    ASSERT_TRUE(window_->save());
    EXPECT_EQ(*platform::read_file(path), "\xEF\xBB\xBF"
                                          "a\r\nb\r\nc");
}

TEST_F(MainWindowTest, SaveAsGivesAnUntitledDocumentAFile) {
    create_window();
    type("hello");
    const auto path = scratch_ / "new.txt";
    ASSERT_TRUE(window_->save_as(QString::fromStdString(path.string())));
    EXPECT_EQ(window_->current_uri().file_path(), path);
    EXPECT_EQ(*platform::read_file(path), "hello");
    EXPECT_TRUE(window_->windowTitle().contains("new.txt"));
    EXPECT_FALSE(window_->isWindowModified());
}

TEST_F(MainWindowTest, OpeningAnOpenFileShowsTheExistingDocument) {
    create_window();
    const auto a = write("a.txt", "A");
    const auto b = write("b.txt", "B");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(a.string())));
    type("!");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(b.string())));
    EXPECT_EQ(window_->open_document_count(), 3u); // untitled, a, b
    ASSERT_TRUE(window_->open_file(QString::fromStdString(a.string())));
    EXPECT_EQ(window_->open_document_count(), 3u);
    EXPECT_EQ(current_text(), "!A"); // the unsaved edit is still there
}

TEST_F(MainWindowTest, TheStatusBarShowsThePositionLeftOfTheExtensionHostState) {
    create_window();
    StatusBar& bar = window_->status_bar();
    const auto ids = bar.item_ids(StatusBarSide::right);
    ASSERT_EQ(ids.size(), 2U);
    EXPECT_EQ(bar.item(ids[0])->text, "Ln 1, Col 1");
    EXPECT_TRUE(bar.item(ids[1])->text.startsWith("Extension host"));
    EXPECT_TRUE(bar.item_ids(StatusBarSide::left).empty());
}

TEST_F(MainWindowTest, TheStatusBarSitsAtTheBottomEdgeOfTheWindow) {
    create_window();
    window_->show();
    QApplication::processEvents();
    const StatusBar& bar = window_->status_bar();
    EXPECT_TRUE(bar.isVisible());
    EXPECT_EQ(bar.height(), 22);
    EXPECT_EQ(bar.width(), window_->width());
    EXPECT_EQ(bar.mapTo(window_.get(), QPoint(0, 0)).y() + bar.height(), window_->height());
    EXPECT_GT(window_->view()->mapTo(window_.get(), QPoint(0, 0)).y(), 0);
    EXPECT_LE(window_->view()->mapTo(window_.get(), QPoint(0, window_->view()->height())).y(),
              bar.mapTo(window_.get(), QPoint(0, 0)).y());
}

TEST_F(MainWindowTest, ClickingAStatusBarItemRunsItsCommand) {
    create_window();
    window_->show();
    QApplication::processEvents();
    window_->status_bar().set_item("test.item", StatusBarSide::left,
                                   {.text = "toggle", .command = "workbench.action.togglePanel"});
    ASSERT_FALSE(window_->workbench()->is_visible(Part::panel));

    QTest::mouseClick(&window_->status_bar(), Qt::LeftButton, {},
                      window_->status_bar().item_rect("test.item").center());
    EXPECT_TRUE(window_->workbench()->is_visible(Part::panel));
}

TEST_F(MainWindowTest, ClickingAStatusBarItemWithAnUnknownCommandSaysSo) {
    create_window();
    window_->show();
    QApplication::processEvents();
    window_->status_bar().set_item("test.item", StatusBarSide::left,
                                   {.text = "broken", .command = "no.such.command"});
    QTest::mouseClick(&window_->status_bar(), Qt::LeftButton, {},
                      window_->status_bar().item_rect("test.item").center());
    EXPECT_TRUE(window_->status_bar().message().contains("no.such.command"));
}

TEST_F(MainWindowTest, ClickingAStatusBarItemDoesNotChangeTheCommandPaletteRecents) {
    create_window();
    window_->show();
    QApplication::processEvents();
    window_->status_bar().set_item("test.item", StatusBarSide::left,
                                   {.text = "toggle", .command = "workbench.action.togglePanel"});
    QTest::mouseClick(&window_->status_bar(), Qt::LeftButton, {},
                      window_->status_bar().item_rect("test.item").center());
    window_->commands().execute("workbench.action.showCommands");
    EXPECT_EQ(window_->command_palette()->list()->model()->index(0, 0).data().toString(),
              "Add Selection to Next Find Match");
}

TEST_F(MainWindowTest, ShowsTheCursorCountForMultipleCursors) {
    create_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "abc abc").string())));
    window_->view()->show();
    const QPoint point = window_->view()->cursor_rect(5).center();
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(point),
                      window_->view()->viewport()->mapToGlobal(QPointF(point)), Qt::LeftButton,
                      Qt::LeftButton, Qt::AltModifier);
    QApplication::sendEvent(window_->view()->viewport(), &press);
    EXPECT_TRUE(label_text("cursors").contains("2 cursors"));
}

TEST_F(MainWindowTest, RendersTheWindow) {
    create_window();
    ASSERT_TRUE(window_->open_file(
        QString::fromStdString(write("sample.txt", "hello, world\nsecond line\n").string())));
    type("A");
    QApplication::processEvents();
    const QImage image = window_->grab().toImage();
    EXPECT_GT(image.width(), 100);
    if (const char* directory = std::getenv("ANVIL_SCREENSHOT_DIR")) {
        image.save(QString::fromUtf8(directory) + "/main_window.png");
    }
}

// --- tabs ---

TEST_F(MainWindowTest, ShowsATabPerOpenDocument) {
    create_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("b.txt", "B").string())));
    EXPECT_EQ(tab_titles(), (std::vector<QString>{"Untitled-1", "a.txt", "b.txt"}));
    EXPECT_EQ(window_->tab_strip()->current_index(), 2);
}

TEST_F(MainWindowTest, SelectingATabShowsItsDocument) {
    create_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("b.txt", "B").string())));
    click_tab(1);
    EXPECT_EQ(current_text(), "A");
    EXPECT_EQ(window_->view()->document(), window_->current_document());
    EXPECT_TRUE(window_->windowTitle().contains("a.txt"));
}

TEST_F(MainWindowTest, TheTabOfAnEditedDocumentIsMarkedModified) {
    create_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    EXPECT_EQ(window_->tab_strip()->tab(1).title, "a.txt");
    EXPECT_FALSE(window_->tab_strip()->tab(1).dirty);
    type("x");
    EXPECT_TRUE(window_->tab_strip()->tab(1).dirty);
    window_->view()->undo();
    EXPECT_EQ(window_->tab_strip()->tab(1).title, "a.txt");
    EXPECT_FALSE(window_->tab_strip()->tab(1).dirty);
    type("x");
    ASSERT_TRUE(window_->save());
    EXPECT_EQ(window_->tab_strip()->tab(1).title, "a.txt");
    EXPECT_FALSE(window_->tab_strip()->tab(1).dirty);
}

TEST_F(MainWindowTest, SaveAsRenamesTheTab) {
    create_window();
    type("hello");
    ASSERT_TRUE(window_->save_as(QString::fromStdString((scratch_ / "new.txt").string())));
    EXPECT_EQ(tab_titles(), (std::vector<QString>{"new.txt"}));
}

TEST_F(MainWindowTest, AmpersandsInFileNamesAreShownLiterally) {
    create_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a&b.txt", "").string())));
    EXPECT_EQ(window_->tab_strip()->tab(1).title, "a&b.txt");
}

TEST_F(MainWindowTest, ReorderingTabsKeepsTheShownDocument) {
    create_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("b.txt", "B").string())));
    drag_tab(2, 0); // b.txt moves to the front
    EXPECT_EQ(tab_titles(), (std::vector<QString>{"b.txt", "Untitled-1", "a.txt"}));
    EXPECT_EQ(current_text(), "B");
    click_tab(2);
    EXPECT_EQ(current_text(), "A");
    click_tab(1);
    EXPECT_EQ(window_->current_document()->text().to_string(), "");
    EXPECT_EQ(window_->current_uri().scheme, "untitled");

    drag_tab(0, 2); // and back again, the other direction
    EXPECT_EQ(tab_titles(), (std::vector<QString>{"Untitled-1", "a.txt", "b.txt"}));
    click_tab(2);
    EXPECT_EQ(current_text(), "B");
    click_tab(1);
    EXPECT_EQ(current_text(), "A");
}

TEST_F(MainWindowTest, ClosingTheShownTabShowsTheTabThatWasShownBeforeIt) {
    create_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("b.txt", "B").string())));
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("c.txt", "C").string())));
    click_tab(1);
    click_tab(3);
    ASSERT_TRUE(window_->close_document(3));
    EXPECT_EQ(tab_titles(), (std::vector<QString>{"Untitled-1", "a.txt", "b.txt"}));
    EXPECT_EQ(current_text(), "A");
    EXPECT_EQ(window_->tab_strip()->current_index(), 1);
}

TEST_F(MainWindowTest, TheSettingDecidesWhetherClosingShowsTheRecentTabOrTheNeighbor) {
    settings_file_ =
        write("settings.json", R"j({"workbench.editor.focusRecentEditorAfterClose": false})j");
    create_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("b.txt", "B").string())));
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("c.txt", "C").string())));
    click_tab(1);
    click_tab(3);
    ASSERT_TRUE(window_->close_document(3));
    EXPECT_EQ(current_text(), "B");

    write("settings.json", R"j({"workbench.editor.focusRecentEditorAfterClose": true})j");
    ASSERT_TRUE(
        wait_until([&] { return window_->settings().editor().focus_recent_editor_after_close; }));
    click_tab(0);
    click_tab(2);
    ASSERT_TRUE(window_->close_document(2));
    EXPECT_EQ(window_->current_uri().scheme, "untitled");
}

TEST_F(MainWindowTest, ClosingTheShownTabShowsItsNeighbour) {
    create_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("b.txt", "B").string())));
    click_tab(1);
    ASSERT_TRUE(window_->close_document(1));
    EXPECT_EQ(tab_titles(), (std::vector<QString>{"Untitled-1", "b.txt"}));
    EXPECT_EQ(current_text(), "B");
    ASSERT_TRUE(window_->close_document(1));
    EXPECT_EQ(tab_titles(), (std::vector<QString>{"Untitled-1"}));
    EXPECT_EQ(window_->current_uri().scheme, "untitled");
}

TEST_F(MainWindowTest, ClosingAnotherTabKeepsTheShownDocument) {
    create_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("b.txt", "B").string())));
    ASSERT_TRUE(window_->close_document(0));
    EXPECT_EQ(current_text(), "B");
    EXPECT_EQ(window_->tab_strip()->current_index(), 1);
    ASSERT_TRUE(window_->close_document(0));
    EXPECT_EQ(current_text(), "B");
    EXPECT_EQ(window_->tab_strip()->current_index(), 0);
    EXPECT_EQ(window_->open_document_count(), 1u);
}

TEST_F(MainWindowTest, TheCloseButtonClosesTheTab) {
    create_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    click_close_button(1);
    EXPECT_EQ(tab_titles(), (std::vector<QString>{"Untitled-1"}));
}

TEST_F(MainWindowTest, AMiddleClickOnATabClosesIt) {
    create_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("b.txt", "B").string())));
    const QPoint at = window_->tab_strip()->tab_rect(1).center();
    send_mouse(QEvent::MouseButtonPress, at, Qt::MiddleButton, Qt::MiddleButton);
    send_mouse(QEvent::MouseButtonRelease, at, Qt::MiddleButton, Qt::NoButton);
    EXPECT_EQ(tab_titles(), (std::vector<QString>{"Untitled-1", "b.txt"}));
}

TEST_F(MainWindowTest, CloseOthersKeepsOnlyTheTabTheMenuIsFor) {
    create_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("b.txt", "B").string())));
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("c.txt", "C").string())));
    choose_from_tab_menu(2, "Close Others");
    EXPECT_EQ(tab_titles(), (std::vector<QString>{"b.txt"}));
    EXPECT_EQ(current_text(), "B");
}

TEST_F(MainWindowTest, CloseToTheRightClosesTheTabsAfterTheOneTheMenuIsFor) {
    create_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("b.txt", "B").string())));
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("c.txt", "C").string())));
    choose_from_tab_menu(1, "Close to the Right");
    EXPECT_EQ(tab_titles(), (std::vector<QString>{"Untitled-1", "a.txt"}));
    EXPECT_EQ(current_text(), "A");
}

TEST_F(MainWindowTest, CloseSavedClosesTheTabsWithoutUnsavedChanges) {
    create_window(false, true);
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    type("!");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("b.txt", "B").string())));
    answer_next_prompt(QMessageBox::Cancel);
    choose_from_tab_menu(0, "Close Saved");
    EXPECT_FALSE(prompt_shown_);
    EXPECT_EQ(tab_titles(), (std::vector<QString>{"a.txt"}));
    EXPECT_EQ(current_text(), "!A");
}

TEST_F(MainWindowTest, CloseSavedWithEverythingSavedLeavesANewUntitledDocument) {
    create_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    choose_from_tab_menu(0, "Close Saved");
    EXPECT_EQ(window_->open_document_count(), 1u);
    EXPECT_EQ(window_->current_uri().scheme, "untitled");
}

TEST_F(MainWindowTest, CancellingTheQuestionOfCloseOthersStopsClosing) {
    create_window(false, true);
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    type("!");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("b.txt", "B").string())));
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("c.txt", "C").string())));
    answer_next_prompt(QMessageBox::Cancel);
    choose_from_tab_menu(3, "Close Others");
    EXPECT_TRUE(prompt_shown_);
    EXPECT_EQ(tab_titles(), (std::vector<QString>{"a.txt", "b.txt", "c.txt"}));
}

TEST_F(MainWindowTest, DiscardingInTheQuestionOfCloseOthersClosesTheTab) {
    create_window(false, true);
    const auto path = write("a.txt", "A");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(path.string())));
    type("!");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("b.txt", "B").string())));
    answer_next_prompt(QMessageBox::Discard);
    choose_from_tab_menu(2, "Close Others");
    EXPECT_TRUE(prompt_shown_);
    EXPECT_EQ(tab_titles(), (std::vector<QString>{"b.txt"}));
    EXPECT_EQ(*platform::read_file(path), "A");
}

TEST_F(MainWindowTest, CopyPathPutsTheFilesPathOnTheClipboard) {
    create_window();
    const auto path = write("a.txt", "A");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(path.string())));
    QApplication::clipboard()->setText("something else");
    choose_from_tab_menu(1, "Copy Path");
    EXPECT_EQ(QApplication::clipboard()->text(), QString::fromStdString(path.string()));
}

TEST_F(MainWindowTest, AnUntitledTabHasNoPathToCopy) {
    create_window();
    EXPECT_FALSE(window_->tab_strip()->tab(0).has_path);
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    EXPECT_TRUE(window_->tab_strip()->tab(1).has_path);
}

TEST_F(MainWindowTest, ATabShowsThePathOfItsFileAsItsToolTip) {
    create_window();
    const auto path = write("a.txt", "A");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(path.string())));
    EXPECT_EQ(window_->tab_strip()->tab(1).tooltip, QString::fromStdString(path.string()));
}

TEST_F(MainWindowTest, ClosingTheLastDocumentLeavesANewUntitledOne) {
    create_window();
    type("scratch");
    ASSERT_TRUE(window_->close_document(0));
    EXPECT_EQ(window_->open_document_count(), 1u);
    EXPECT_EQ(tab_titles(), (std::vector<QString>{"Untitled-2"}));
    EXPECT_EQ(current_text(), "");
    EXPECT_FALSE(window_->isWindowModified());
    EXPECT_EQ(window_->view()->document(), window_->current_document());
}

TEST_F(MainWindowTest, ClosedFilesReopenFromDisk) {
    create_window();
    const auto path = write("a.txt", "A");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(path.string())));
    type("!"); // discarded: confirmation is off
    ASSERT_TRUE(window_->close_document(1));
    ASSERT_TRUE(window_->open_file(QString::fromStdString(path.string())));
    EXPECT_EQ(current_text(), "A");
    EXPECT_FALSE(window_->isWindowModified());
    EXPECT_EQ(window_->open_document_count(), 2u);
}

TEST_F(MainWindowTest, ClosingAModifiedTabAsksAndDiscardClosesIt) {
    create_window(false, true);
    const auto path = write("a.txt", "A");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(path.string())));
    type("!");
    answer_next_prompt(QMessageBox::Discard);
    EXPECT_TRUE(window_->close_document(1));
    EXPECT_TRUE(prompt_shown_);
    EXPECT_EQ(window_->open_document_count(), 1u);
    EXPECT_EQ(*platform::read_file(path), "A");
}

TEST_F(MainWindowTest, ClosingAModifiedTabAsksAndSaveWritesTheFile) {
    create_window(false, true);
    const auto path = write("a.txt", "A");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(path.string())));
    type("!");
    answer_next_prompt(QMessageBox::Save);
    EXPECT_TRUE(window_->close_document(1));
    EXPECT_TRUE(prompt_shown_);
    EXPECT_EQ(window_->open_document_count(), 1u);
    EXPECT_EQ(*platform::read_file(path), "!A");
}

TEST_F(MainWindowTest, SavingFromTheClosePromptWritesTheDocumentBeingClosed) {
    create_window(false, true);
    const auto a = write("a.txt", "A");
    const auto b = write("b.txt", "B");
    const auto c = write("c.txt", "C");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(a.string())));
    type("!");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(b.string())));

    // An extension can show another document while the question is open.
    answer_next_prompt(QMessageBox::Save,
                       [&] { window_->open_file(QString::fromStdString(c.string())); });
    EXPECT_TRUE(window_->close_document(1));
    EXPECT_TRUE(prompt_shown_);
    EXPECT_EQ(*platform::read_file(a), "!A");
    EXPECT_EQ(*platform::read_file(c), "C");
    // A new tab opens to the right of the active one, which was a.txt when c.txt opened.
    EXPECT_EQ(tab_titles(), (std::vector<QString>{"Untitled-1", "c.txt", "b.txt"}));
}

TEST_F(MainWindowTest, SavingFromTheQuitPromptWritesTheDocumentBeingAskedAbout) {
    create_window(false, true);
    const auto a = write("a.txt", "A");
    const auto c = write("c.txt", "C");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(a.string())));
    type("!");

    answer_next_prompt(QMessageBox::Save,
                       [&] { window_->open_file(QString::fromStdString(c.string())); });
    EXPECT_TRUE(window_->close());
    EXPECT_TRUE(prompt_shown_);
    EXPECT_EQ(*platform::read_file(a), "!A");
}

TEST_F(MainWindowTest, CancellingTheCloseKeepsTheTab) {
    create_window(false, true);
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    type("!");
    click_tab(0);
    answer_next_prompt(QMessageBox::Cancel);
    EXPECT_FALSE(window_->close_document(1));
    EXPECT_TRUE(prompt_shown_);
    EXPECT_EQ(window_->open_document_count(), 2u);
    EXPECT_EQ(current_text(), "!A"); // the tab being closed is shown while the question is asked
}

TEST_F(MainWindowTest, ClosingACleanTabDoesNotAsk) {
    create_window(false, true);
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    answer_next_prompt(QMessageBox::Cancel);
    EXPECT_TRUE(window_->close_document(1));
    EXPECT_FALSE(prompt_shown_);
}

TEST_F(MainWindowTest, ExtensionsSeeDocumentsClose) {
    create_window(true);
    if (!window_) {
        return;
    }
    const auto a = write("a.txt", "A");
    const auto b = write("b.txt", "B");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(a.string())));
    ASSERT_TRUE(window_->open_file(QString::fromStdString(b.string())));
    ASSERT_EQ(run("anvil.docs.openDocuments").size(), 3u);

    ASSERT_TRUE(window_->close_document(1));
    EXPECT_EQ(run("anvil.docs.closes"),
              json::parse("[\"" + core::Uri::file(a).to_string() + "\"]"));
    const json open = run("anvil.docs.openDocuments");
    EXPECT_EQ(open.size(), 2u);
    EXPECT_TRUE(std::find(open.begin(), open.end(), core::Uri::file(a).to_string()) == open.end());
}

TEST_F(MainWindowTest, ClosingTheShownDocumentMakesTheNeighbourActiveInExtensions) {
    create_window(true);
    if (!window_) {
        return;
    }
    const auto a = write("a.txt", "A");
    const auto b = write("b.txt", "B");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(a.string())));
    ASSERT_TRUE(window_->open_file(QString::fromStdString(b.string())));
    ASSERT_TRUE(window_->close_document(2));
    EXPECT_EQ(run("anvil.docs.active")["uri"], core::Uri::file(a).to_string());
}

TEST_F(MainWindowTest, AReopenedFileIsAFreshDocumentForExtensions) {
    create_window(true);
    if (!window_) {
        return;
    }
    const auto path = write("a.txt", "A");
    const std::string uri = core::Uri::file(path).to_string();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(path.string())));
    type("!");
    ASSERT_TRUE(window_->close_document(1));
    ASSERT_EQ(run("anvil.docs.closes").size(), 1u);

    ASSERT_TRUE(window_->open_file(QString::fromStdString(path.string())));
    const json active = run("anvil.docs.active");
    EXPECT_EQ(active["uri"], uri);
    EXPECT_EQ(active["text"], "A");
    EXPECT_FALSE(active["isDirty"].get<bool>());
    type("?");
    EXPECT_EQ(run("anvil.docs.active")["text"], "?A");
}

TEST_F(MainWindowTest, ExtensionsEditingAnotherDocumentUpdateItsTab) {
    create_window(true);
    if (!window_) {
        return;
    }
    const auto a = write("a.txt", "A");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(a.string())));
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("b.txt", "B").string())));
    ASSERT_TRUE(run("anvil.docs.workspaceEdit", json::array({a.string()})).get<bool>());
    ASSERT_TRUE(wait_until([&] { return window_->tab_strip()->tab(1).dirty; }));
    EXPECT_EQ(window_->tab_strip()->current_index(), 2);
}

TEST_F(MainWindowTest, TypingCostDoesNotGrowWithTheNumberOfTabs) {
    create_window();
    constexpr int keystrokes = 200;
    const auto time_typing = [&] {
        using clock = std::chrono::steady_clock;
        auto best = clock::duration::max();
        for (int round = 0; round < 5; ++round) {
            const auto start = clock::now();
            for (int i = 0; i < keystrokes; ++i) {
                type("x");
            }
            best = std::min(best, clock::now() - start);
        }
        return best;
    };
    const auto one_tab = time_typing();

    // Four groups, with the 500 tabs spread over them.
    for (int i = 0; i < 3; ++i) {
        window_->commands().execute("workbench.action.splitEditor");
    }
    for (int i = 0; i < 499; ++i) {
        ASSERT_TRUE(window_->open_file(
            QString::fromStdString(write("tab" + std::to_string(i) + ".txt", "x").string())));
        window_->commands().execute("workbench.action.focusNextGroup");
    }
    ASSERT_EQ(window_->editor_area()->groups().size(), 4U);
    ASSERT_EQ(window_->open_document_count(), 500U);
    int tabs = 0;
    for (EditorGroupWidget* group : window_->editor_area()->groups()) {
        tabs += group->tabs()->count();
        EXPECT_GE(group->tabs()->count(), 100);
    }
    ASSERT_EQ(tabs, 500);
    const auto many_tabs = time_typing();

    // Typing into the last tab costs about as much as typing into the only one. Laying out or
    // repainting the strip for every tab on every keystroke would cost many times as much.
    EXPECT_LT(many_tabs, one_tab * 5 + std::chrono::milliseconds(5))
        << "one tab: " << std::chrono::duration_cast<std::chrono::microseconds>(one_tab).count()
        << " us, 500 tabs in 4 groups: "
        << std::chrono::duration_cast<std::chrono::microseconds>(many_tabs).count() << " us";
}

// --- editor groups ---

namespace {

std::vector<QString> titles_of(EditorGroupWidget* group) {
    std::vector<QString> titles;
    for (int i = 0; i < group->tabs()->count(); ++i) {
        titles.push_back(group->tabs()->tab(i).title);
    }
    return titles;
}

} // namespace

TEST_F(MainWindowTest, SplitEditorAddsAnEmptyGroupToTheRightThatHasTheFocus) {
    create_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    const auto first = window_->editor_layout().focused();
    window_->commands().execute("workbench.action.splitEditor");
    QApplication::processEvents();

    const auto ids = window_->editor_layout().group_ids();
    ASSERT_EQ(ids.size(), 2U);
    EXPECT_EQ(ids[0], first);
    EXPECT_EQ(window_->editor_layout().focused(), ids[1]);
    ASSERT_NE(window_->editor_area()->root_splitter(), nullptr);
    EXPECT_EQ(window_->editor_area()->root_splitter()->orientation(), Qt::Horizontal);
    EXPECT_EQ(window_->editor_area()->root_splitter()->count(), 2);

    EditorGroupWidget* left = window_->editor_area()->group(ids[0]);
    EditorGroupWidget* right = window_->editor_area()->group(ids[1]);
    EXPECT_EQ(titles_of(left), (std::vector<QString>{"Untitled-1", "a.txt"}));
    EXPECT_TRUE(titles_of(right).empty());
    EXPECT_TRUE(right->is_empty());
    EXPECT_FALSE(left->is_empty());
    EXPECT_TRUE(left->isVisible());
    EXPECT_TRUE(right->isVisible());
    EXPECT_LT(left->geometry().right(), right->geometry().left() + 2);
    EXPECT_EQ(right->view()->document(), nullptr);
}

TEST_F(MainWindowTest, SplitEditorDownStacksTheGroups) {
    create_window();
    window_->commands().execute("workbench.action.splitEditorDown");
    QApplication::processEvents();
    ASSERT_NE(window_->editor_area()->root_splitter(), nullptr);
    EXPECT_EQ(window_->editor_area()->root_splitter()->orientation(), Qt::Vertical);
    const auto groups = window_->editor_area()->groups();
    ASSERT_EQ(groups.size(), 2U);
    EXPECT_LT(groups[0]->geometry().bottom(), groups[1]->geometry().top() + 2);
}

TEST_F(MainWindowTest, TheTwoGroupsSplitTheAreaEvenly) {
    create_window();
    window_->commands().execute("workbench.action.splitEditor");
    QApplication::processEvents();
    const auto groups = window_->editor_area()->groups();
    EXPECT_NEAR(groups[0]->width(), groups[1]->width(), 3);
    EXPECT_GT(groups[0]->width(), 100);
}

TEST_F(MainWindowTest, TheFocusedGroupReceivesFileOpens) {
    create_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    window_->commands().execute("workbench.action.splitEditor");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("b.txt", "B").string())));
    const auto groups = window_->editor_area()->groups();
    EXPECT_EQ(titles_of(groups[0]), (std::vector<QString>{"Untitled-1", "a.txt"}));
    EXPECT_EQ(titles_of(groups[1]), (std::vector<QString>{"b.txt"}));
    EXPECT_FALSE(groups[1]->is_empty());
    EXPECT_EQ(current_text(), "B");
    EXPECT_EQ(window_->open_document_count(), 3U);

    window_->commands().execute("workbench.action.focusPreviousGroup");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("c.txt", "C").string())));
    EXPECT_EQ(titles_of(groups[0]), (std::vector<QString>{"Untitled-1", "a.txt", "c.txt"}));
    EXPECT_EQ(titles_of(groups[1]), (std::vector<QString>{"b.txt"}));
}

TEST_F(MainWindowTest, TwoGroupsShowDifferentDocuments) {
    create_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "AAA").string())));
    window_->commands().execute("workbench.action.splitEditor");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("b.txt", "BBB").string())));
    const auto groups = window_->editor_area()->groups();
    EXPECT_EQ(groups[0]->view()->document()->text().to_string(), "AAA");
    EXPECT_EQ(groups[1]->view()->document()->text().to_string(), "BBB");
    EXPECT_NE(groups[0]->view()->document(), groups[1]->view()->document());
}

TEST_F(MainWindowTest, OpeningADocumentThatIsOpenInAnotherGroupFocusesItThere) {
    create_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    const auto first = window_->editor_layout().focused();
    window_->commands().execute("workbench.action.splitEditor");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("b.txt", "B").string())));
    ASSERT_NE(window_->editor_layout().focused(), first);

    ASSERT_TRUE(window_->open_file(QString::fromStdString((scratch_ / "a.txt").string())));
    EXPECT_EQ(window_->editor_layout().focused(), first);
    EXPECT_EQ(current_text(), "A");
    const auto groups = window_->editor_area()->groups();
    EXPECT_EQ(titles_of(groups[1]), (std::vector<QString>{"b.txt"}));
    EXPECT_EQ(window_->open_document_count(), 3U);
}

TEST_F(MainWindowTest, ShowingTheDocumentOfAnotherGroupSwitchesItsTabToo) {
    create_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("b.txt", "B").string())));
    window_->commands().execute("workbench.action.splitEditor");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("c.txt", "C").string())));
    ASSERT_TRUE(window_->open_file(QString::fromStdString((scratch_ / "a.txt").string())));
    const auto groups = window_->editor_area()->groups();
    EXPECT_EQ(groups[0]->tabs()->current_index(), 1);
    EXPECT_EQ(groups[0]->view()->document()->text().to_string(), "A");
    EXPECT_EQ(window_->editor_layout().focused(), groups[0]->id());
}

TEST_F(MainWindowTest, TheFocusMovesBetweenGroupsWithTheCommandsAndWrapsAround) {
    create_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    window_->commands().execute("workbench.action.splitEditor");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("b.txt", "B").string())));
    const auto ids = window_->editor_layout().group_ids();
    EXPECT_EQ(window_->editor_layout().focused(), ids[1]);
    EXPECT_TRUE(window_->view()->hasFocus());

    window_->commands().execute("workbench.action.focusNextGroup");
    EXPECT_EQ(window_->editor_layout().focused(), ids[0]);
    EXPECT_EQ(current_text(), "A");
    EXPECT_TRUE(window_->view()->hasFocus());
    window_->commands().execute("workbench.action.focusPreviousGroup");
    EXPECT_EQ(window_->editor_layout().focused(), ids[1]);
    EXPECT_EQ(current_text(), "B");
    window_->commands().execute("workbench.action.focusPreviousGroup");
    EXPECT_EQ(window_->editor_layout().focused(), ids[0]);
}

TEST_F(MainWindowTest, TheFocusedGroupHasTheActiveTabHighlight) {
    create_window();
    window_->commands().execute("workbench.action.splitEditor");
    const auto groups = window_->editor_area()->groups();
    EXPECT_FALSE(groups[0]->tabs()->focused());
    EXPECT_TRUE(groups[1]->tabs()->focused());
    window_->commands().execute("workbench.action.focusPreviousGroup");
    EXPECT_TRUE(groups[0]->tabs()->focused());
    EXPECT_FALSE(groups[1]->tabs()->focused());
}

TEST_F(MainWindowTest, ClickingInAnotherGroupFocusesIt) {
    create_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    const auto first = window_->editor_layout().focused();
    window_->commands().execute("workbench.action.splitEditor");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("b.txt", "B").string())));
    window_->editor_area()->group(first)->view()->setFocus();
    EXPECT_EQ(window_->editor_layout().focused(), first);
    EXPECT_EQ(current_text(), "A");
}

namespace {

// Everything the user does to drag a tab out of a group's strip.
struct TabDrag {
    TabStrip* strip;
    QPoint global(const QPoint& local) const {
        return strip->mapToGlobal(local);
    }
    void send(QEvent::Type type, const QPoint& global_position, Qt::MouseButton button,
              Qt::MouseButtons buttons) const {
        const QPoint local = strip->mapFromGlobal(global_position);
        QMouseEvent event(type, local, global_position, button, buttons, {});
        QApplication::sendEvent(strip, &event);
    }
    void begin(int index) const {
        send(QEvent::MouseButtonPress, global(strip->tab_rect(index).center() - QPoint(10, 0)),
             Qt::LeftButton, Qt::LeftButton);
    }
    void move_to(const QPoint& global_position) const {
        send(QEvent::MouseMove, global_position, Qt::NoButton, Qt::LeftButton);
    }
    void drop_at(const QPoint& global_position) const {
        move_to(global_position);
        send(QEvent::MouseButtonRelease, global_position, Qt::LeftButton, Qt::NoButton);
    }
};

} // namespace

TEST_F(MainWindowTest, DraggingATabOntoAnotherGroupsStripMovesItThereAtThatPosition) {
    create_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    window_->commands().execute("workbench.action.splitEditor");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("b.txt", "B").string())));
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("c.txt", "C").string())));
    const auto groups = window_->editor_area()->groups();
    const TabDrag drag{groups[0]->tabs()};
    drag.begin(1);
    drag.drop_at(groups[1]->tabs()->mapToGlobal(QPoint(130, 10)));

    EXPECT_EQ(titles_of(groups[0]), (std::vector<QString>{"Untitled-1"}));
    EXPECT_EQ(titles_of(groups[1]), (std::vector<QString>{"b.txt", "a.txt", "c.txt"}));
    EXPECT_EQ(window_->editor_layout().focused(), groups[1]->id());
    EXPECT_EQ(current_text(), "A");
    EXPECT_EQ(groups[1]->tabs()->current_index(), 1);
    EXPECT_EQ(window_->open_document_count(), 4U);
}

TEST_F(MainWindowTest, DraggingATabOntoTheMiddleOfAnotherGroupAddsItAtTheEnd) {
    create_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    window_->commands().execute("workbench.action.splitEditor");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("b.txt", "B").string())));
    const auto groups = window_->editor_area()->groups();
    const TabDrag drag{groups[0]->tabs()};
    drag.begin(0);
    drag.drop_at(groups[1]->mapToGlobal(QPoint(groups[1]->width() / 2, groups[1]->height() / 2)));
    EXPECT_EQ(titles_of(groups[0]), (std::vector<QString>{"a.txt"}));
    EXPECT_EQ(titles_of(groups[1]), (std::vector<QString>{"b.txt", "Untitled-1"}));
}

TEST_F(MainWindowTest, DraggingATabToTheEdgeOfAGroupSplitsItAndPutsTheTabInTheNewGroup) {
    create_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    window_->commands().execute("workbench.action.splitEditor");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("b.txt", "B").string())));
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("c.txt", "C").string())));
    const auto before = window_->editor_area()->groups();
    const TabDrag drag{before[0]->tabs()};
    drag.begin(1);
    EditorGroupWidget* target = before[1];
    drag.drop_at(target->mapToGlobal(QPoint(target->width() - 10, target->height() / 2)));

    const auto groups = window_->editor_area()->groups();
    ASSERT_EQ(groups.size(), 3U);
    EXPECT_EQ(titles_of(groups[0]), (std::vector<QString>{"Untitled-1"}));
    EXPECT_EQ(titles_of(groups[1]), (std::vector<QString>{"b.txt", "c.txt"}));
    EXPECT_EQ(titles_of(groups[2]), (std::vector<QString>{"a.txt"}));
    EXPECT_EQ(window_->editor_layout().focused(), groups[2]->id());
    EXPECT_EQ(current_text(), "A");
    QApplication::processEvents();
    EXPECT_LT(groups[1]->geometry().right(), groups[2]->geometry().left() + 4);
}

TEST_F(MainWindowTest, DraggingATabToTheBottomEdgeOfItsOwnGroupSplitsThatGroupDown) {
    create_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    EditorGroupWidget* group = window_->editor_area()->groups()[0];
    const TabDrag drag{group->tabs()};
    drag.begin(1);
    drag.drop_at(group->mapToGlobal(QPoint(group->width() / 2, group->height() - 10)));
    const auto groups = window_->editor_area()->groups();
    ASSERT_EQ(groups.size(), 2U);
    EXPECT_EQ(titles_of(groups[0]), (std::vector<QString>{"Untitled-1"}));
    EXPECT_EQ(titles_of(groups[1]), (std::vector<QString>{"a.txt"}));
    QApplication::processEvents();
    EXPECT_EQ(window_->editor_area()->root_splitter()->orientation(), Qt::Vertical);
}

TEST_F(MainWindowTest, DraggingTheLastTabOfAGroupToAnotherClosesTheGroup) {
    create_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    window_->commands().execute("workbench.action.splitEditor");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("b.txt", "B").string())));
    const auto groups = window_->editor_area()->groups();
    const TabDrag drag{groups[1]->tabs()};
    drag.begin(0);
    drag.drop_at(groups[0]->tabs()->mapToGlobal(QPoint(10, 10)));
    EXPECT_EQ(window_->editor_area()->groups().size(), 1U);
    EXPECT_EQ(titles_of(window_->editor_area()->groups()[0]),
              (std::vector<QString>{"b.txt", "Untitled-1", "a.txt"}));
    EXPECT_EQ(current_text(), "B");
}

TEST_F(MainWindowTest, ADraggedTabKeepsItsDocumentAndNothingIsAsked) {
    create_window(false, true);
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    type("!");
    window_->commands().execute("workbench.action.splitEditor");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("b.txt", "B").string())));
    const auto groups = window_->editor_area()->groups();
    answer_next_prompt(QMessageBox::Cancel);
    const TabDrag drag{groups[0]->tabs()};
    drag.begin(1);
    drag.drop_at(groups[1]->tabs()->mapToGlobal(QPoint(300, 10)));
    EXPECT_FALSE(prompt_shown_);
    EXPECT_EQ(titles_of(groups[1]), (std::vector<QString>{"b.txt", "a.txt"}));
    EXPECT_EQ(current_text(), "!A");
    EXPECT_TRUE(window_->current_document()->is_dirty());
    EXPECT_TRUE(groups[1]->tabs()->tab(1).dirty);
}

TEST_F(MainWindowTest, EscapeKeepsADraggedTabWhereItWas) {
    create_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    window_->commands().execute("workbench.action.splitEditor");
    QApplication::processEvents();
    const auto groups = window_->editor_area()->groups();
    const TabDrag drag{groups[0]->tabs()};
    drag.begin(1);
    drag.move_to(groups[1]->mapToGlobal(QPoint(groups[1]->width() / 2, groups[1]->height() / 2)));
    EXPECT_FALSE(window_->editor_area()->drop_shade().isEmpty());
    QTest::keyClick(groups[0]->tabs(), Qt::Key_Escape);
    EXPECT_TRUE(window_->editor_area()->drop_shade().isEmpty());
    drag.send(QEvent::MouseButtonRelease,
              groups[1]->mapToGlobal(QPoint(groups[1]->width() / 2, groups[1]->height() / 2)),
              Qt::LeftButton, Qt::NoButton);
    EXPECT_EQ(titles_of(groups[0]), (std::vector<QString>{"Untitled-1", "a.txt"}));
    EXPECT_TRUE(titles_of(groups[1]).empty());
}

TEST_F(MainWindowTest, DroppingATabOutsideTheWindowChangesNothing) {
    create_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    window_->commands().execute("workbench.action.splitEditor");
    const auto groups = window_->editor_area()->groups();
    const TabDrag drag{groups[0]->tabs()};
    drag.begin(1);
    drag.drop_at(window_->mapToGlobal(QPoint(window_->width() + 400, window_->height() + 400)));
    EXPECT_EQ(titles_of(groups[0]), (std::vector<QString>{"Untitled-1", "a.txt"}));
    EXPECT_EQ(window_->editor_area()->groups().size(), 2U);
}

TEST_F(MainWindowTest, ADragAcrossGroupsRendersForReview) {
    create_window();
    ASSERT_TRUE(window_->open_file(
        QString::fromStdString(write("first.txt", "hello, world\nsecond line\n").string())));
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("other.txt", "other\n").string())));
    window_->commands().execute("workbench.action.splitEditor");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("right.txt", "right\n").string())));
    const auto groups = window_->editor_area()->groups();
    const TabDrag drag{groups[0]->tabs()};
    drag.begin(2);
    drag.move_to(groups[1]->mapToGlobal(QPoint(groups[1]->width() - 20, groups[1]->height() / 2)));
    QApplication::processEvents();
    const QImage image = window_->grab().toImage();
    EXPECT_GT(image.width(), 100);
    if (const char* directory = std::getenv("ANVIL_SCREENSHOT_DIR")) {
        image.save(QString::fromUtf8(directory) + "/drag_tab_edge.png");
    }
    QTest::keyClick(groups[0]->tabs(), Qt::Key_Escape);
}

TEST_F(MainWindowTest, SelectingATabOfAnotherGroupFocusesThatGroup) {
    create_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    const auto first = window_->editor_layout().focused();
    window_->commands().execute("workbench.action.splitEditor");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("b.txt", "B").string())));
    TabStrip* strip = window_->editor_area()->group(first)->tabs();
    const QPoint at = strip->tab_rect(0).center() - QPoint(10, 0);
    QMouseEvent press(QEvent::MouseButtonPress, at, strip->mapToGlobal(at), Qt::LeftButton,
                      Qt::LeftButton, {});
    QApplication::sendEvent(strip, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, at, strip->mapToGlobal(at), Qt::LeftButton,
                        Qt::NoButton, {});
    QApplication::sendEvent(strip, &release);
    EXPECT_EQ(window_->editor_layout().focused(), first);
    EXPECT_EQ(window_->current_uri().scheme, "untitled");
}

TEST_F(MainWindowTest, TheStatusBarFollowsTheCaretOfTheFocusedGroup) {
    create_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "abc\ndef").string())));
    window_->view()->move(core::Motion::right);
    window_->view()->move(core::Motion::right);
    EXPECT_EQ(label_text("Ln "), "Ln 1, Col 3");
    window_->commands().execute("workbench.action.splitEditor");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("b.txt", "xyz").string())));
    EXPECT_EQ(label_text("Ln "), "Ln 1, Col 1");
    window_->commands().execute("workbench.action.focusPreviousGroup");
    EXPECT_EQ(label_text("Ln "), "Ln 1, Col 3");
    window_->editor_area()->groups()[1]->view()->move(core::Motion::down);
    EXPECT_EQ(label_text("Ln "), "Ln 1, Col 3");
}

TEST_F(MainWindowTest, TheTitleAndTheCommandsFollowTheFocusedGroup) {
    create_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    window_->commands().execute("workbench.action.splitEditor");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("b.txt", "B").string())));
    EXPECT_TRUE(window_->windowTitle().contains("b.txt"));
    type("!");
    EXPECT_EQ(window_->editor_area()->groups()[1]->view()->document()->text().to_string(), "!B");
    EXPECT_EQ(window_->editor_area()->groups()[0]->view()->document()->text().to_string(), "A");
    window_->commands().execute("workbench.action.focusPreviousGroup");
    EXPECT_TRUE(window_->windowTitle().contains("a.txt"));
    EXPECT_FALSE(window_->isWindowModified());
    window_->commands().execute("workbench.action.focusNextGroup");
    EXPECT_TRUE(window_->isWindowModified());
    EXPECT_TRUE(window_->editor_area()->groups()[1]->tabs()->tab(0).dirty);
    EXPECT_FALSE(window_->editor_area()->groups()[0]->tabs()->tab(1).dirty);
}

TEST_F(MainWindowTest, TypingInOneGroupLeavesTheOthersAlone) {
    create_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    window_->commands().execute("workbench.action.splitEditor");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("b.txt", "B").string())));
    type("1");
    window_->commands().execute("workbench.action.files.save");
    EXPECT_EQ(*platform::read_file(scratch_ / "b.txt"), "1B");
    EXPECT_EQ(*platform::read_file(scratch_ / "a.txt"), "A");
}

TEST_F(MainWindowTest, ClosingTheLastTabOfAGroupClosesTheGroup) {
    create_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    window_->commands().execute("workbench.action.splitEditor");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("b.txt", "B").string())));
    ASSERT_EQ(window_->editor_area()->groups().size(), 2U);
    EXPECT_TRUE(window_->close_document(0));
    EXPECT_EQ(window_->editor_area()->groups().size(), 1U);
    EXPECT_EQ(window_->editor_area()->root_splitter(), nullptr);
    EXPECT_EQ(window_->open_document_count(), 2U);
    EXPECT_EQ(titles_of(window_->editor_area()->groups()[0]),
              (std::vector<QString>{"Untitled-1", "a.txt"}));
    EXPECT_EQ(window_->editor_layout().focused(), window_->editor_area()->groups()[0]->id());
    EXPECT_EQ(current_text(), "A");
}

TEST_F(MainWindowTest, CloseEditorGroupClosesTheGroupWithItsDocuments) {
    create_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    window_->commands().execute("workbench.action.splitEditor");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("b.txt", "B").string())));
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("c.txt", "C").string())));
    window_->commands().execute("workbench.action.closeGroup");
    EXPECT_EQ(window_->editor_area()->groups().size(), 1U);
    EXPECT_EQ(window_->open_document_count(), 2U);
    EXPECT_EQ(current_text(), "A");
}

TEST_F(MainWindowTest, CloseEditorGroupAsksAboutUnsavedChangesAndCancelKeepsTheGroup) {
    create_window(false, true);
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    window_->commands().execute("workbench.action.splitEditor");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("b.txt", "B").string())));
    type("!");
    answer_next_prompt(QMessageBox::Cancel);
    window_->commands().execute("workbench.action.closeGroup");
    EXPECT_TRUE(prompt_shown_);
    EXPECT_EQ(window_->editor_area()->groups().size(), 2U);
    EXPECT_EQ(window_->open_document_count(), 3U);

    answer_next_prompt(QMessageBox::Discard);
    window_->commands().execute("workbench.action.closeGroup");
    EXPECT_TRUE(prompt_shown_);
    EXPECT_EQ(window_->editor_area()->groups().size(), 1U);
    EXPECT_EQ(window_->open_document_count(), 2U);
    EXPECT_EQ(*platform::read_file(scratch_ / "b.txt"), "B");
}

TEST_F(MainWindowTest, TheOnlyGroupCannotBeClosed) {
    create_window();
    window_->commands().execute("workbench.action.closeGroup");
    EXPECT_EQ(window_->editor_area()->groups().size(), 1U);
    EXPECT_EQ(window_->open_document_count(), 1U);
}

TEST_F(MainWindowTest, ClosingTheLastDocumentOfAGroupWhileTheOthersAreEmptyLeavesAnUntitledOne) {
    create_window();
    window_->commands().execute("workbench.action.splitEditor");
    window_->commands().execute("workbench.action.focusPreviousGroup");
    EXPECT_TRUE(window_->close_document(0));
    EXPECT_EQ(window_->open_document_count(), 1U);
    EXPECT_EQ(window_->current_uri().scheme, "untitled");
    EXPECT_EQ(window_->current_uri().path, "Untitled-2");
    EXPECT_EQ(window_->editor_area()->groups().size(), 2U);
}

TEST_F(MainWindowTest, AnEmptyFocusedGroupHasNoDocumentToCommandOrToShow) {
    create_window();
    window_->commands().execute("workbench.action.splitEditor");
    EXPECT_EQ(window_->windowTitle(), "Anvil");
    EXPECT_TRUE(label_text("Ln ").isEmpty());
    EXPECT_FALSE(window_->save());
    window_->commands().execute("workbench.action.closeActiveEditor");
    window_->commands().execute("actions.find");
    window_->commands().execute("undo");
    EXPECT_FALSE(window_->editor_area()->focused_group()->find_bar()->isVisible());
    EXPECT_EQ(window_->open_document_count(), 1U);
}

TEST_F(MainWindowTest, SplittingAGroupThatIsNotFocusedAddsTheGroupNextToIt) {
    create_window();
    window_->commands().execute("workbench.action.splitEditor");
    window_->commands().execute("workbench.action.focusPreviousGroup");
    window_->commands().execute("workbench.action.splitEditorDown");
    const auto ids = window_->editor_layout().group_ids();
    ASSERT_EQ(ids.size(), 3U);
    EXPECT_EQ(window_->editor_layout().focused(), ids[1]);
    QApplication::processEvents();
    const auto* root = window_->editor_area()->root_splitter();
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->count(), 2);
    const auto* nested = qobject_cast<const QSplitter*>(root->widget(0));
    ASSERT_NE(nested, nullptr);
    EXPECT_EQ(nested->orientation(), Qt::Vertical);
}

TEST_F(MainWindowTest, MovingASplitterUpdatesTheRatiosOfTheLayout) {
    create_window();
    window_->commands().execute("workbench.action.splitEditor");
    QApplication::processEvents();
    QSplitter* splitter = window_->editor_area()->root_splitter();
    ASSERT_NE(splitter, nullptr);
    const int total = splitter->width();
    QSplitterHandle* handle = splitter->handle(1);
    const QPoint from = handle->rect().center();
    const QPoint to = from + QPoint(total / 4 - handle->mapTo(splitter, from).x(), 0);
    QMouseEvent press(QEvent::MouseButtonPress, from, handle->mapToGlobal(from), Qt::LeftButton,
                      Qt::LeftButton, {});
    QApplication::sendEvent(handle, &press);
    QMouseEvent move(QEvent::MouseMove, to, handle->mapToGlobal(to), Qt::NoButton, Qt::LeftButton,
                     {});
    QApplication::sendEvent(handle, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, to, handle->mapToGlobal(to), Qt::LeftButton,
                        Qt::NoButton, {});
    QApplication::sendEvent(handle, &release);
    const auto& ratios = window_->editor_layout().root().ratios;
    ASSERT_EQ(ratios.size(), 2U);
    EXPECT_NEAR(ratios[0], 0.25, 0.03);
    EXPECT_NEAR(ratios[0] + ratios[1], 1.0, 1e-9);
}

TEST_F(MainWindowTest, TheSplitterStaysWhereTheUserLeavesItWhateverThePixel) {
    create_window();
    window_->commands().execute("workbench.action.splitEditor");
    QApplication::processEvents();
    QSplitter* splitter = window_->editor_area()->root_splitter();
    ASSERT_NE(splitter, nullptr);
    QSplitterHandle* handle = splitter->handle(1);
    const QPoint from = handle->rect().center();
    QMouseEvent press(QEvent::MouseButtonPress, from, handle->mapToGlobal(from), Qt::LeftButton,
                      Qt::LeftButton, {});
    QApplication::sendEvent(handle, &press);
    for (int x = 131; x < 700; x += 37) {
        const QPoint to(from.x() + x - handle->mapTo(splitter, from).x(), from.y());
        QMouseEvent move(QEvent::MouseMove, to, handle->mapToGlobal(to), Qt::NoButton,
                         Qt::LeftButton, {});
        QApplication::sendEvent(handle, &move);
        const QList<int> sizes = splitter->sizes();
        QApplication::processEvents();
        EXPECT_EQ(splitter->sizes(), sizes) << "at " << x;
    }
    QMouseEvent release(QEvent::MouseButtonRelease, from, handle->mapToGlobal(from), Qt::LeftButton,
                        Qt::NoButton, {});
    QApplication::sendEvent(handle, &release);
}

TEST_F(MainWindowTest, ChangingTheRatiosOfTheLayoutMovesTheSplitter) {
    create_window();
    window_->commands().execute("workbench.action.splitEditor");
    QApplication::processEvents();
    window_->editor_layout().set_ratios({}, {1, 3});
    QApplication::processEvents();
    const auto groups = window_->editor_area()->groups();
    EXPECT_NEAR(static_cast<double>(groups[0]->width()) / groups[1]->width(), 1.0 / 3.0, 0.05);
}

TEST_F(MainWindowTest, GroupsThatAreCreatedLaterGetTheSettings) {
    settings_file_ = write("settings.json", R"j({"editor.fontSize": 20})j");
    create_window();
    window_->commands().execute("workbench.action.splitEditor");
    const auto groups = window_->editor_area()->groups();
    EXPECT_EQ(groups[1]->view()->line_height(), groups[0]->view()->line_height());
    EXPECT_GT(groups[1]->view()->line_height(), 20);
}

TEST_F(MainWindowTest, SavingADocumentUnderANewNameRenamesItsTabInItsGroup) {
    create_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    window_->commands().execute("workbench.action.splitEditor");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("b.txt", "B").string())));
    ASSERT_TRUE(window_->save_as(QString::fromStdString((scratch_ / "renamed.txt").string())));
    const auto groups = window_->editor_area()->groups();
    EXPECT_EQ(titles_of(groups[1]), (std::vector<QString>{"renamed.txt"}));
    EXPECT_EQ(titles_of(groups[0]), (std::vector<QString>{"Untitled-1", "a.txt"}));
    EXPECT_TRUE(window_->windowTitle().contains("renamed.txt"));
}

TEST_F(MainWindowTest, ASplitWindowRendersForReview) {
    create_window();
    ASSERT_TRUE(window_->open_file(
        QString::fromStdString(write("first.txt", "hello, world\nsecond line\n").string())));
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("other.txt", "other\n").string())));
    window_->commands().execute("workbench.action.splitEditor");
    ASSERT_TRUE(window_->open_file(
        QString::fromStdString(write("right.txt", "a document\nin the second group\n").string())));
    window_->commands().execute("workbench.action.splitEditorDown");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("below.txt", "below\n").string())));
    window_->commands().execute("workbench.action.focusPreviousGroup");
    QApplication::processEvents();
    const QImage image = window_->grab().toImage();
    EXPECT_GT(image.width(), 100);
    if (const char* directory = std::getenv("ANVIL_SCREENSHOT_DIR")) {
        image.save(QString::fromUtf8(directory) + "/split_editors.png");
    }
}

// --- with the extension host ---

TEST_F(MainWindowTest, ExtensionsSeeTheShownDocument) {
    create_window(true);
    if (!window_) {
        return;
    }
    const auto path = write("a.txt", "hello\nworld");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(path.string())));
    ASSERT_TRUE(window_->bridge().running());
    const json active = run("anvil.docs.active");
    EXPECT_EQ(active["text"], "hello\nworld");
    EXPECT_EQ(active["uri"], core::Uri::file(path).to_string());
}

TEST_F(MainWindowTest, TheActiveEditorOfExtensionsFollowsTheFocusedGroup) {
    create_window(true);
    if (!window_) {
        return;
    }
    const auto a = write("a.txt", "AAA");
    const auto b = write("b.txt", "BBB");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(a.string())));
    window_->commands().execute("workbench.action.splitEditor");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(b.string())));
    EXPECT_EQ(run("anvil.docs.active")["uri"], core::Uri::file(b).to_string());

    window_->commands().execute("workbench.action.focusPreviousGroup");
    ASSERT_TRUE(wait_until(
        [&] { return run("anvil.docs.active")["uri"] == core::Uri::file(a).to_string(); }));
    type("X");
    EXPECT_EQ(run("anvil.docs.active")["text"], "XAAA");

    window_->commands().execute("workbench.action.focusNextGroup");
    ASSERT_TRUE(wait_until(
        [&] { return run("anvil.docs.active")["uri"] == core::Uri::file(b).to_string(); }));
    EXPECT_EQ(run("anvil.docs.active")["text"], "BBB");
}

TEST_F(MainWindowTest, TypingInTheEditorReachesExtensions) {
    create_window(true);
    if (!window_) {
        return;
    }
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "hello").string())));
    type("X");
    const json active = run("anvil.docs.active");
    EXPECT_EQ(active["text"], "Xhello");
    EXPECT_TRUE(active["isDirty"].get<bool>());
    EXPECT_EQ(active["selections"], json::parse("[[0,1,0,1]]"));
}

TEST_F(MainWindowTest, EditsMadeByExtensionsAppearInTheEditor) {
    create_window(true);
    if (!window_) {
        return;
    }
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "world").string())));
    EXPECT_TRUE(run("anvil.docs.insert", json::array({"hello "})).get<bool>());
    EXPECT_EQ(current_text(), "hello world");
    EXPECT_TRUE(window_->isWindowModified());
    window_->view()->undo();
    EXPECT_EQ(current_text(), "world");
}

TEST_F(MainWindowTest, TheEditorRefreshesWhenAnExtensionChangesTheText) {
    create_window(true);
    if (!window_) {
        return;
    }
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "x").string())));
    ASSERT_EQ(window_->view()->verticalScrollBar()->maximum(), 0); // one line fits the viewport
    EXPECT_TRUE(run("anvil.docs.insert", json::array({std::string(400, '\n')})).get<bool>());
    // The view only learns the new size from the notification that an extension edited the text.
    EXPECT_GT(window_->view()->verticalScrollBar()->maximum(),
              300 * window_->view()->line_height());
}

TEST_F(MainWindowTest, ExtensionsCanAskTheWindowToShowAnotherDocument) {
    create_window(true);
    if (!window_) {
        return;
    }
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    const auto other = write("other.txt", "second file");
    run("anvil.docs.show", json::array({other.string()}));
    EXPECT_EQ(window_->current_uri().file_path(), other);
    EXPECT_EQ(current_text(), "second file");
    EXPECT_EQ(window_->open_document_count(), 3u);
}

TEST_F(MainWindowTest, SavingTheDocumentIsReportedToExtensions) {
    create_window(true);
    if (!window_) {
        return;
    }
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "abc").string())));
    type("!");
    ASSERT_TRUE(window_->save());
    ASSERT_TRUE(wait_until([&] { return !run("anvil.docs.active")["isDirty"].get<bool>(); }));
    EXPECT_EQ(run("anvil.docs.saves").size(), 1u);
}

TEST_F(MainWindowTest, SavingFromAnExtensionClearsTheModifiedMarkers) {
    create_window(true);
    if (!window_) {
        return;
    }
    const auto path = write("a.txt", "abc");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(path.string())));
    type("!");
    ASSERT_TRUE(window_->isWindowModified());
    ASSERT_TRUE(window_->tab_strip()->tab(1).dirty);

    run("anvil.docs.save");
    EXPECT_EQ(*platform::read_file(path), "!abc");
    EXPECT_FALSE(window_->isWindowModified());
    EXPECT_EQ(window_->tab_strip()->tab(1).title, "a.txt");
    EXPECT_FALSE(window_->tab_strip()->tab(1).dirty);
}

TEST_F(MainWindowTest, ExtensionCommandsAreAvailableFromTheCommandList) {
    create_window(true);
    if (!window_) {
        return;
    }
    const auto ids = window_->bridge().host()->commands().command_ids();
    EXPECT_NE(std::find(ids.begin(), ids.end(), "anvil.docs.insert"), ids.end());
}

TEST_F(MainWindowTest, FindAndReplaceAreReachableFromTheEditMenuAndTheirShortcuts) {
    create_window();
    window_->activateWindow();
    EXPECT_TRUE(window_->find_bar()->isHidden());

    QTest::keyClick(window_->view(), Qt::Key_F, Qt::ControlModifier);
    EXPECT_FALSE(window_->find_bar()->isHidden());
    EXPECT_TRUE(window_->find_bar()->find_field()->hasFocus());
    EXPECT_FALSE(window_->find_bar()->replace_field()->isVisibleTo(window_->find_bar()));

    window_->find_bar()->close_bar();
    QTest::keyClick(window_->view(), Qt::Key_H, Qt::ControlModifier);
    EXPECT_TRUE(window_->find_bar()->replace_field()->isVisibleTo(window_->find_bar()));
}

TEST_F(MainWindowTest, TheFindBarSitsBetweenTheTabBarAndTheEditor) {
    create_window();
    window_->find_bar()->open();
    QApplication::processEvents();
    const int tabs_bottom = window_->tab_strip()->geometry().bottom();
    const int bar_top = window_->find_bar()->geometry().top();
    const int bar_bottom = window_->find_bar()->geometry().bottom();
    const int editor_top = window_->view()->geometry().top();
    EXPECT_LE(tabs_bottom, bar_top);
    EXPECT_LE(bar_bottom, editor_top);
}

TEST_F(MainWindowTest, FindNextAndPreviousWorkFromTheMenuShortcuts) {
    create_window();
    window_->activateWindow();
    window_->view()->setFocus();
    type("a b a b");
    window_->find_bar()->open();
    window_->find_bar()->find_field()->clear();
    window_->find_bar()->find_field()->setText("b");
    ASSERT_EQ(window_->current_document()->selections().primary(), (core::Selection{2, 3}));

    QTest::keyClick(window_->view(), Qt::Key_F3);
    EXPECT_EQ(window_->current_document()->selections().primary(), (core::Selection{6, 7}));
    QTest::keyClick(window_->view(), Qt::Key_F3, Qt::ShiftModifier);
    EXPECT_EQ(window_->current_document()->selections().primary(), (core::Selection{2, 3}));
}

TEST_F(MainWindowTest, SwitchingTabsSearchesTheNewDocument) {
    create_window();
    const auto path = write("b.txt", "x x x x");
    window_->find_bar()->open();
    window_->find_bar()->find_field()->setText("x");
    ASSERT_EQ(window_->find_bar()->match_count(), 0u);

    ASSERT_TRUE(window_->open_file(QString::fromStdString(path.string())));
    EXPECT_EQ(window_->find_bar()->match_count(), 4u);
    click_tab(0);
    EXPECT_EQ(window_->find_bar()->match_count(), 0u);
}

TEST_F(MainWindowTest, ReplacingMarksTheDocumentModifiedAndReachesTheTab) {
    create_window();
    const auto path = write("c.txt", "one two one");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(path.string())));
    ASSERT_FALSE(window_->isWindowModified());
    window_->find_bar()->open(true);
    window_->find_bar()->find_field()->setText("one");
    window_->find_bar()->replace_field()->setText("1");
    window_->find_bar()->replace_all();
    EXPECT_EQ(window_->current_document()->text().to_string(), "1 two 1");
    EXPECT_TRUE(window_->isWindowModified());
    EXPECT_TRUE(window_->tab_strip()->tab(1).dirty);
    EXPECT_EQ(window_->tab_strip()->tab(1).title, "c.txt");
}

TEST_F(MainWindowTest, CtrlFSeedsTheFindFieldFromTheEditor) {
    create_window();
    window_->activateWindow();
    window_->view()->setFocus();
    type("hello world");
    window_->view()->select_range({6, 6});

    QTest::keyClick(window_->view(), Qt::Key_F, Qt::ControlModifier);
    EXPECT_EQ(window_->find_bar()->find_field()->text(), "world");
    EXPECT_EQ(window_->find_bar()->status(), "? of 1");
}

namespace {

Qt::KeyboardModifiers option_modifiers() {
#ifdef Q_OS_MACOS
    return Qt::ControlModifier | Qt::AltModifier;
#else
    return Qt::AltModifier;
#endif
}

} // namespace

TEST_F(MainWindowTest, TheOptionShortcutsWorkFromTheEditorWhileTheBarIsOpen) {
    create_window();
    window_->activateWindow();
    window_->find_bar()->open();
    window_->view()->setFocus();
    QApplication::processEvents();
    auto* bar = window_->find_bar();

    QTest::keyClick(window_->view(), Qt::Key_C, option_modifiers());
    EXPECT_TRUE(bar->match_case_button()->isChecked());
    QTest::keyClick(window_->view(), Qt::Key_W, option_modifiers());
    EXPECT_TRUE(bar->whole_word_button()->isChecked());
    QTest::keyClick(window_->view(), Qt::Key_R, option_modifiers());
    EXPECT_TRUE(bar->regex_button()->isChecked());
    EXPECT_EQ(QApplication::activePopupWidget(), nullptr);

    QTest::keyClick(window_->view(), Qt::Key_C, option_modifiers());
    EXPECT_FALSE(bar->match_case_button()->isChecked());
}

TEST_F(MainWindowTest, TheOptionShortcutsWorkFromTheFindFieldToo) {
    create_window();
    window_->activateWindow();
    window_->find_bar()->open();
    QApplication::processEvents();
    QTest::keyClick(window_->find_bar()->find_field(), Qt::Key_C, option_modifiers());
    EXPECT_TRUE(window_->find_bar()->match_case_button()->isChecked());
}

TEST_F(MainWindowTest, TheOptionShortcutsDoNothingWhileTheBarIsClosed) {
    create_window();
    window_->activateWindow();
    window_->view()->setFocus();
    QApplication::processEvents();
    ASSERT_TRUE(window_->find_bar()->isHidden());
    for (const Qt::Key key : {Qt::Key_C, Qt::Key_W, Qt::Key_R}) {
        QTest::keyClick(window_->view(), key, option_modifiers());
    }
    EXPECT_FALSE(window_->find_bar()->match_case_button()->isChecked());
    EXPECT_FALSE(window_->find_bar()->whole_word_button()->isChecked());
    EXPECT_FALSE(window_->find_bar()->regex_button()->isChecked());
    EXPECT_EQ(QApplication::activePopupWidget(), nullptr);
}

// A shortcut that is also a menu mnemonic is ambiguous, and Qt then triggers neither.
TEST_F(MainWindowTest, NoMenuUsesAnOptionShortcutAsItsMnemonic) {
    create_window();
    for (const QAction* menu : window_->menuBar()->actions()) {
        const QKeySequence mnemonic = QKeySequence::mnemonic(menu->text());
        for (const Qt::Key key : {Qt::Key_C, Qt::Key_W, Qt::Key_R}) {
            EXPECT_NE(mnemonic, QKeySequence(Qt::ALT | key)) << menu->text().toStdString();
        }
    }
}

// --- settings ---

TEST_F(MainWindowTest, WithoutASettingsFileTheEditorUsesTheDefaults) {
    settings_file_ = scratch_ / "settings.json";
    create_window();
    EXPECT_EQ(window_->view()->font().pixelSize(), platform::editor_defaults().font_size);
    EXPECT_EQ(window_->current_document()->line_ending(), core::LineEnding::lf);
    EXPECT_FALSE(std::filesystem::exists(settings_file_));
}

TEST_F(MainWindowTest, TheSettingsFileDrivesTheEditorAtStartup) {
    settings_file_ = write("settings.json", R"j({
        // VS Code's key names
        "editor.fontSize": 22,
        "editor.tabSize": 3,
        "editor.insertSpaces": true,
        "files.eol": "\r\n",
    })j");
    create_window();
    EXPECT_EQ(window_->view()->font().pixelSize(), 22);
    EXPECT_EQ(window_->current_document()->line_ending(), core::LineEnding::crlf);
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
    QApplication::sendEvent(window_->view(), &press);
    EXPECT_EQ(current_text(), "   ");
}

TEST_F(MainWindowTest, ChangingTheFileChangesTheMetricsWithoutARestartAndKeepsTheCaret) {
    settings_file_ = write("settings.json", R"j({"editor.fontSize": 14})j");
    create_window();
    type("hello");
    const int height = window_->view()->line_height();
    const int width = window_->view()->char_width();
    const auto selections = window_->current_document()->selections();

    write("settings.json", R"j({"editor.fontSize": 30})j");
    ASSERT_TRUE(wait_until([&] { return window_->view()->line_height() != height; }));
    EXPECT_GT(window_->view()->line_height(), height);
    EXPECT_GT(window_->view()->char_width(), width);
    EXPECT_EQ(window_->current_document()->selections(), selections);
    EXPECT_EQ(current_text(), "hello");
}

TEST_F(MainWindowTest, ACorruptFileKeepsThePreviousValuesAndReportsOnceInTheStatusBar) {
    settings_file_ = write("settings.json", R"j({"editor.fontSize": 20})j");
    auto logger = std::make_shared<core::RecordingLogger>();
    ui::MainWindowOptions options;
    options.settings_file = settings_file_;
    options.logger = logger;
    window_ = std::make_unique<MainWindow>(std::move(options));
    window_->show();
    QSignalSpy failures(&window_->settings(), &ui::SettingsStore::load_failed);
    ASSERT_EQ(window_->view()->font().pixelSize(), 20);

    write("settings.json", "{ \"editor.fontSize\": 40,, }");
    ASSERT_TRUE(wait_until([&] { return failures.count() == 1; }));
    EXPECT_EQ(window_->view()->font().pixelSize(), 20);
    const QString shown = window_->status_bar().message();
    EXPECT_TRUE(shown.contains("line 1, column 25")) << shown.toStdString();
    EXPECT_TRUE(logger->contains(core::LogLevel::error, "line 1, column 25", "settings"));

    window_->status_bar().clear_message();
    write("settings.json", "{ \"editor.fontSize\": 40,, }");
    QTest::qWait(400);
    EXPECT_EQ(failures.count(), 1);
    EXPECT_TRUE(window_->status_bar().message().isEmpty());
    EXPECT_EQ(window_->view()->font().pixelSize(), 20);
}

TEST_F(MainWindowTest, AWronglyTypedValueKeepsThePreviousValuesAndNamesTheKey) {
    settings_file_ = write("settings.json", R"j({"editor.fontSize": 20, "editor.tabSize": 2})j");
    create_window();
    write("settings.json", R"j({"editor.fontSize": 40, "editor.tabSize": "wide"})j");
    ASSERT_TRUE(
        wait_until([&] { return window_->status_bar().message().contains("editor.tabSize"); }));
    EXPECT_EQ(window_->view()->font().pixelSize(), 20);
}

TEST_F(MainWindowTest, TheEditorStartsWhenTheSettingsFileIsCorrupt) {
    settings_file_ = write("settings.json", "not json at all");
    create_window();
    EXPECT_EQ(window_->view()->font().pixelSize(), platform::editor_defaults().font_size);
    EXPECT_TRUE(window_->status_bar().message().contains("settings.json"));
    type("still works");
    EXPECT_EQ(current_text(), "still works");
}

TEST_F(MainWindowTest, NewUntitledDocumentsTakeTheirLineEndingFromTheSettings) {
    settings_file_ = write("settings.json", R"j({"files.eol": "\r\n"})j");
    create_window();
    ASSERT_EQ(window_->current_document()->line_ending(), core::LineEnding::crlf);

    write("settings.json", R"j({"files.eol": "\n"})j");
    ASSERT_TRUE(wait_until(
        [&] { return window_->settings().editor().line_ending == core::LineEnding::lf; }));
    ASSERT_TRUE(window_->close_document(0));
    EXPECT_EQ(window_->current_document()->line_ending(), core::LineEnding::lf);
}

TEST_F(MainWindowTest, OpenedFilesKeepTheirOwnLineEndingWhateverTheSetting) {
    settings_file_ = write("settings.json", R"j({"files.eol": "\r\n"})j");
    create_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("lf.txt", "a\nb\n").string())));
    EXPECT_EQ(window_->current_document()->line_ending(), core::LineEnding::lf);
}

TEST_F(MainWindowTest, ExtensionsSeeTheTabSizeAndInsertSpacesFromTheSettings) {
    settings_file_ =
        write("settings.json", R"j({"editor.tabSize": 2, "editor.insertSpaces": false})j");
    create_window(true);
    json active = run("anvil.docs.active");
    ASSERT_TRUE(active.contains("options")) << active.dump();
    EXPECT_EQ(active["options"]["tabSize"], 2);
    EXPECT_EQ(active["options"]["insertSpaces"], false);

    write("settings.json", R"j({"editor.tabSize": 6, "editor.insertSpaces": true})j");
    ASSERT_TRUE(wait_until([&] { return run("anvil.docs.active")["options"]["tabSize"] == 6; }));
    EXPECT_EQ(run("anvil.docs.active")["options"]["insertSpaces"], true);
}

TEST_F(MainWindowTest, TheEditorsOperationsAreCommandsWithVsCodeIds) {
    create_window();
    const std::vector<std::string> ids = {"undo",
                                          "redo",
                                          "cursorLeft",
                                          "cursorRight",
                                          "cursorUp",
                                          "cursorDown",
                                          "cursorHome",
                                          "cursorEnd",
                                          "deleteLeft",
                                          "deleteRight",
                                          "editor.action.selectAll",
                                          "editor.action.clipboardCopyAction",
                                          "editor.action.clipboardCutAction",
                                          "editor.action.clipboardPasteAction",
                                          "workbench.action.files.save",
                                          "workbench.action.files.saveAs",
                                          "workbench.action.files.openFile",
                                          "workbench.action.closeActiveEditor",
                                          "actions.find",
                                          "editor.action.nextMatchFindAction",
                                          "editor.action.previousMatchFindAction",
                                          "editor.action.startFindReplaceAction",
                                          "workbench.action.showCommands",
                                          "workbench.action.toggleSidebarVisibility",
                                          "workbench.action.togglePanel",
                                          "workbench.action.toggleActivityBarVisibility",
                                          "workbench.action.toggleMaximizedPanel",
                                          "workbench.action.splitEditor",
                                          "workbench.action.splitEditorRight",
                                          "workbench.action.splitEditorDown",
                                          "workbench.action.splitEditorLeft",
                                          "workbench.action.splitEditorUp",
                                          "workbench.action.focusNextGroup",
                                          "workbench.action.focusPreviousGroup",
                                          "workbench.action.closeGroup",
                                          "workbench.action.files.newUntitledFile",
                                          "workbench.action.quit",
                                          "workbench.action.closeWindow",
                                          "editor.action.addSelectionToNextFindMatch",
                                          "editor.action.selectHighlights",
                                          "editor.action.selectAllMatches",
                                          "setContext"};
    for (const std::string& id : ids) {
        const auto* command = window_->commands().find(id);
        ASSERT_NE(command, nullptr) << id;
        EXPECT_FALSE(command->title.empty()) << id;
    }
    EXPECT_EQ(window_->commands().commands().size(), ids.size());
}

TEST_F(MainWindowTest, UndoAndRedoCommandsChangeTheText) {
    create_window();
    type("ab");
    window_->commands().execute("undo");
    EXPECT_EQ(current_text(), "");
    window_->commands().execute("redo");
    EXPECT_EQ(current_text(), "ab");
}

TEST_F(MainWindowTest, CursorCommandsMoveTheCaret) {
    create_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "ab\ncd").string())));
    const auto caret = [&] { return window_->current_document()->selections().primary(); };

    window_->commands().execute("cursorRight");
    EXPECT_EQ(caret(), (core::Selection{1, 1}));
    window_->commands().execute("cursorLeft");
    EXPECT_EQ(caret(), (core::Selection{0, 0}));
    window_->commands().execute("cursorDown");
    EXPECT_EQ(caret(), (core::Selection{3, 3}));
    window_->commands().execute("cursorUp");
    EXPECT_EQ(caret(), (core::Selection{0, 0}));
    window_->commands().execute("cursorEnd");
    EXPECT_EQ(caret(), (core::Selection{2, 2}));
    window_->commands().execute("cursorHome");
    EXPECT_EQ(caret(), (core::Selection{0, 0}));
}

TEST_F(MainWindowTest, DeleteCommandsRemoveTheCharacterBesideTheCaret) {
    create_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "abc").string())));
    window_->commands().execute("cursorRight");
    window_->commands().execute("deleteLeft");
    EXPECT_EQ(current_text(), "bc");
    window_->commands().execute("deleteRight");
    EXPECT_EQ(current_text(), "c");
}

TEST_F(MainWindowTest, SelectAllAndTheClipboardCommandsEditTheText) {
    create_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "abc").string())));
    window_->commands().execute("editor.action.selectAll");
    EXPECT_EQ(window_->current_document()->selections().primary(), (core::Selection{0, 3}));

    window_->commands().execute("editor.action.clipboardCopyAction");
    EXPECT_EQ(QGuiApplication::clipboard()->text(), "abc");
    EXPECT_EQ(current_text(), "abc");

    window_->commands().execute("editor.action.clipboardCutAction");
    EXPECT_EQ(current_text(), "");
    window_->commands().execute("editor.action.clipboardPasteAction");
    window_->commands().execute("editor.action.clipboardPasteAction");
    EXPECT_EQ(current_text(), "abcabc");
}

TEST_F(MainWindowTest, FindCommandsOpenTheFindBarAndStepThroughMatches) {
    create_window();
    EXPECT_TRUE(window_->find_bar()->isHidden());
    window_->commands().execute("actions.find");
    EXPECT_FALSE(window_->find_bar()->isHidden());
    EXPECT_FALSE(window_->find_bar()->replace_field()->isVisibleTo(window_->find_bar()));
    window_->commands().execute("editor.action.startFindReplaceAction");
    EXPECT_TRUE(window_->find_bar()->replace_field()->isVisibleTo(window_->find_bar()));

    type("a b a b");
    window_->find_bar()->find_field()->clear();
    window_->find_bar()->find_field()->setText("b");
    ASSERT_EQ(window_->current_document()->selections().primary(), (core::Selection{2, 3}));
    window_->commands().execute("editor.action.nextMatchFindAction");
    EXPECT_EQ(window_->current_document()->selections().primary(), (core::Selection{6, 7}));
    window_->commands().execute("editor.action.previousMatchFindAction");
    EXPECT_EQ(window_->current_document()->selections().primary(), (core::Selection{2, 3}));
}

TEST_F(MainWindowTest, SaveAndCloseCommandsActOnTheShownDocument) {
    create_window();
    const auto path = write("a.txt", "abc");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(path.string())));
    type("X");
    window_->commands().execute("workbench.action.files.save");
    EXPECT_EQ(*platform::read_file(path), "Xabc");
    EXPECT_FALSE(window_->isWindowModified());

    ASSERT_EQ(window_->open_document_count(), 2U);
    window_->commands().execute("workbench.action.closeActiveEditor");
    EXPECT_EQ(window_->open_document_count(), 1U);
}

TEST_F(MainWindowTest, TheMenusRunTheirCommands) {
    create_window();
    const std::vector<std::string> command_ids = {
        "workbench.action.files.newUntitledFile",
        "workbench.action.files.openFile",
        "workbench.action.files.save",
        "workbench.action.files.saveAs",
        "workbench.action.closeActiveEditor",
        "workbench.action.closeWindow",
        "workbench.action.quit",
        "undo",
        "redo",
        "editor.action.clipboardCutAction",
        "editor.action.clipboardCopyAction",
        "editor.action.clipboardPasteAction",
        "editor.action.selectAll",
        "actions.find",
        "editor.action.startFindReplaceAction",
        "workbench.action.togglePanel",
        "workbench.action.toggleSidebarVisibility",
        "workbench.action.splitEditorRight",
        "workbench.action.splitEditorDown",
        "workbench.action.splitEditorLeft",
        "workbench.action.splitEditorUp",
        "workbench.action.focusNextGroup",
        "workbench.action.focusPreviousGroup",
        "workbench.action.showCommands",
    };
    std::vector<std::string> executed;
    const auto commands = window_->commands().commands();
    for (const auto& command : commands) {
        window_->commands().add(
            command.id, command.title,
            [&executed, id = command.id](const json&) { executed.push_back(id); });
    }

    for (const std::string& id : command_ids) {
        QAction* found = menu_action(id);
        ASSERT_NE(found, nullptr) << id;
        EXPECT_TRUE(found->isEnabled()) << id;
        executed.clear();
        found->trigger();
        EXPECT_EQ(executed, std::vector<std::string>{id}) << id;
    }
}

TEST_F(MainWindowTest, TheSplitCommandsSplitTheFocusedGroupInEveryDirection) {
    struct Case {
        const char* id;
        workbench::Orientation orientation;
        bool new_group_first;
    };
    const std::vector<Case> cases = {
        {"workbench.action.splitEditorRight", workbench::Orientation::horizontal, false},
        {"workbench.action.splitEditorDown", workbench::Orientation::vertical, false},
        {"workbench.action.splitEditorLeft", workbench::Orientation::horizontal, true},
        {"workbench.action.splitEditorUp", workbench::Orientation::vertical, true},
    };
    for (const Case& c : cases) {
        create_window();
        const workbench::GroupId original = window_->editor_layout().group_ids().front();
        window_->commands().execute(c.id);
        const auto ids = window_->editor_layout().group_ids();
        ASSERT_EQ(ids.size(), 2u) << c.id;
        EXPECT_EQ(window_->editor_layout().root().orientation, c.orientation) << c.id;
        EXPECT_EQ(ids[c.new_group_first ? 1 : 0], original) << c.id;
    }
}

TEST_F(MainWindowTest, NewUntitledFileOpensAnotherUntitledDocument) {
    create_window();
    const std::size_t before = window_->open_document_count();
    window_->commands().execute("workbench.action.files.newUntitledFile");
    EXPECT_EQ(window_->open_document_count(), before + 1);
    EXPECT_TRUE(window_->current_document()->text().empty());
}

TEST_F(MainWindowTest, CloseWindowClosesTheWindow) {
    create_window();
    window_->show();
    ASSERT_TRUE(window_->isVisible());
    window_->commands().execute("workbench.action.closeWindow");
    EXPECT_FALSE(window_->isVisible());
}

TEST_F(MainWindowTest, TheMenuBarHasVSCodesMenus) {
    create_window();
    QStringList titles;
    for (const QAction* top : window_->menuBar()->actions()) {
        titles << top->text();
    }
    EXPECT_EQ(titles, (QStringList{"&File", "&Edit", "&Selection", "&View", "&Go", "Run",
                                   "&Terminal", "&Help"}));
}

TEST_F(MainWindowTest, AnItemWhoseCommandDoesNotExistIsShownDisabled) {
    create_window();
    ASSERT_EQ(window_->commands().find("workbench.action.debug.start"), nullptr);
    QAction* start = menu_action("workbench.action.debug.start");
    ASSERT_NE(start, nullptr);
    EXPECT_FALSE(start->isEnabled());
    EXPECT_EQ(start->text(), "&Start Debugging");
    EXPECT_TRUE(start->toolTip().contains("not available yet")) << start->toolTip().toStdString();
    EXPECT_TRUE(menu_action("workbench.action.files.save")->isEnabled());
}

TEST_F(MainWindowTest, RegisteringACommandEnablesItsMenuItem) {
    create_window();
    ASSERT_FALSE(menu_action("workbench.action.debug.start")->isEnabled());

    int runs = 0;
    window_->commands().add("workbench.action.debug.start", "Start Debugging",
                            [&runs](const json&) { ++runs; });
    QAction* start = menu_action("workbench.action.debug.start");
    ASSERT_NE(start, nullptr);
    EXPECT_TRUE(start->isEnabled());
    start->trigger();
    EXPECT_EQ(runs, 1);
}

TEST_F(MainWindowTest, SubmenusHoldTheirItems) {
    create_window();
    QAction* reopen = menu_action("workbench.action.reopenClosedEditor");
    ASSERT_NE(reopen, nullptr);
    const auto* recent = qobject_cast<QMenu*>(reopen->associatedObjects().value(0));
    ASSERT_NE(recent, nullptr);
    EXPECT_EQ(recent->menuAction()->text(), "Open &Recent");
}

TEST_F(MainWindowTest, MenuEntriesAreHiddenByTheirWhenClauses) {
    create_window();
    EXPECT_EQ(menu_action("workbench.action.closeFolder"), nullptr);
    EXPECT_EQ(menu_action("workbench.action.toggleConfirmBeforeClose"), nullptr);
    EXPECT_NE(menu_action("workbench.action.files.openFolder"), nullptr);
}

TEST_F(MainWindowTest, ExtensionsCanRunEditorCommandsByTheirId) {
    create_window(true);
    if (!window_) {
        return;
    }
    const auto ids = window_->bridge().host()->commands().command_ids();
    EXPECT_NE(std::find(ids.begin(), ids.end(), "undo"), ids.end());

    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "abc").string())));
    type("X");
    ASSERT_EQ(current_text(), "Xabc");
    EXPECT_EQ(run("anvil.docs.execute", json::array({"undo"})), nullptr);
    EXPECT_EQ(current_text(), "abc");

    run("anvil.docs.execute", json::array({"cursorRight", "ignored", 1}));
    EXPECT_EQ(window_->current_document()->selections().primary(), (core::Selection{1, 1}));
}

// --- command palette ---

TEST_F(MainWindowTest, TheCommandPaletteListsTheEditorsCommandsByTitle) {
    create_window();
    EXPECT_TRUE(window_->command_palette()->isHidden());
    window_->commands().execute("workbench.action.showCommands");
    ui::QuickPick* palette = window_->command_palette();
    EXPECT_TRUE(palette->isVisible());
    EXPECT_EQ(palette->filter_field()->text(), ">");
    // `setContext` is the commands of extensions' business, and selecting the matches of the find
    // bar is its own key, so neither is on offer.
    EXPECT_EQ(palette->shown_items().size(), window_->commands().commands().size() - 2);

    std::vector<QString> titles;
    for (std::size_t i = 0; i < palette->shown_items().size(); ++i) {
        titles.push_back(palette->list()->model()->index(static_cast<int>(i), 0).data().toString());
    }
    EXPECT_NE(std::find(titles.begin(), titles.end(), "Undo"), titles.end());
    EXPECT_NE(std::find(titles.begin(), titles.end(), "Show All Commands"), titles.end());
    EXPECT_TRUE(
        std::is_sorted(titles.begin(), titles.end(), [](const QString& a, const QString& b) {
            return a.compare(b, Qt::CaseInsensitive) < 0;
        }));
}

TEST_F(MainWindowTest, TypingFiltersThePaletteAndEnterRunsTheCommand) {
    create_window();
    type("ab");
    ASSERT_EQ(current_text(), "ab");
    window_->commands().execute("workbench.action.showCommands");
    ui::QuickPick* palette = window_->command_palette();
    QTest::keyClicks(palette->filter_field(), "undo");
    ASSERT_FALSE(palette->shown_items().empty());
    EXPECT_EQ(palette->list()->model()->index(0, 0).data().toString(), "Undo");

    QTest::keyClick(palette->filter_field(), Qt::Key_Return);
    EXPECT_TRUE(palette->isHidden());
    EXPECT_EQ(current_text(), "");
}

TEST_F(MainWindowTest, TheShortcutsAndTheMenuOpenThePalette) {
    create_window();
    window_->activateWindow();
    EXPECT_TRUE(QTest::qWaitForWindowActive(window_.get()));
    window_->view()->setFocus();

    QTest::keyClick(window_->view(), Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_TRUE(window_->command_palette()->isVisible());
    QTest::keyClick(window_->command_palette()->filter_field(), Qt::Key_Escape);
    EXPECT_TRUE(window_->command_palette()->isHidden());

    QTest::keyClick(window_->view(), Qt::Key_F1);
    EXPECT_TRUE(window_->command_palette()->isVisible());
    QTest::keyClick(window_->command_palette()->filter_field(), Qt::Key_Escape);

    QAction* menu_item = menu_action("workbench.action.showCommands");
    ASSERT_NE(menu_item, nullptr);
    menu_item->trigger();
    EXPECT_TRUE(window_->command_palette()->isVisible());
}

TEST_F(MainWindowTest, EscapeClosesThePaletteAndTheEditorKeepsTheFocus) {
    create_window();
    window_->activateWindow();
    EXPECT_TRUE(QTest::qWaitForWindowActive(window_.get()));
    window_->view()->setFocus();
    ASSERT_TRUE(window_->view()->hasFocus());

    window_->commands().execute("workbench.action.showCommands");
    EXPECT_FALSE(window_->view()->hasFocus());
    QTest::keyClick(window_->command_palette()->filter_field(), Qt::Key_Escape);
    EXPECT_TRUE(window_->view()->hasFocus());
}

TEST_F(MainWindowTest, RecentlyUsedCommandsComeFirstInThePalette) {
    create_window();
    const auto first_title = [&] {
        return window_->command_palette()->list()->model()->index(0, 0).data().toString();
    };
    window_->commands().execute("workbench.action.showCommands");
    EXPECT_EQ(first_title(), "Add Selection to Next Find Match");
    QTest::keyClicks(window_->command_palette()->filter_field(), "select all");
    QTest::keyClick(window_->command_palette()->filter_field(), Qt::Key_Return);

    window_->commands().execute("workbench.action.showCommands");
    EXPECT_EQ(first_title(), "Select All");
    QTest::keyClicks(window_->command_palette()->filter_field(), "redo");
    QTest::keyClick(window_->command_palette()->filter_field(), Qt::Key_Return);

    window_->commands().execute("workbench.action.showCommands");
    EXPECT_EQ(first_title(), "Redo");
    EXPECT_EQ(window_->command_palette()->list()->model()->index(1, 0).data().toString(),
              "Select All");
}

TEST_F(MainWindowTest, TheCommandPaletteShowsAMessageWhenNoCommandMatches) {
    create_window();
    window_->commands().execute("workbench.action.showCommands");
    QTest::keyClicks(window_->command_palette()->filter_field(), "qqqqzz");
    EXPECT_TRUE(window_->command_palette()->shown_items().empty());
    bool shown = false;
    for (const QLabel* label : window_->command_palette()->findChildren<QLabel*>()) {
        shown = shown || (label->text() == "No matching commands" && label->isVisible());
    }
    EXPECT_TRUE(shown);
}

TEST_F(MainWindowTest, ExtensionCommandsAreInThePaletteByTitleAndRunFromIt) {
    create_window(true);
    if (!window_) {
        return;
    }
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "ab\ncd").string())));
    window_->commands().execute("workbench.action.showCommands");
    ui::QuickPick* palette = window_->command_palette();
    QTest::keyClicks(palette->filter_field(), "Anvil: Replace Two");
    ASSERT_FALSE(palette->shown_items().empty());
    EXPECT_EQ(palette->list()->model()->index(0, 0).data().toString(), "Anvil: Replace Two");

    QTest::keyClick(palette->filter_field(), Qt::Key_Return);
    ASSERT_TRUE(wait_until([&] { return current_text() == "Xb\nd"; })) << current_text();
}

TEST_F(MainWindowTest, AFailingCommandRunFromThePaletteIsReportedInTheStatusBar) {
    create_window(true);
    if (!window_) {
        return;
    }
    window_->commands().execute("workbench.action.showCommands");
    ui::QuickPick* palette = window_->command_palette();
    QTest::keyClicks(palette->filter_field(), "Anvil: Open");
    ASSERT_FALSE(palette->shown_items().empty());
    ASSERT_EQ(palette->list()->model()->index(0, 0).data().toString(), "Anvil: Open");
    QTest::keyClick(palette->filter_field(), Qt::Key_Return);
    // The command needs a path it is not given, so it throws in the extension host.
    EXPECT_TRUE(wait_until([&] {
        return window_->status_bar().message().contains("anvil.docs.open failed");
    })) << window_->status_bar().message().toStdString();
}

TEST_F(MainWindowTest, NoCommandPaletteOpensOrRunsWhileADialogIsOpen) {
    create_window();
    type("ab");
    QMessageBox box(window_.get());
    bool checked = false;
    QTimer::singleShot(50, &box, [&] {
        window_->commands().execute("workbench.action.showCommands");
        EXPECT_TRUE(window_->command_palette()->isHidden());
        checked = true;
        box.accept();
    });
    box.exec();
    ASSERT_TRUE(checked);

    // A choice made while a dialog is open does not run.
    window_->commands().execute("workbench.action.showCommands");
    QTest::keyClicks(window_->command_palette()->filter_field(), "undo");
    QMessageBox second(window_.get());
    QTimer::singleShot(50, &second, [&] {
        QTest::keyClick(window_->command_palette()->filter_field(), Qt::Key_Return);
        second.accept();
    });
    second.exec();
    EXPECT_EQ(current_text(), "ab");
}

TEST_F(MainWindowTest, RendersTheCommandPalette) {
    create_window();
    ASSERT_TRUE(window_->open_file(
        QString::fromStdString(write("sample.txt", "hello, world\nsecond line\n").string())));
    window_->commands().execute("workbench.action.showCommands");
    QTest::keyClicks(window_->command_palette()->filter_field(), "se");
    QApplication::processEvents();
    const QImage image = window_->grab().toImage();
    EXPECT_GT(image.width(), 100);
    if (const char* directory = std::getenv("ANVIL_SCREENSHOT_DIR")) {
        image.save(QString::fromUtf8(directory) + "/command_palette.png");
    }
}

// --- colors ---

TEST_F(MainWindowTest, WithoutThemesTheWorkbenchColorsFollowTheWindowsPalette) {
    themes_directory_ = scratch_ / "no-themes";
    create_window();
    QPalette palette = window_->palette();
    EXPECT_EQ(window_->colors().color("statusBar.background"), palette.color(QPalette::Window));

    palette.setColor(QPalette::Window, QColor("#4466aa"));
    palette.setColor(QPalette::Base, QColor("#aa6644"));
    window_->setPalette(palette);
    EXPECT_EQ(window_->colors().color("statusBar.background"), QColor("#4466aa"));
    EXPECT_EQ(window_->colors().color("editor.background"), QColor("#aa6644"));
}

TEST_F(MainWindowTest, TheCommandPaletteTakesItsColorsFromTheWorkbench) {
    create_window();
    window_->commands().execute("workbench.action.showCommands");
    window_->colors().set_colors({{"quickInput.background", QColor("#204060")}});
    QApplication::processEvents();
    const QImage image = window_->command_palette()->grab().toImage();
    EXPECT_EQ(image.pixelColor(image.width() / 2, image.height() - 3), QColor("#204060"));
}

// --- workbench ---

TEST_F(MainWindowTest, TheWindowIsAWorkbenchWithTheEditorInItsEditorArea) {
    create_window();
    ASSERT_NE(window_->workbench(), nullptr);
    EXPECT_EQ(window_->centralWidget(), window_->workbench());
    EXPECT_TRUE(window_->workbench()->isAncestorOf(window_->view()));
    EXPECT_TRUE(window_->workbench()->isAncestorOf(window_->tab_strip()));
    EXPECT_TRUE(window_->workbench()->isAncestorOf(window_->find_bar()));

    EXPECT_FALSE(window_->workbench()->is_visible(ui::Part::sidebar));
    EXPECT_FALSE(window_->workbench()->is_visible(ui::Part::panel));
    EXPECT_FALSE(window_->workbench()->is_visible(ui::Part::activity_bar));
    EXPECT_TRUE(window_->workbench()->is_visible(ui::Part::status_bar));
    EXPECT_GT(window_->workbench()->size(ui::Part::status_bar), 0);
}

TEST_F(MainWindowTest, TheViewMenuTogglesTheSidebarAndThePanel) {
    create_window();
    QAction* sidebar = menu_action("workbench.action.toggleSidebarVisibility");
    ASSERT_NE(sidebar, nullptr);
    EXPECT_TRUE(sidebar->text().endsWith("\tCtrl+B")) << sidebar->text().toStdString();
    const auto* appearance = qobject_cast<QMenu*>(sidebar->associatedObjects().value(0));
    ASSERT_NE(appearance, nullptr);
    QAction* panel =
        find_command_action(const_cast<QMenu*>(appearance), "workbench.action.togglePanel");
    ASSERT_NE(panel, nullptr);
    EXPECT_TRUE(panel->text().endsWith("\tCtrl+J")) << panel->text().toStdString();

    const int editor_width = window_->view()->width();
    sidebar->trigger();
    QApplication::processEvents();
    EXPECT_TRUE(window_->workbench()->is_visible(ui::Part::sidebar));
    EXPECT_LT(window_->view()->width(), editor_width);
    panel->trigger();
    EXPECT_TRUE(window_->workbench()->is_visible(ui::Part::panel));

    sidebar->trigger();
    panel->trigger();
    QApplication::processEvents();
    EXPECT_FALSE(window_->workbench()->is_visible(ui::Part::sidebar));
    EXPECT_FALSE(window_->workbench()->is_visible(ui::Part::panel));
    EXPECT_EQ(window_->view()->width(), editor_width);
}

TEST_F(MainWindowTest, TheShortcutsAndTheCommandsToggleTheParts) {
    create_window();
    window_->activateWindow();
    EXPECT_TRUE(QTest::qWaitForWindowActive(window_.get()));
    window_->view()->setFocus();

    QTest::keyClick(window_->view(), Qt::Key_B, Qt::ControlModifier);
    EXPECT_TRUE(window_->workbench()->is_visible(ui::Part::sidebar));
    QTest::keyClick(window_->view(), Qt::Key_J, Qt::ControlModifier);
    EXPECT_TRUE(window_->workbench()->is_visible(ui::Part::panel));

    window_->commands().execute("workbench.action.toggleSidebarVisibility");
    window_->commands().execute("workbench.action.togglePanel");
    EXPECT_FALSE(window_->workbench()->is_visible(ui::Part::sidebar));
    EXPECT_FALSE(window_->workbench()->is_visible(ui::Part::panel));

    window_->commands().execute("workbench.action.toggleActivityBarVisibility");
    EXPECT_TRUE(window_->workbench()->is_visible(ui::Part::activity_bar));
}

TEST_F(MainWindowTest, TheWorkbenchPartsFollowTheWorkbenchColors) {
    create_window();
    window_->workbench()->set_visible(ui::Part::sidebar, true);
    window_->colors().set_colors({{"sideBar.background", QColor("#224466")}});
    QApplication::processEvents();
    QWidget* sidebar = window_->workbench()->sidebar();
    const QImage image = sidebar->grab().toImage();
    EXPECT_EQ(image.pixelColor(sidebar->width() / 2, sidebar->height() / 2), QColor("#224466"));
}

TEST_F(MainWindowTest, RendersTheWindowWithTheSidebarAndThePanelShown) {
    create_window();
    ASSERT_TRUE(window_->open_file(
        QString::fromStdString(write("sample.txt", "hello, world\nsecond line\n").string())));
    window_->workbench()->set_visible(ui::Part::activity_bar, true);
    window_->workbench()->set_visible(ui::Part::sidebar, true);
    window_->workbench()->set_visible(ui::Part::panel, true);
    QApplication::processEvents();
    const QImage image = window_->grab().toImage();
    EXPECT_GT(image.width(), 100);
    if (const char* directory = std::getenv("ANVIL_SCREENSHOT_DIR")) {
        image.save(QString::fromUtf8(directory) + "/main_window_workbench.png");
    }
}

// --- view containers ---

TEST_F(MainWindowTest, ARegisteredViewContainerShowsTheActivityBarAndOpensInTheSidebar) {
    create_window();
    EXPECT_FALSE(window_->workbench()->is_visible(ui::Part::activity_bar));

    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::black);
    int created = 0;
    ui::ViewContainer container;
    container.id = "workbench.view.explorer";
    container.title = "Explorer";
    container.icon = QIcon(pixmap);
    container.create = [&] {
        ++created;
        return new QLabel("files");
    };
    window_->view_containers().add(std::move(container));
    QApplication::processEvents();
    EXPECT_TRUE(window_->workbench()->is_visible(ui::Part::activity_bar));
    EXPECT_EQ(created, 0);

    const int editor_width = window_->view()->width();
    window_->sidebar()->activate("workbench.view.explorer");
    QApplication::processEvents();
    EXPECT_EQ(created, 1);
    EXPECT_TRUE(window_->workbench()->is_visible(ui::Part::sidebar));
    EXPECT_EQ(window_->sidebar()->title(), "EXPLORER");
    EXPECT_LT(window_->view()->width(), editor_width);

    window_->commands().execute("workbench.action.toggleSidebarVisibility");
    EXPECT_FALSE(window_->workbench()->is_visible(ui::Part::sidebar));
    window_->commands().execute("workbench.action.toggleSidebarVisibility");
    EXPECT_TRUE(window_->workbench()->is_visible(ui::Part::sidebar));
    EXPECT_EQ(window_->sidebar()->active_container(), "workbench.view.explorer");
    EXPECT_EQ(created, 1);
}

TEST_F(MainWindowTest, TheTabKeyInsertsATabWhileTheSidebarAndThePanelAreOpen) {
    create_window();
    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::black);
    ui::ViewContainer explorer;
    explorer.id = "workbench.view.explorer";
    explorer.title = "Explorer";
    explorer.icon = QIcon(pixmap);
    explorer.create = [] { return new QLabel("files"); };
    window_->view_containers().add(std::move(explorer));
    ui::ViewContainer output;
    output.id = "workbench.panel.output";
    output.title = "Output";
    output.location = ui::ViewContainerLocation::panel;
    output.create = [] { return new QLabel("log"); };
    window_->view_containers().add(std::move(output));
    window_->show();
    window_->sidebar()->activate("workbench.view.explorer");
    window_->commands().execute("workbench.action.togglePanel");
    QApplication::processEvents();
    ASSERT_TRUE(window_->workbench()->is_visible(ui::Part::sidebar));
    ASSERT_TRUE(window_->workbench()->is_visible(ui::Part::panel));

    window_->view()->setFocus();
    ASSERT_TRUE(window_->view()->hasFocus());
    QTest::keyClick(window_->view(), Qt::Key_Tab);
    EXPECT_NE(current_text(), "");
    EXPECT_TRUE(window_->view()->hasFocus());
    QTest::keyClick(window_->view(), Qt::Key_Backtab, Qt::ShiftModifier);
    EXPECT_TRUE(window_->view()->hasFocus());
}

TEST_F(MainWindowTest, APanelContainerOpensInThePanelAndTheCommandsMaximizeIt) {
    create_window();
    int created = 0;
    ui::ViewContainer container;
    container.id = "workbench.panel.output";
    container.title = "Output";
    container.location = ui::ViewContainerLocation::panel;
    container.create = [&] {
        ++created;
        return new QLabel("log");
    };
    window_->view_containers().add(std::move(container));
    QApplication::processEvents();
    EXPECT_FALSE(window_->workbench()->is_visible(ui::Part::panel));
    EXPECT_FALSE(window_->workbench()->is_visible(ui::Part::activity_bar));
    EXPECT_EQ(created, 0);

    window_->commands().execute("workbench.action.togglePanel");
    QApplication::processEvents();
    EXPECT_TRUE(window_->workbench()->is_visible(ui::Part::panel));
    EXPECT_EQ(window_->panel()->active_container(), "workbench.panel.output");
    EXPECT_EQ(created, 1);

    window_->commands().execute("workbench.action.toggleMaximizedPanel");
    QApplication::processEvents();
    EXPECT_TRUE(window_->workbench()->is_maximized(ui::Part::panel));
    EXPECT_FALSE(window_->view()->isVisible());
    window_->commands().execute("workbench.action.toggleMaximizedPanel");
    QApplication::processEvents();
    EXPECT_FALSE(window_->workbench()->is_maximized(ui::Part::panel));
    EXPECT_TRUE(window_->view()->isVisible());
}

// --- color themes ---

TEST_F(MainWindowTest, TheEditorStartsInVsCodesDefaultTheme) {
    create_window();
    EXPECT_EQ(window_->themes().current(), "Dark 2026");
    EXPECT_EQ(window_->colors().color("editor.background"), QColor("#121314"));
    window_->activateWindow();
    QApplication::processEvents();
    const QImage image = window_->view()->viewport()->grab().toImage();
    EXPECT_EQ(image.pixelColor(image.width() - 5, image.height() - 5), QColor("#121314"));
    if (const char* directory = std::getenv("ANVIL_SCREENSHOT_DIR")) {
        ASSERT_TRUE(window_->open_file(QString::fromStdString(
            write("sample.txt", "hello, world\nsecond line\nthird line\n").string())));
        window_->workbench()->set_visible(ui::Part::sidebar, true);
        window_->workbench()->set_visible(ui::Part::panel, true);
        window_->view()->select_range({3, 8});
        QApplication::processEvents();
        window_->grab().toImage().save(QString::fromUtf8(directory) + "/window_dark_2026.png");
    }
}

TEST_F(MainWindowTest, TheSettingChoosesTheThemeAtStartup) {
    settings_file_ = write("settings.json", R"j({"workbench.colorTheme": "Light 2026"})j");
    create_window();
    EXPECT_EQ(window_->themes().current(), "Light 2026");
    const QImage image = window_->view()->viewport()->grab().toImage();
    EXPECT_EQ(image.pixelColor(image.width() - 5, image.height() - 5), QColor("#FFFFFF"));
    if (const char* directory = std::getenv("ANVIL_SCREENSHOT_DIR")) {
        ASSERT_TRUE(window_->open_file(QString::fromStdString(
            write("sample.txt", "hello, world\nsecond line\nthird line\n").string())));
        window_->workbench()->set_visible(ui::Part::sidebar, true);
        window_->workbench()->set_visible(ui::Part::panel, true);
        window_->view()->select_range({3, 8});
        QApplication::processEvents();
        window_->grab().toImage().save(QString::fromUtf8(directory) + "/window_light_2026.png");
    }
}

TEST_F(MainWindowTest, ChangingTheSettingSwitchesTheThemeAndKeepsTheCaretAndTheScrollPosition) {
    settings_file_ = write("settings.json", R"j({"workbench.colorTheme": "Dark 2026"})j");
    create_window();
    std::string many;
    for (int i = 0; i < 200; ++i) {
        many += "line " + std::to_string(i) + "\n";
    }
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("many.txt", many).string())));
    window_->view()->select_range({40, 44});
    window_->view()->verticalScrollBar()->setValue(300);
    const auto selections = window_->current_document()->selections();
    const int scroll = window_->view()->verticalScrollBar()->value();
    ASSERT_GT(scroll, 0);

    write("settings.json", R"j({"workbench.colorTheme": "Light Modern"})j");
    ASSERT_TRUE(wait_until([&] { return window_->themes().current() == "Light Modern"; }));
    EXPECT_EQ(window_->colors().color("editor.background"), QColor("#FFFFFF"));
    EXPECT_EQ(window_->current_document()->selections(), selections);
    EXPECT_EQ(window_->view()->verticalScrollBar()->value(), scroll);
    QApplication::processEvents();
    const QImage image = window_->view()->viewport()->grab().toImage();
    EXPECT_EQ(image.pixelColor(image.width() - 5, image.height() - 5), QColor("#FFFFFF"));
}

TEST_F(MainWindowTest, AnUnknownThemeInTheSettingsKeepsTheThemeAndIsReportedOnce) {
    settings_file_ = write("settings.json", R"j({"workbench.colorTheme": "Dark Modern"})j");
    create_window();
    ASSERT_EQ(window_->themes().current(), "Dark Modern");
    QSignalSpy failures(&window_->themes(), &ui::ThemeStore::load_failed);

    write("settings.json", R"j({"workbench.colorTheme": "Solarized"})j");
    ASSERT_TRUE(wait_until([&] { return failures.count() == 1; }));
    EXPECT_EQ(window_->themes().current(), "Dark Modern");
    EXPECT_TRUE(window_->status_bar().message().contains("Solarized"));

    window_->status_bar().clear_message();
    write("settings.json", R"j({"workbench.colorTheme": "Solarized", "editor.tabSize": 3})j");
    ASSERT_TRUE(wait_until([&] { return window_->settings().editor().tab_size == 3; }));
    EXPECT_EQ(failures.count(), 1);
}

TEST_F(MainWindowTest, AnUnknownThemeAtStartupFallsBackToTheDefaultAndIsReported) {
    settings_file_ = write("settings.json", R"j({"workbench.colorTheme": "Solarized"})j");
    create_window();
    EXPECT_EQ(window_->themes().current(), "Dark 2026");
    EXPECT_TRUE(window_->status_bar().message().contains("Solarized"));
}

TEST_F(MainWindowTest, TheEditorViewDrawsWithTheColorsOfTheTheme) {
    settings_file_ = write("settings.json", R"j({"workbench.colorTheme": "Dark Modern"})j");
    create_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "abc\ndef\n").string())));
    window_->view()->resize(500, 300);
    QApplication::processEvents();
    const QImage image = window_->view()->viewport()->grab().toImage();
    // Dark Modern: editor background #1F1F1F, gutter the same, and nothing of the palette's white.
    EXPECT_EQ(image.pixelColor(image.width() - 5, image.height() - 5), QColor("#1F1F1F"));
    EXPECT_EQ(image.pixelColor(2, image.height() - 5), QColor("#1F1F1F"));
}

// --- key bindings ---

class MainWindowKeysTest : public MainWindowTest {
protected:
    void create_active_window() {
        create_window();
        window_->activateWindow();
        ASSERT_TRUE(QTest::qWaitForWindowActive(window_.get()));
        window_->view()->setFocus();
        ASSERT_TRUE(window_->view()->hasFocus());
    }
};

TEST_F(MainWindowKeysTest, TypedCharactersAreNotTakenByTheBindings) {
    create_active_window();
    QTest::keyClicks(window_->view(), "abc ABC 123 {}[]\\/;'.,-=");
    EXPECT_EQ(current_text(), "abc ABC 123 {}[]\\/;'.,-=");
}

TEST_F(MainWindowKeysTest, TheEditingKeysRunTheirCommands) {
    create_active_window();
    type("abc");
    QTest::keyClick(window_->view(), Qt::Key_Left);
    QTest::keyClick(window_->view(), Qt::Key_Backspace);
    EXPECT_EQ(current_text(), "ac");
    QTest::keyClick(window_->view(), Qt::Key_Z, Qt::ControlModifier);
    EXPECT_EQ(current_text(), "abc");
    QTest::keyClick(window_->view(), Qt::Key_Y, Qt::ControlModifier);
    EXPECT_EQ(current_text(), "ac");
    QTest::keyClick(window_->view(), Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_EQ(current_text(), "ac");
    QTest::keyClick(window_->view(), Qt::Key_Delete);
    EXPECT_EQ(current_text(), "a");
}

TEST_F(MainWindowKeysTest, HomeAndTheCursorHomeCommandGoToTheFirstCharacterOfTheLine) {
    create_active_window();
    type("    abc");
    QTest::keyClick(window_->view(), Qt::Key_Home);
    EXPECT_EQ(window_->current_document()->selections().primary(), core::Selection::caret(4));
    QTest::keyClick(window_->view(), Qt::Key_Home);
    EXPECT_EQ(window_->current_document()->selections().primary(), core::Selection::caret(0));
    window_->commands().execute("cursorHome");
    EXPECT_EQ(window_->current_document()->selections().primary(), core::Selection::caret(4));
    window_->commands().execute("cursorEnd");
    QTest::keyClick(window_->view(), Qt::Key_Home, Qt::ShiftModifier);
    EXPECT_EQ(window_->current_document()->selections().primary(), (core::Selection{7, 4}));
}

TEST_F(MainWindowKeysTest, TheClipboardKeysCutCopyAndPaste) {
    create_active_window();
    type("hello");
    QTest::keyClick(window_->view(), Qt::Key_A, Qt::ControlModifier);
    QTest::keyClick(window_->view(), Qt::Key_X, Qt::ControlModifier);
    EXPECT_EQ(current_text(), "");
    EXPECT_EQ(QGuiApplication::clipboard()->text(), "hello");
    QTest::keyClick(window_->view(), Qt::Key_V, Qt::ControlModifier);
    QTest::keyClick(window_->view(), Qt::Key_V, Qt::ControlModifier);
    EXPECT_EQ(current_text(), "hellohello");
}

TEST_F(MainWindowKeysTest, ControlSSavesAndControlWClosesTheEditor) {
    create_active_window();
    const auto path = write("a.txt", "abc");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(path.string())));
    type("X");
    QTest::keyClick(window_->view(), Qt::Key_S, Qt::ControlModifier);
    EXPECT_EQ(*platform::read_file(path), "Xabc");

    ASSERT_EQ(window_->open_document_count(), 2U);
    QTest::keyClick(window_->view(), Qt::Key_W, Qt::ControlModifier);
    EXPECT_EQ(window_->open_document_count(), 1U);
}

TEST_F(MainWindowKeysTest, ControlWClosesAnEmptyGroupAndNotTheLastOne) {
    create_active_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    window_->commands().execute("workbench.action.splitEditor");
    QApplication::processEvents();
    ASSERT_EQ(window_->editor_area()->groups().size(), 2U);
    ASSERT_TRUE(window_->editor_area()->groups()[1]->is_empty());

    QTest::keyClick(window_->focusWidget() ? window_->focusWidget() : window_.get(), Qt::Key_W,
                    Qt::ControlModifier);
    QApplication::processEvents();
    EXPECT_EQ(window_->editor_area()->groups().size(), 1U);
    EXPECT_EQ(window_->open_document_count(), 2U);
}

TEST_F(MainWindowKeysTest, ControlWInAGroupWithTabsClosesTheTabEvenWithSeveralGroups) {
    create_active_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    window_->commands().execute("workbench.action.splitEditor");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("b.txt", "B").string())));
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("c.txt", "C").string())));
    QApplication::processEvents();
    ASSERT_EQ(window_->editor_area()->groups().size(), 2U);

    QTest::keyClick(window_->view(), Qt::Key_W, Qt::ControlModifier);
    QApplication::processEvents();
    EXPECT_EQ(window_->editor_area()->groups().size(), 2U);
    EXPECT_EQ(current_text(), "B");
}

TEST_F(MainWindowKeysTest, TheFindFieldKeepsItsOwnKeys) {
    create_active_window();
    type("abc");
    QTest::keyClick(window_->view(), Qt::Key_F, Qt::ControlModifier);
    ASSERT_FALSE(window_->find_bar()->isHidden());
    QLineEdit* field = window_->find_bar()->find_field();
    ASSERT_TRUE(field->hasFocus());

    QTest::keyClicks(field, "xyz");
    EXPECT_EQ(field->text(), "xyz");
    QTest::keyClick(field, Qt::Key_Left);
    QTest::keyClick(field, Qt::Key_Backspace);
    EXPECT_EQ(field->text(), "xz");
    QTest::keyClick(field, Qt::Key_A, Qt::ControlModifier);
    EXPECT_EQ(field->selectedText(), "xz");
    EXPECT_EQ(current_text(), "abc");
    EXPECT_TRUE(window_->current_document()->selections().primary().empty());
}

TEST_F(MainWindowKeysTest, FindNextAndPreviousWorkFromTheFindField) {
    create_active_window();
    type("a b a b");
    QTest::keyClick(window_->view(), Qt::Key_F, Qt::ControlModifier);
    window_->find_bar()->find_field()->clear();
    window_->find_bar()->find_field()->setText("b");
    ASSERT_EQ(window_->current_document()->selections().primary(), (core::Selection{2, 3}));
    QTest::keyClick(window_->find_bar()->find_field(), Qt::Key_F3);
    EXPECT_EQ(window_->current_document()->selections().primary(), (core::Selection{6, 7}));
    QTest::keyClick(window_->find_bar()->find_field(), Qt::Key_F3, Qt::ShiftModifier);
    EXPECT_EQ(window_->current_document()->selections().primary(), (core::Selection{2, 3}));
}

TEST_F(MainWindowKeysTest, ControlHOpensFindAndReplace) {
    create_active_window();
    QTest::keyClick(window_->view(), Qt::Key_H, Qt::ControlModifier);
    EXPECT_FALSE(window_->find_bar()->isHidden());
    EXPECT_TRUE(window_->find_bar()->replace_field()->isVisibleTo(window_->find_bar()));
}

TEST_F(MainWindowKeysTest, NoBindingActsWhileADialogIsOpen) {
    create_active_window();
    type("ab");
    QMessageBox box(window_.get());
    bool checked = false;
    QTimer::singleShot(50, &box, [&] {
        QTest::keyClick(&box, Qt::Key_Z, Qt::ControlModifier);
        QTest::keyClick(&box, Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
        checked = true;
        box.accept();
    });
    box.exec();
    ASSERT_TRUE(checked);
    EXPECT_EQ(current_text(), "ab");
    EXPECT_TRUE(window_->command_palette()->isHidden());
}

TEST_F(MainWindowKeysTest, TheSetContextCommandSetsAKeyThatBindingsRead) {
    create_active_window();
    auto binding = [](std::string_view keys, std::string command, std::string_view when) {
        core::Keybinding result;
        result.chord = *core::parse_key_chord(keys);
        result.command = std::move(command);
        result.when = *core::WhenExpression::parse(when);
        return result;
    };
    window_->keybindings().set_bindings(
        {binding("ctrl+j", "workbench.action.togglePanel", "myExt.armed")});

    QTest::keyClick(window_->view(), Qt::Key_J, Qt::ControlModifier);
    EXPECT_FALSE(window_->workbench()->is_visible(Part::panel));

    window_->commands().execute("setContext", json::array({"myExt.armed", true}));
    QTest::keyClick(window_->view(), Qt::Key_J, Qt::ControlModifier);
    EXPECT_TRUE(window_->workbench()->is_visible(Part::panel));

    window_->commands().execute("setContext", json::array({"myExt.armed", nullptr}));
    QTest::keyClick(window_->view(), Qt::Key_J, Qt::ControlModifier);
    EXPECT_TRUE(window_->workbench()->is_visible(Part::panel));
}

TEST_F(MainWindowKeysTest, AChordShowsItsProgressInTheStatusBar) {
    create_active_window();
    core::Keybinding chord;
    chord.chord = *core::parse_key_chord("ctrl+k ctrl+j");
    chord.command = "workbench.action.togglePanel";
    window_->keybindings().set_bindings({chord});

    QTest::keyClick(window_->view(), Qt::Key_K, Qt::ControlModifier);
    EXPECT_EQ(window_->status_bar().message(),
              "(Ctrl+K) was pressed. Waiting for second key of chord...");
    EXPECT_FALSE(window_->workbench()->is_visible(Part::panel));
    QApplication::processEvents();
    if (const char* directory = std::getenv("ANVIL_SCREENSHOT_DIR")) {
        window_->grab().save(QString::fromUtf8(directory) + "/chord_status.png");
    }

    QTest::keyClick(window_->view(), Qt::Key_J, Qt::ControlModifier);
    EXPECT_TRUE(window_->workbench()->is_visible(Part::panel));
    EXPECT_EQ(window_->status_bar().message(), "");

    QTest::keyClick(window_->view(), Qt::Key_K, Qt::ControlModifier);
    QTest::keyClick(window_->view(), Qt::Key_X, Qt::ControlModifier);
    EXPECT_EQ(window_->status_bar().message(),
              "The key combination (Ctrl+K, Ctrl+X) is not a command.");
}

TEST_F(MainWindowKeysTest, AnotherMessageSurvivesTheEndOfAChord) {
    create_active_window();
    core::Keybinding chord;
    chord.chord = *core::parse_key_chord("ctrl+k ctrl+j");
    chord.command = "workbench.action.togglePanel";
    window_->keybindings().set_bindings({chord});

    QTest::keyClick(window_->view(), Qt::Key_K, Qt::ControlModifier);
    window_->status_bar().show_message("Saved", 0);
    QTest::keyClick(window_->view(), Qt::Key_J, Qt::ControlModifier);
    EXPECT_EQ(window_->status_bar().message(), "Saved");
}

TEST_F(MainWindowKeysTest, TheMenusShowTheShortcutsOfTheirCommands) {
    create_window();
    const auto shows = [&](const std::string& command, const QString& shortcut) {
        QAction* action = menu_action(command);
        ASSERT_NE(action, nullptr) << command;
        EXPECT_TRUE(action->text().endsWith("\t" + shortcut))
            << command << ": " << action->text().toStdString();
    };
    shows("workbench.action.files.save", "Ctrl+S");
    shows("workbench.action.files.openFile", "Ctrl+O");
    shows("undo", "Ctrl+Z");
    shows("workbench.action.showCommands", "Ctrl+Shift+P");
}

TEST_F(MainWindowKeysTest, ARebindingChangesWhatTheMenuShows) {
    create_window();
    ASSERT_TRUE(menu_action("workbench.action.files.save")->text().endsWith("\tCtrl+S"));
    core::Keybinding rebound;
    rebound.chord = *core::parse_key_chord("ctrl+alt+s");
    rebound.command = "workbench.action.files.save";
    window_->keybindings().set_bindings({rebound});

    QAction* save = menu_action("workbench.action.files.save");
    ASSERT_NE(save, nullptr);
    EXPECT_TRUE(save->text().endsWith("\tCtrl+Alt+S")) << save->text().toStdString();
}

TEST_F(MainWindowKeysTest, AUserBindingOverridesADefaultOneAndTheMenuFollows) {
    keybindings_file_ = write(
        "keybindings.json", R"j([{"key": "ctrl+s", "command": "workbench.action.togglePanel"}])j");
    create_active_window();
    const auto path = write("a.txt", "abc");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(path.string())));
    ASSERT_FALSE(window_->workbench()->is_visible(Part::panel));
    type("X");

    QTest::keyClick(window_->view(), Qt::Key_S, Qt::ControlModifier);
    EXPECT_TRUE(window_->workbench()->is_visible(Part::panel));
    EXPECT_EQ(*platform::read_file(path), "abc");

    EXPECT_EQ(menu_action("workbench.action.files.save")->text(), "&Save");
    EXPECT_TRUE(menu_action("workbench.action.togglePanel")->text().endsWith("\tCtrl+S"))
        << menu_action("workbench.action.togglePanel")->text().toStdString();
}

TEST_F(MainWindowKeysTest, AUserRemovalTakesADefaultBindingAway) {
    keybindings_file_ =
        write("keybindings.json",
              R"j([{"key": "ctrl+b", "command": "-workbench.action.toggleSidebarVisibility"}])j");
    create_active_window();
    const bool shown = window_->workbench()->is_visible(Part::sidebar);

    QTest::keyClick(window_->view(), Qt::Key_B, Qt::ControlModifier);
    EXPECT_EQ(window_->workbench()->is_visible(Part::sidebar), shown);
    EXPECT_FALSE(menu_action("workbench.action.toggleSidebarVisibility")->text().contains('\t'));
}

TEST_F(MainWindowKeysTest, AUserBindingHasItsWhenAndItsArguments) {
    keybindings_file_ = write("keybindings.json",
                              R"j([{"key": "ctrl+alt+j", "command": "workbench.action.togglePanel",
              "when": "editorTextFocus"}])j");
    create_active_window();

    window_->find_bar()->show();
    window_->find_bar()->find_field()->setFocus();
    QTest::keyClick(window_->find_bar()->find_field(), Qt::Key_J,
                    Qt::ControlModifier | Qt::AltModifier);
    EXPECT_FALSE(window_->workbench()->is_visible(Part::panel));

    window_->view()->setFocus();
    QTest::keyClick(window_->view(), Qt::Key_J, Qt::ControlModifier | Qt::AltModifier);
    EXPECT_TRUE(window_->workbench()->is_visible(Part::panel));
}

TEST_F(MainWindowKeysTest, AFileThatAppearsLaterIsAppliedAndTheMenuFollows) {
    keybindings_file_ = scratch_ / "keybindings.json";
    create_active_window();
    QTest::keyClick(window_->view(), Qt::Key_Y, Qt::ControlModifier | Qt::AltModifier);
    EXPECT_FALSE(window_->workbench()->is_visible(Part::panel));

    write("keybindings.json",
          R"j([{"key": "ctrl+alt+y", "command": "workbench.action.togglePanel"}])j");
    ASSERT_TRUE(wait_until([&] {
        return menu_action("workbench.action.togglePanel")->text().endsWith("\tCtrl+Alt+Y");
    }));
    QTest::keyClick(window_->view(), Qt::Key_Y, Qt::ControlModifier | Qt::AltModifier);
    EXPECT_TRUE(window_->workbench()->is_visible(Part::panel));

    std::filesystem::remove(keybindings_file_);
    ASSERT_TRUE(wait_until(
        [&] { return menu_action("workbench.action.togglePanel")->text().endsWith("\tCtrl+J"); }));
}

TEST_F(MainWindowKeysTest, AFileWithASyntaxErrorKeepsTheBindingsAndSaysSo) {
    keybindings_file_ =
        write("keybindings.json",
              R"j([{"key": "ctrl+alt+y", "command": "workbench.action.togglePanel"}])j");
    create_active_window();

    write("keybindings.json", "[{");
    ASSERT_TRUE(wait_until(
        [&] { return window_->status_bar().message().startsWith("Key bindings not applied"); }));
    EXPECT_TRUE(window_->status_bar().message().contains("keybindings.json:"));
    QTest::keyClick(window_->view(), Qt::Key_Y, Qt::ControlModifier | Qt::AltModifier);
    EXPECT_TRUE(window_->workbench()->is_visible(Part::panel));
}

TEST_F(MainWindowKeysTest, AChordInProgressEndsWhenTheFileChanges) {
    keybindings_file_ =
        write("keybindings.json",
              R"j([{"key": "ctrl+k ctrl+u", "command": "workbench.action.togglePanel"}])j");
    create_active_window();

    QTest::keyClick(window_->view(), Qt::Key_K, Qt::ControlModifier);
    ASSERT_TRUE(window_->status_bar().message().startsWith("(Ctrl+K)"));
    write("keybindings.json",
          R"j([{"key": "ctrl+k ctrl+u", "command": "workbench.action.togglePanel"},
               {"key": "f9", "command": "workbench.action.togglePanel"}])j");
    ASSERT_TRUE(wait_until([&] { return window_->status_bar().message().isEmpty(); }));
    QTest::keyClick(window_->view(), Qt::Key_U, Qt::ControlModifier);
    EXPECT_FALSE(window_->workbench()->is_visible(Part::panel));
}

// --- several cursors ---

class MainWindowOccurrencesTest : public MainWindowKeysTest {
protected:
    void open_text(const std::string& text) {
        create_active_window();
        type(QString::fromStdString(text));
    }

    void put_caret(std::size_t offset) {
        ASSERT_TRUE(window_->current_document()
                        ->set_selections(core::SelectionSet::from({core::Selection::caret(offset)}))
                        .has_value());
        window_->view()->document_changed_elsewhere();
    }

    void ctrl(Qt::Key key, Qt::KeyboardModifiers more = Qt::NoModifier) {
        QTest::keyClick(window_->view(), key, Qt::ControlModifier | more);
    }

    std::vector<core::Selection> selections() const {
        return window_->current_document()->selections().all();
    }
};

TEST_F(MainWindowOccurrencesTest,
       ControlDSelectsTheWordThenAddsTheNextOccurrencesAndTypingRenamesThemAll) {
    open_text("foo bar foo baz foo");
    put_caret(1);
    ctrl(Qt::Key_D);
    EXPECT_EQ(selections(), (std::vector<core::Selection>{{0, 3}}));
    ctrl(Qt::Key_D);
    EXPECT_EQ(selections(), (std::vector<core::Selection>{{0, 3}, {8, 11}}));
    EXPECT_EQ(window_->current_document()->selections().primary(), (core::Selection{8, 11}));
    ctrl(Qt::Key_D);
    EXPECT_EQ(selections(), (std::vector<core::Selection>{{0, 3}, {8, 11}, {16, 19}}));

    type("qux");
    EXPECT_EQ(current_text(), "qux bar qux baz qux");
}

TEST_F(MainWindowOccurrencesTest, ControlDWrapsAroundAndStopsWhenEveryOccurrenceIsSelected) {
    open_text("foo bar foo");
    put_caret(9);
    ctrl(Qt::Key_D);
    ctrl(Qt::Key_D);
    EXPECT_EQ(selections(), (std::vector<core::Selection>{{0, 3}, {8, 11}}));
    EXPECT_EQ(window_->current_document()->selections().primary(), (core::Selection{0, 3}));
    ctrl(Qt::Key_D);
    EXPECT_EQ(selections(), (std::vector<core::Selection>{{0, 3}, {8, 11}}));
}

TEST_F(MainWindowOccurrencesTest, TheSearchOfALoneCaretKeepsMatchingWholeWordsAcrossPresses) {
    open_text("foo foobar foo");
    put_caret(1);
    ctrl(Qt::Key_D);
    ctrl(Qt::Key_D);
    EXPECT_EQ(selections(), (std::vector<core::Selection>{{0, 3}, {11, 14}}));
}

TEST_F(MainWindowOccurrencesTest, ASelectionSearchesItsTextWithTheFindBarsOptions) {
    open_text("foo foobar Foo foo");
    ctrl(Qt::Key_Home);
    QTest::keyClick(window_->view(), Qt::Key_Right, Qt::ShiftModifier);
    QTest::keyClick(window_->view(), Qt::Key_Right, Qt::ShiftModifier);
    QTest::keyClick(window_->view(), Qt::Key_Right, Qt::ShiftModifier);
    ASSERT_EQ(selections(), (std::vector<core::Selection>{{0, 3}}));
    ctrl(Qt::Key_D);
    EXPECT_EQ(window_->current_document()->selections().primary(), (core::Selection{4, 7}));

    put_caret(0);
    QTest::keyClick(window_->view(), Qt::Key_Right, Qt::ShiftModifier | Qt::ControlModifier);
    window_->commands().execute("actions.find");
    window_->find_bar()->match_case_button()->setChecked(true);
    window_->find_bar()->whole_word_button()->setChecked(true);
    window_->view()->setFocus();
    ASSERT_EQ(selections(), (std::vector<core::Selection>{{0, 3}}));
    ctrl(Qt::Key_D);
    EXPECT_EQ(window_->current_document()->selections().primary(), (core::Selection{15, 18}));
}

TEST_F(MainWindowOccurrencesTest, MovingTheCaretEndsTheSearch) {
    open_text("foo foobar foo");
    put_caret(1);
    ctrl(Qt::Key_D);
    QTest::keyClick(window_->view(), Qt::Key_Left);
    QTest::keyClick(window_->view(), Qt::Key_Left);
    ASSERT_EQ(selections(), (std::vector<core::Selection>{{0, 0}}));
    ctrl(Qt::Key_D);
    ctrl(Qt::Key_D);
    EXPECT_EQ(selections(), (std::vector<core::Selection>{{0, 3}, {11, 14}}));
}

TEST_F(MainWindowOccurrencesTest, LeavingTheEditorEndsTheSearch) {
    open_text("foo foobar foo");
    put_caret(1);
    ctrl(Qt::Key_D);
    window_->commands().execute("actions.find");
    window_->view()->setFocus();
    ASSERT_EQ(selections(), (std::vector<core::Selection>{{0, 3}}));
    // The selection is searched with the find bar's options now, not as a whole word.
    ctrl(Qt::Key_D);
    EXPECT_EQ(window_->current_document()->selections().primary(), (core::Selection{4, 7}));
}

TEST_F(MainWindowOccurrencesTest, ACaretOutsideAWordDoesNothing) {
    open_text("foo  bar");
    put_caret(4);
    ctrl(Qt::Key_D);
    EXPECT_EQ(selections(), (std::vector<core::Selection>{{4, 4}}));
}

TEST_F(MainWindowOccurrencesTest, SelectionsOfDifferentTextGrowToTheirWordsFirst) {
    open_text("foo bar foo bar");
    ASSERT_TRUE(window_->current_document()
                    ->set_selections(core::SelectionSet::from(
                        {core::Selection::caret(1), core::Selection::caret(5)}))
                    .has_value());
    window_->view()->document_changed_elsewhere();
    ctrl(Qt::Key_D);
    EXPECT_EQ(selections(), (std::vector<core::Selection>{{0, 3}, {4, 7}}));
    // Still two different texts: nothing to look for yet.
    ctrl(Qt::Key_D);
    EXPECT_EQ(selections(), (std::vector<core::Selection>{{0, 3}, {4, 7}}));
}

TEST_F(MainWindowOccurrencesTest, SeveralSelectionsOfTheSameTextSearchForIt) {
    open_text("foo bar foo foo");
    ASSERT_TRUE(window_->current_document()
                    ->set_selections(
                        core::SelectionSet::from({core::Selection{0, 3}, core::Selection{8, 11}}))
                    .has_value());
    window_->view()->document_changed_elsewhere();
    ctrl(Qt::Key_D);
    EXPECT_EQ(selections(), (std::vector<core::Selection>{{0, 3}, {8, 11}, {12, 15}}));
}

TEST_F(MainWindowOccurrencesTest, ControlShiftLSelectsEveryOccurrenceAtOnce) {
    open_text("foo bar foo foobar foo");
    put_caret(9);
    ctrl(Qt::Key_L, Qt::ShiftModifier);
    EXPECT_EQ(selections(), (std::vector<core::Selection>{{0, 3}, {8, 11}, {19, 22}}));
    EXPECT_EQ(window_->current_document()->selections().primary(), (core::Selection{8, 11}));
    type("x");
    EXPECT_EQ(current_text(), "x bar x foobar x");
}

TEST_F(MainWindowOccurrencesTest,
       AltEnterInTheFindBarSelectsEveryMatchOfItsPatternAndFocusesTheEditor) {
    open_text("a1 b22 c333");
    window_->commands().execute("actions.find");
    window_->find_bar()->regex_button()->setChecked(true);
    window_->find_bar()->find_field()->setText("\\d+");
    ASSERT_FALSE(window_->find_bar()->has_error());
    ASSERT_TRUE(window_->find_bar()->find_field()->hasFocus());

    QTest::keyClick(window_->find_bar()->find_field(), Qt::Key_Return, Qt::AltModifier);
    EXPECT_EQ(selections(), (std::vector<core::Selection>{{1, 2}, {4, 6}, {8, 11}}));
    EXPECT_TRUE(window_->view()->hasFocus());
    EXPECT_FALSE(window_->find_bar()->isHidden());
}

TEST_F(MainWindowOccurrencesTest, TheSelectAllMatchesCommandDoesNothingWhileTheFindBarIsClosed) {
    open_text("a1 b22");
    window_->commands().execute("editor.action.selectAllMatches");
    EXPECT_EQ(selections().size(), 1U);
}

TEST_F(MainWindowOccurrencesTest, ControlDInTheFindBarAddsTheNextMatchOfItsPattern) {
    open_text("one two one two one");
    put_caret(0);
    window_->commands().execute("actions.find");
    window_->find_bar()->find_field()->clear();
    window_->find_bar()->find_field()->setText("one");
    ASSERT_TRUE(window_->find_bar()->find_field()->hasFocus());
    ASSERT_EQ(selections(), (std::vector<core::Selection>{{0, 3}}));

    QTest::keyClick(window_->find_bar()->find_field(), Qt::Key_D, Qt::ControlModifier);
    EXPECT_EQ(selections(), (std::vector<core::Selection>{{0, 3}, {8, 11}}));
    EXPECT_EQ(window_->find_bar()->find_field()->text(), "one");
}

TEST_F(MainWindowOccurrencesTest, TheFindBarsPatternWinsOverTheSelectionWhileTheBarHasTheFocus) {
    open_text("one two one two one");
    window_->commands().execute("actions.find");
    window_->find_bar()->find_field()->clear();
    window_->find_bar()->find_field()->setText("two");
    ASSERT_TRUE(window_->find_bar()->find_field()->hasFocus());
    ASSERT_TRUE(window_->current_document()
                    ->set_selections(core::SelectionSet::from({core::Selection{8, 11}}))
                    .has_value());
    window_->view()->document_changed_elsewhere();

    QTest::keyClick(window_->find_bar()->find_field(), Qt::Key_D, Qt::ControlModifier);
    EXPECT_EQ(selections(), (std::vector<core::Selection>{{8, 11}, {12, 15}}));

    QTest::keyClick(window_->find_bar()->find_field(), Qt::Key_L,
                    Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_EQ(selections(), (std::vector<core::Selection>{{4, 7}, {12, 15}}));
}

TEST_F(MainWindowOccurrencesTest, SelectingMoreThanTheMatchLimitStopsAtItAndSaysSo) {
    open_text("");
    std::string text;
    for (int i = 0; i < 20100; ++i) {
        text += "a ";
    }
    const std::vector<core::Edit> insertion{{0, 0, text}};
    ASSERT_TRUE(window_->current_document()->apply_edits(insertion).has_value());
    window_->view()->document_changed_elsewhere();
    put_caret(0);

    ctrl(Qt::Key_L, Qt::ShiftModifier);
    EXPECT_EQ(selections().size(), 20000U);
    EXPECT_EQ(window_->status_bar().message(), "Only the first 20000 occurrences were selected");
}

TEST_F(MainWindowOccurrencesTest, RendersTheSelectedOccurrences) {
    open_text("");
    const std::vector<core::Edit> insertion{
        {0, 0, "let count = 1;\nlet total = count + count;\nreturn count;"}};
    ASSERT_TRUE(window_->current_document()->apply_edits(insertion).has_value());
    window_->view()->document_changed_elsewhere();
    put_caret(5);
    ctrl(Qt::Key_D);
    ctrl(Qt::Key_D);
    ctrl(Qt::Key_D);
    QApplication::processEvents();
    if (const char* directory = std::getenv("ANVIL_SCREENSHOT_DIR")) {
        window_->grab().save(QString::fromUtf8(directory) + "/occurrences.png");
    }
    EXPECT_EQ(selections().size(), 3U);
}

// --- extension contributions ---

class MainWindowContributionsTest : public MainWindowTest {
protected:
    void SetUp() override {
        MainWindowTest::SetUp();
        extension_dirs_ = {ANVIL_CONTRIBUTIONS_EXTENSION};
    }

    // The labels of the palette, in the order they are shown, after it was opened with `filter`.
    std::vector<QString> palette_labels(const QString& filter = {}) {
        window_->commands().execute("workbench.action.showCommands");
        ui::QuickPick* palette = window_->command_palette();
        if (!filter.isEmpty()) {
            QTest::keyClicks(palette->filter_field(), filter);
        }
        std::vector<QString> labels;
        for (int row = 0; row < palette->list()->model()->rowCount(); ++row) {
            labels.push_back(palette->list()->model()->index(row, 0).data().toString());
        }
        return labels;
    }

    bool shows(const std::vector<QString>& labels, const QString& label) {
        return std::ranges::find(labels, label) != labels.end();
    }
};

TEST_F(MainWindowContributionsTest, TheCommandsOfAnExtensionAreInThePaletteByCategoryAndTitle) {
    create_window(true);
    if (!window_) {
        return;
    }
    const auto labels = palette_labels();
    EXPECT_TRUE(shows(labels, "Fixtures: Lazy Command"));
    EXPECT_TRUE(shows(labels, "Keyed"));
    EXPECT_FALSE(shows(labels, "anvil.contrib.lazy"));
    // A command that is contributed twice is listed once, with the first title.
    EXPECT_FALSE(shows(labels, "Keyed Again"));
}

TEST_F(MainWindowContributionsTest, RendersTheDisabledAndTheEnabledCommandsOfAnExtension) {
    create_window(true);
    if (!window_) {
        return;
    }
    window_->commands().execute("workbench.action.showCommands");
    QTest::keyClicks(window_->command_palette()->filter_field(), "ed");
    QApplication::processEvents();
    if (const char* directory = std::getenv("ANVIL_SCREENSHOT_DIR")) {
        window_->grab().save(QString::fromUtf8(directory) + "/palette_contributions.png");
    }
    EXPECT_GE(window_->command_palette()->shown_items().size(), 2U);
}

TEST_F(MainWindowContributionsTest, ARuleOfTheCommandPaletteHidesACommandUntilItsWhenHolds) {
    create_window(true);
    if (!window_) {
        return;
    }
    EXPECT_FALSE(shows(palette_labels(), "Hidden"));
    window_->command_palette()->cancel();

    window_->commands().execute("setContext", json::array({"contrib.showHidden", true}));
    EXPECT_TRUE(shows(palette_labels(), "Hidden"));
    window_->command_palette()->cancel();

    window_->commands().execute("setContext", json::array({"contrib.showHidden", nullptr}));
    EXPECT_FALSE(shows(palette_labels(), "Hidden"));
}

TEST_F(MainWindowContributionsTest, ChoosingACommandActivatesItsExtensionAndRunsIt) {
    create_window(true);
    if (!window_) {
        return;
    }
    window_->commands().execute("workbench.action.showCommands");
    ui::QuickPick* palette = window_->command_palette();
    QTest::keyClicks(palette->filter_field(), "Fixtures: Lazy");
    ASSERT_FALSE(palette->shown_items().empty());
    QTest::keyClick(palette->filter_field(), Qt::Key_Return);
    ASSERT_TRUE(wait_until([&] { return current_text() == "[lazy]"; })) << current_text();
}

TEST_F(MainWindowContributionsTest, ACommandWhoseEnablementIsFalseIsDisabledAndDoesNotRun) {
    create_window(true);
    if (!window_) {
        return;
    }
    window_->commands().execute("workbench.action.showCommands");
    ui::QuickPick* palette = window_->command_palette();
    QTest::keyClicks(palette->filter_field(), "Gated");
    ASSERT_FALSE(palette->shown_items().empty());
    QTest::keyClick(palette->filter_field(), Qt::Key_Return);
    EXPECT_TRUE(palette->isVisible());
    QTest::qWait(300);
    EXPECT_EQ(current_text(), "");
    palette->cancel();

    window_->view()->setFocus();
    window_->commands().execute("anvil.contrib.gated");
    QTest::keyClick(window_->view(), Qt::Key_7, Qt::ControlModifier | Qt::AltModifier);
    QTest::qWait(300);
    EXPECT_EQ(current_text(), "");

    window_->commands().execute("setContext", json::array({"contrib.armed", true}));
    window_->commands().execute("workbench.action.showCommands");
    QTest::keyClicks(window_->command_palette()->filter_field(), "Gated");
    QTest::keyClick(window_->command_palette()->filter_field(), Qt::Key_Return);
    ASSERT_TRUE(wait_until([&] { return current_text() == "[gated]"; })) << current_text();
}

TEST_F(MainWindowContributionsTest, AnEnablementThatCannotBeReadDoesNotTakeTheCommandAway) {
    create_window(true);
    if (!window_) {
        return;
    }
    window_->commands().execute("workbench.action.showCommands");
    ui::QuickPick* palette = window_->command_palette();
    QTest::keyClicks(palette->filter_field(), "Typo");
    ASSERT_FALSE(palette->shown_items().empty());
    QTest::keyClick(palette->filter_field(), Qt::Key_Return);
    ASSERT_TRUE(wait_until([&] { return current_text() == "[typo]"; })) << current_text();
}

TEST_F(MainWindowContributionsTest, AKeyBindingOfTheExtensionRunsItsCommandWithThePlatformKey) {
    create_window(true);
    if (!window_) {
        return;
    }
    window_->activateWindow();
    ASSERT_TRUE(QTest::qWaitForWindowActive(window_.get()));
    window_->view()->setFocus();

    QTest::keyClick(window_->view(), Qt::Key_9, Qt::ControlModifier | Qt::AltModifier);
    QTest::qWait(300);
    EXPECT_EQ(current_text(), "");

    QTest::keyClick(window_->view(), Qt::Key_8, Qt::ControlModifier | Qt::AltModifier);
    ASSERT_TRUE(wait_until([&] { return current_text() == "[keyed]"; })) << current_text();
}

TEST_F(MainWindowContributionsTest, AKeyBindingOfTheExtensionKeepsItsWhen) {
    create_window(true);
    if (!window_) {
        return;
    }
    window_->activateWindow();
    ASSERT_TRUE(QTest::qWaitForWindowActive(window_.get()));
    window_->commands().execute("actions.find");
    ASSERT_TRUE(window_->find_bar()->find_field()->hasFocus());

    QTest::keyClick(window_->find_bar()->find_field(), Qt::Key_8,
                    Qt::ControlModifier | Qt::AltModifier);
    QTest::qWait(300);
    EXPECT_EQ(current_text(), "");
}

TEST_F(MainWindowContributionsTest, TheUsersBindingsWinOverThoseOfAnExtension) {
    keybindings_file_ =
        write("keybindings.json",
              R"j([{"key": "ctrl+alt+8", "command": "workbench.action.togglePanel"}])j");
    create_window(true);
    if (!window_) {
        return;
    }
    window_->activateWindow();
    ASSERT_TRUE(QTest::qWaitForWindowActive(window_.get()));
    window_->view()->setFocus();

    QTest::keyClick(window_->view(), Qt::Key_8, Qt::ControlModifier | Qt::AltModifier);
    EXPECT_TRUE(window_->workbench()->is_visible(Part::panel));
    QTest::qWait(300);
    EXPECT_EQ(current_text(), "");
}

TEST_F(MainWindowTest, ThemesOfExtensionsAreListedAfterTheBuiltInOnesAndTheSettingSelectsOne) {
    extension_dirs_ = {ANVIL_THEMES_EXTENSION};
    settings_file_ = write("settings.json", R"j({"workbench.colorTheme": "Fixture Light"})j");
    create_window(true);
    if (!window_) {
        return;
    }
    const auto& themes = window_->themes().themes();
    ASSERT_EQ(themes.size(), 13U);
    EXPECT_EQ(themes[10].id, "Fixture Light");
    EXPECT_EQ(themes[10].kind, core::ThemeKind::light);
    EXPECT_EQ(themes[11].id, "fixture-included");
    EXPECT_EQ(themes[11].label, "Fixture Included");
    EXPECT_EQ(themes[11].kind, core::ThemeKind::dark);
    EXPECT_EQ(themes[12].id, "Fixture Missing");
    EXPECT_FALSE(themes[12].available);

    EXPECT_EQ(window_->themes().current(), "Fixture Light");
    EXPECT_EQ(window_->colors().color("editor.background"), QColor("#fdf6e3"));
}

TEST_F(MainWindowTest, AContributedThemeCanIncludeAFileAndAMissingOneFailsWithoutChangingTheTheme) {
    extension_dirs_ = {ANVIL_THEMES_EXTENSION};
    create_window(true);
    if (!window_) {
        return;
    }
    ASSERT_TRUE(window_->themes().select("Fixture Included"));
    EXPECT_EQ(window_->colors().color("editor.background"), QColor("#001122"));
    EXPECT_EQ(window_->colors().color("editor.foreground"), QColor("#123456"));
    EXPECT_FALSE(window_->themes().select("Fixture Missing"));
    EXPECT_EQ(window_->themes().current(), "fixture-included");
}
