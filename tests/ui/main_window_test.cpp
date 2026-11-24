#include "anvil/platform/editor_defaults.hpp"
#include "anvil/platform/file.hpp"
#include "anvil/ui/main_window.hpp"
#include "anvil/ui/menu_style.hpp"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QFileDialog>
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
#include <QToolButton>
#include <chrono>
#include <fstream>
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
        options.executable = executable_;
        options.start_process = start_process_;
        options.custom_title_bar = custom_title_bar_;
        options.workspace = workspace_;
        options.choose_folder = choose_folder_;
        options.logger = logger_;
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

    // The title that the palette lists first when nothing was used yet: the one that sorts first
    // of the commands it shows.
    QString first_command_title() const {
        QString first;
        for (const auto& command : window_->commands().commands()) {
            if (command.id == "setContext" || command.id == "editor.action.selectAllMatches") {
                continue;
            }
            const QString title = QString::fromStdString(command.title);
            if (first.isEmpty() || title.compare(first, Qt::CaseInsensitive) < 0) {
                first = title;
            }
        }
        return first;
    }

    // Waits for the event loop, which services the extension host, until `done` holds.
    template <class Done> bool wait_until(Done done) {
        for (int i = 0; i < 3000 && !done(); ++i) {
            QTest::qWait(10);
        }
        return done();
    }

    // Waits until the extension host that was started last has finished starting.
    bool wait_host_ready() {
        return wait_until([&] {
                   return window_->bridge().host() &&
                          window_->bridge().host()->state() != exthost::HostState::starting;
               }) &&
               window_->bridge().host()->state() == exthost::HostState::ready;
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
        for (QAction* top : window_->menu_bar()->actions()) {
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
    std::optional<core::Workspace> workspace_;
    std::function<std::optional<std::filesystem::path>()> choose_folder_;
    std::shared_ptr<core::RecordingLogger> logger_ = std::make_shared<core::RecordingLogger>();
    std::filesystem::path settings_file_;
    std::filesystem::path keybindings_file_;
    // The tests keep the system frame except those about the title bar.
    bool custom_title_bar_ = false;
    std::vector<std::filesystem::path> extension_dirs_ = {ANVIL_DOCUMENTS_EXTENSION};
    // Unset: the themes that ship with the editor.
    std::optional<std::filesystem::path> themes_directory_;
    std::filesystem::path executable_;
    std::function<std::expected<std::int64_t, Error>(const platform::ProcessSpec&)> start_process_;
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

class AutoSaveTest : public MainWindowTest {
protected:
    // A window with the settings, showing a file that holds "abc" and has the focus.
    void open_with(std::string_view settings) {
        settings_file_ = write("settings.json", settings);
        create_window();
        path_ = write("a.txt", "abc");
        ASSERT_TRUE(window_->open_file(QString::fromStdString(path_.string())));
        window_->activateWindow();
        ASSERT_TRUE(QTest::qWaitForWindowActive(window_.get()));
        window_->view()->setFocus();
        QApplication::processEvents();
    }
    std::string on_disk() const {
        return *platform::read_file(path_);
    }
    // Moves the focus to a widget of the window that is not an editor.
    QLineEdit* focus_another_widget() {
        auto* edit = new QLineEdit(window_.get());
        edit->show();
        edit->setFocus();
        QApplication::processEvents();
        return edit;
    }
    // Makes another window the active one, which the window under test is told about.
    void deactivate_window() {
        other_ = std::make_unique<QWidget>();
        other_->show();
        other_->activateWindow();
        ASSERT_TRUE(QTest::qWaitForWindowActive(other_.get()));
        QApplication::processEvents();
    }

    std::filesystem::path path_;
    std::unique_ptr<QWidget> other_;
};

TEST_F(AutoSaveTest, NothingIsSavedByItselfWhileAutoSaveIsOff) {
    open_with(R"j({"files.autoSaveDelay": 20})j");
    type("X");
    QTest::qWait(150);
    focus_another_widget();
    ASSERT_NO_FATAL_FAILURE(deactivate_window());
    EXPECT_EQ(on_disk(), "abc");
    EXPECT_TRUE(window_->isWindowModified());
}

TEST_F(AutoSaveTest, AfterDelaySavesTheDocumentThatLongAfterItsLastChange) {
    open_with(R"j({"files.autoSave": "afterDelay", "files.autoSaveDelay": 50})j");
    type("X");
    EXPECT_EQ(on_disk(), "abc") << "not at once";
    EXPECT_TRUE(wait_until([&] { return on_disk() == "Xabc"; }));
    EXPECT_FALSE(window_->isWindowModified());
}

TEST_F(AutoSaveTest, EachChangePushesTheSaveAfterDelayOut) {
    open_with(R"j({"files.autoSave": "afterDelay", "files.autoSaveDelay": 400})j");
    type("X");
    QTest::qWait(250);
    type("Y");
    QTest::qWait(250);
    EXPECT_EQ(on_disk(), "abc") << "500 ms after the first change, 250 ms after the second";
    EXPECT_TRUE(wait_until([&] { return on_disk() == "XYabc"; }));
}

TEST_F(AutoSaveTest, AnUntitledDocumentIsNeverSavedAutomatically) {
    settings_file_ = write("settings.json", R"j({"files.autoSave": "afterDelay",
                                                "files.autoSaveDelay": 20})j");
    create_window();
    window_->commands().execute("workbench.action.files.newUntitledFile");
    type("X");
    QTest::qWait(150);
    EXPECT_TRUE(window_->isWindowModified());
    EXPECT_EQ(window_->current_document()->text().to_string(), "X");
}

TEST_F(AutoSaveTest, ADocumentClosedBeforeItsSaveIsNotWritten) {
    open_with(R"j({"files.autoSave": "afterDelay", "files.autoSaveDelay": 50})j");
    type("X");
    window_->commands().execute("workbench.action.closeActiveEditor");
    QTest::qWait(200);
    EXPECT_EQ(on_disk(), "abc");
}

TEST_F(AutoSaveTest, AFailedSaveIsShownInTheStatusBarAndLeavesTheChangesUnsaved) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "root can write to a read-only directory";
    }
    open_with(R"j({"files.autoSave": "afterDelay", "files.autoSaveDelay": 20})j");
    std::filesystem::permissions(path_, std::filesystem::perms::owner_read);
    std::filesystem::permissions(scratch_, std::filesystem::perms::owner_read |
                                               std::filesystem::perms::owner_exec);
    type("X");
    EXPECT_TRUE(
        wait_until([&] { return window_->status_bar().message().startsWith("Auto save failed"); }));
    EXPECT_TRUE(window_->isWindowModified());
    std::filesystem::permissions(scratch_, std::filesystem::perms::owner_all);
}

TEST_F(AutoSaveTest, OnFocusChangeSavesWhenTheEditorLosesTheFocus) {
    open_with(R"j({"files.autoSave": "onFocusChange"})j");
    type("X");
    QTest::qWait(150);
    EXPECT_EQ(on_disk(), "abc") << "typing does not save";
    focus_another_widget();
    EXPECT_EQ(on_disk(), "Xabc");
    EXPECT_FALSE(window_->isWindowModified());
}

TEST_F(AutoSaveTest, OnFocusChangeSavesTheDocumentThatAnotherTabReplaces) {
    open_with(R"j({"files.autoSave": "onFocusChange"})j");
    type("X");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("b.txt", "b").string())));
    EXPECT_EQ(on_disk(), "Xabc");
}

TEST_F(AutoSaveTest, OnFocusChangeSavesWhenTheWindowLosesTheFocusToo) {
    open_with(R"j({"files.autoSave": "onFocusChange"})j");
    type("X");
    ASSERT_NO_FATAL_FAILURE(deactivate_window());
    EXPECT_EQ(on_disk(), "Xabc");
}

TEST_F(AutoSaveTest, TheFindBarOfTheGroupIsPartOfTheEditor) {
    open_with(R"j({"files.autoSave": "onFocusChange"})j");
    type("X");
    window_->commands().execute("actions.find");
    QApplication::processEvents();
    ASSERT_NE(window_->view(), QApplication::focusWidget());
    EXPECT_EQ(on_disk(), "abc");
}

TEST_F(AutoSaveTest, OnWindowChangeSavesOnlyWhenTheWindowLosesTheFocus) {
    open_with(R"j({"files.autoSave": "onWindowChange"})j");
    type("X");
    focus_another_widget();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("b.txt", "b").string())));
    EXPECT_EQ(on_disk(), "abc");
    window_->open_file(QString::fromStdString(path_.string()));
    ASSERT_NO_FATAL_FAILURE(deactivate_window());
    EXPECT_EQ(on_disk(), "Xabc");
}

TEST_F(AutoSaveTest, TurningAutoSaveOnSavesTheDocumentsWithUnsavedChanges) {
    open_with("{}");
    type("X");
    ASSERT_EQ(on_disk(), "abc");
    window_->change_setting("files.autoSave", "onWindowChange");
    EXPECT_EQ(on_disk(), "Xabc");
}

TEST_F(AutoSaveTest, TheToggleCommandSwitchesBetweenOffAndAfterDelay) {
    open_with(R"j({"files.autoSave": "off"})j");
    window_->commands().execute("workbench.action.toggleAutoSave");
    EXPECT_EQ(window_->settings().editor().auto_save, core::AutoSave::after_delay);
    EXPECT_NE(platform::read_file(settings_file_)->find("\"afterDelay\""), std::string::npos);
    window_->commands().execute("workbench.action.toggleAutoSave");
    EXPECT_EQ(window_->settings().editor().auto_save, core::AutoSave::off);
}

TEST_F(AutoSaveTest, TheToggleCommandTurnsEveryOtherModeOff) {
    for (const char* mode : {"afterDelay", "onFocusChange", "onWindowChange"}) {
        settings_file_ =
            write("settings.json", std::string(R"j({"files.autoSave": ")j") + mode + R"j("})j");
        create_window();
        ASSERT_NE(window_->settings().editor().auto_save, core::AutoSave::off) << mode;
        window_->commands().execute("workbench.action.toggleAutoSave");
        EXPECT_EQ(window_->settings().editor().auto_save, core::AutoSave::off) << mode;
    }
}

class SaveAllTest : public MainWindowTest {
protected:
    // Answers the next file dialog the window opens: with `chosen` when it is set, and by
    // cancelling when not, after running `while_open` once with the dialog on screen. `selectFile`
    // leaves the name field alone while it has the keyboard focus, which depends on timing, so the
    // name is put in the field itself, and the answer is repeated until the dialog is gone.
    void answer_next_save_dialog(std::optional<std::filesystem::path> chosen,
                                 std::function<void()> while_open = {}) {
        dialog_shown_ = false;
        auto* timer = new QTimer(window_.get());
        timer->setInterval(5);
        QObject::connect(
            timer, &QTimer::timeout, timer,
            [this, timer, chosen = std::move(chosen), while_open = std::move(while_open)] {
                auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
                if (!dialog) {
                    if (dialog_shown_) {
                        timer->deleteLater();
                    }
                    return;
                }
                if (!dialog_shown_) {
                    dialog_shown_ = true;
                    if (while_open) {
                        while_open();
                    }
                }
                if (chosen) {
                    const QString path = QString::fromStdString(chosen->string());
                    dialog->selectFile(path);
                    if (auto* name = dialog->findChild<QLineEdit*>("fileNameEdit")) {
                        name->setText(path);
                    }
                    QMetaObject::invokeMethod(dialog, "accept");
                } else {
                    dialog->reject();
                }
            });
        timer->start();
    }

    std::string on_disk(const std::filesystem::path& path) const {
        return *platform::read_file(path);
    }
    // Opens the file and types at the start of it.
    void edit(const std::filesystem::path& path, const QString& typed) {
        ASSERT_TRUE(window_->open_file(QString::fromStdString(path.string())));
        type(typed);
    }

    bool dialog_shown_ = false;
};

TEST_F(SaveAllTest, WritesEveryDocumentWithUnsavedChangesAndNoOther) {
    create_window();
    const auto a = write("a.txt", "A");
    const auto b = write("b.txt", "B");
    const auto c = write("c.txt", "C");
    ASSERT_NO_FATAL_FAILURE(edit(a, "1"));
    ASSERT_NO_FATAL_FAILURE(edit(b, "2"));
    ASSERT_TRUE(window_->open_file(QString::fromStdString(c.string())));
    ASSERT_TRUE(window_->save_all());
    EXPECT_EQ(on_disk(a), "1A");
    EXPECT_EQ(on_disk(b), "2B");
    EXPECT_EQ(on_disk(c), "C");
    EXPECT_FALSE(window_->isWindowModified());
}

TEST_F(SaveAllTest, AsksForAnUntitledDocumentsNameWhenItsTurnComes) {
    create_window();
    const auto a = write("a.txt", "A");
    type("U");
    ASSERT_NO_FATAL_FAILURE(edit(a, "1"));
    const auto named = scratch_ / "named.txt";
    answer_next_save_dialog(named, [&] {
        EXPECT_EQ(on_disk(a), "A") << "the untitled document comes first in the tabs";
    });
    EXPECT_TRUE(window_->save_all());
    EXPECT_TRUE(dialog_shown_);
    EXPECT_EQ(on_disk(named), "U");
    EXPECT_EQ(on_disk(a), "1A");
}

TEST_F(SaveAllTest, StopsWhenTheUserCancelsTheQuestion) {
    create_window();
    const auto a = write("a.txt", "A");
    const auto b = write("b.txt", "B");
    ASSERT_NO_FATAL_FAILURE(edit(a, "1"));
    window_->commands().execute("workbench.action.files.newUntitledFile");
    type("U");
    ASSERT_NO_FATAL_FAILURE(edit(b, "2"));
    answer_next_save_dialog(std::nullopt);
    EXPECT_FALSE(window_->save_all());
    EXPECT_TRUE(dialog_shown_);
    EXPECT_EQ(on_disk(a), "1A") << "before the untitled document";
    EXPECT_EQ(on_disk(b), "B") << "after the untitled document";
}

TEST_F(SaveAllTest, FindsTheDocumentsAgainWhenTabsChangeWhileItAsks) {
    create_window();
    const auto a = write("a.txt", "A");
    const auto b = write("b.txt", "B");
    type("U");
    ASSERT_NO_FATAL_FAILURE(edit(a, "1"));
    ASSERT_NO_FATAL_FAILURE(edit(b, "2"));
    const auto named = scratch_ / "named.txt";
    // An extension closes a document with unsaved changes while the question is open.
    answer_next_save_dialog(named, [&] { window_->close_document(1); });
    EXPECT_TRUE(window_->save_all());
    EXPECT_EQ(on_disk(named), "U");
    EXPECT_EQ(on_disk(a), "A") << "closed before its turn";
    EXPECT_EQ(on_disk(b), "2B");
}

TEST_F(SaveAllTest, ABlockedWriteDoesNotStopTheOthersAndIsReported) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "root can write to a read-only file";
    }
    create_window();
    const auto a = write("a.txt", "A");
    const auto b = write("b.txt", "B");
    ASSERT_NO_FATAL_FAILURE(edit(a, "1"));
    ASSERT_NO_FATAL_FAILURE(edit(b, "2"));
    std::filesystem::permissions(scratch_, std::filesystem::perms::owner_read |
                                               std::filesystem::perms::owner_exec);
    int prompts = 0;
    const std::function<void()> answer_again = [&] {
        ++prompts;
        answer_next_prompt(QMessageBox::Ok, answer_again);
    };
    answer_next_prompt(QMessageBox::Ok, answer_again);
    EXPECT_FALSE(window_->save_all());
    std::filesystem::permissions(scratch_, std::filesystem::perms::owner_all);
    EXPECT_EQ(prompts, 2) << "one for each of the two documents that could not be written";
}

TEST_F(MainWindowTest, RevertReplacesTheTextWithTheFileAndLeavesTheDocumentClean) {
    create_window();
    const auto path = write("a.txt", "one\ntwo\nthree");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(path.string())));
    type("X");
    ASSERT_TRUE(window_->isWindowModified());
    ASSERT_TRUE(platform::write_file(path, "new\ncontent\nhere\nnow").has_value());
    EXPECT_TRUE(window_->revert());
    EXPECT_EQ(current_text(), "new\ncontent\nhere\nnow");
    EXPECT_FALSE(window_->isWindowModified());
    EXPECT_FALSE(window_->current_document()->is_dirty());
}

TEST_F(MainWindowTest, RevertKeepsEachSelectionAtItsLineAndColumnClampedToTheNewText) {
    create_window();
    const auto path = write("a.txt", "one\ntwo\nthree");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(path.string())));
    ASSERT_TRUE(window_->current_document()
                    ->set_selections(core::SelectionSet::from({core::Selection{5, 10}}))
                    .has_value());
    ASSERT_TRUE(platform::write_file(path, "abcdef\nxy").has_value());
    EXPECT_TRUE(window_->revert());
    // Line 1, column 1 stays there; line 2, column 2 is past the end of the shorter text.
    EXPECT_EQ(window_->current_document()->selections().primary(), (core::Selection{8, 9}));

    ASSERT_TRUE(platform::write_file(path, "ab").has_value());
    EXPECT_TRUE(window_->revert());
    EXPECT_EQ(window_->current_document()->selections().primary(), (core::Selection{2, 2}));
}

TEST_F(MainWindowTest, RevertCanBeUndoneAndThenTheDocumentHasUnsavedChanges) {
    create_window();
    const auto path = write("a.txt", "old");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(path.string())));
    type("X");
    ASSERT_TRUE(window_->revert());
    EXPECT_EQ(current_text(), "old");
    window_->view()->undo();
    EXPECT_EQ(current_text(), "Xold");
    EXPECT_TRUE(window_->isWindowModified());
}

TEST_F(MainWindowTest, RevertDoesNothingForAnUntitledDocumentAndSaysWhenTheFileIsGone) {
    create_window();
    type("typed");
    EXPECT_FALSE(window_->revert());
    EXPECT_EQ(current_text(), "typed");
    EXPECT_TRUE(window_->isWindowModified());

    const auto path = write("a.txt", "A");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(path.string())));
    type("!");
    std::filesystem::remove(path);
    EXPECT_FALSE(window_->revert());
    EXPECT_EQ(current_text(), "!A");
    EXPECT_TRUE(window_->status_bar().message().startsWith("Revert failed"));
}

TEST_F(MainWindowTest, RevertTellsTheExtensionsAndTheDocumentIsSavedForThem) {
    create_window(true);
    if (!window_) {
        return;
    }
    const auto path = write("a.txt", "one");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(path.string())));
    type("X");
    ASSERT_TRUE(platform::write_file(path, "two").has_value());
    EXPECT_TRUE(window_->revert());
    EXPECT_EQ(current_text(), "two");
    EXPECT_FALSE(window_->current_document()->is_dirty());
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

TEST_F(MainWindowTest, TheStatusBarShowsThePositionThenTheLanguageThenTheExtensionHostState) {
    create_window();
    StatusBar& bar = window_->status_bar();
    const auto ids = bar.item_ids(StatusBarSide::right);
    ASSERT_EQ(ids.size(), 3U);
    EXPECT_EQ(bar.item(ids[0])->text, "Ln 1, Col 1");
    EXPECT_EQ(bar.item(ids[1])->text, "Plain Text");
    EXPECT_TRUE(bar.item(ids[2])->text.startsWith("Extension host"));
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
              first_command_title());
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

TEST_F(MainWindowTest, TheWindowTitleNamesTheWorkspaceFolder) {
    std::filesystem::create_directories(scratch_ / "project");
    workspace_ = *core::Workspace::open(scratch_ / "project");
    custom_title_bar_ = true;
    create_window();
    EXPECT_EQ(window_->windowTitle(), "Untitled-1 — project — Anvil[*]");

    const auto file = write("notes.txt", "x");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(file.string())));
    EXPECT_EQ(window_->windowTitle(), "notes.txt — project — Anvil[*]");

    window_->commands().execute("workbench.action.splitEditor");
    EXPECT_EQ(window_->windowTitle(), "project — Anvil");
    EXPECT_EQ(window_->title_bar()->command_center()->text(), "project");
}

TEST_F(MainWindowTest, ExtensionsSeeTheWorkspaceFolder) {
    std::filesystem::create_directories(scratch_ / "project");
    workspace_ = *core::Workspace::open(scratch_ / "project");
    extension_dirs_ = {ANVIL_WORKSPACE_EXTENSION};
    create_window(true);
    if (!window_) {
        return;
    }
    const json seen = run("anvil.workspace.report", json::array({"/elsewhere"}));
    EXPECT_EQ(seen["name"], "project");
    ASSERT_EQ(seen["folders"].size(), 1u);
    EXPECT_EQ(seen["folders"][0]["path"], workspace_->folder.generic_string());
}

TEST_F(MainWindowTest, OpeningAFolderRestartsTheExtensionHostWithIt) {
    std::filesystem::create_directories(scratch_ / "project");
    extension_dirs_ = {ANVIL_WORKSPACE_EXTENSION};
    create_window(true);
    if (!window_) {
        return;
    }
    const json before = run("anvil.workspace.report", json::array({"/x"}));
    EXPECT_TRUE(before["folders"].empty());
    const long old_pid = before["pid"].get<long>();

    ASSERT_TRUE(window_->open_folder(scratch_ / "project"));
    ASSERT_TRUE(wait_host_ready());

    ASSERT_TRUE(window_->workspace().has_value());
    EXPECT_EQ(window_->workspace()->name, "project");
    const json after = run("anvil.workspace.report", json::array({"/x"}));
    ASSERT_EQ(after["folders"].size(), 1u);
    EXPECT_EQ(after["folders"][0]["path"], window_->workspace()->folder.generic_string());
    EXPECT_EQ(after["name"], "project");
    EXPECT_NE(after["pid"].get<long>(), old_pid);
    EXPECT_NE(::kill(static_cast<pid_t>(old_pid), 0), 0) << "the old extension host still runs";
    EXPECT_TRUE(window_->bridge().host()->commands().extension_commands().contains(
        "anvil.workspace.report"));

    EXPECT_TRUE(window_->open_folder(scratch_ / "project"));
    EXPECT_EQ(run("anvil.workspace.report", json::array({"/x"}))["pid"].get<long>(),
              after["pid"].get<long>())
        << "opening the folder that is open restarted the host";
}

TEST_F(MainWindowTest, ClosingTheFolderRestartsTheExtensionHostWithoutOne) {
    std::filesystem::create_directories(scratch_ / "project");
    workspace_ = *core::Workspace::open(scratch_ / "project");
    extension_dirs_ = {ANVIL_WORKSPACE_EXTENSION};
    create_window(true);
    if (!window_) {
        return;
    }
    const long old_pid = run("anvil.workspace.report", json::array({"/x"}))["pid"].get<long>();

    window_->close_folder();
    ASSERT_TRUE(wait_host_ready());

    EXPECT_FALSE(window_->workspace().has_value());
    const json after = run("anvil.workspace.report", json::array({"/x"}));
    EXPECT_TRUE(after["folders"].empty());
    EXPECT_EQ(after["name"], nullptr);
    EXPECT_NE(after["pid"].get<long>(), old_pid);
    EXPECT_NE(::kill(static_cast<pid_t>(old_pid), 0), 0) << "the old extension host still runs";
}

TEST_F(MainWindowTest, ChangingTheFolderKeepsTheTabsAndDocumentsAndAsksNothing) {
    std::filesystem::create_directories(scratch_ / "project");
    extension_dirs_ = {ANVIL_WORKSPACE_EXTENSION};
    create_window(true, true);
    if (!window_) {
        return;
    }
    const auto saved = write("saved.txt", "on disk");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(saved.string())));
    type("unsaved edit");
    const std::string first_text = current_text();
    window_->commands().execute("workbench.action.splitEditor");
    window_->commands().execute("workbench.action.files.newUntitledFile");
    type("second group");
    const auto titles = tab_titles();
    const std::size_t documents = window_->open_document_count();
    const std::size_t groups = window_->editor_area()->groups().size();
    ASSERT_TRUE(window_->current_document()->is_dirty());

    prompt_shown_ = false;
    answer_next_prompt(QMessageBox::Cancel);
    ASSERT_TRUE(window_->open_folder(scratch_ / "project"));
    ASSERT_TRUE(wait_host_ready());

    EXPECT_FALSE(prompt_shown_);
    EXPECT_EQ(tab_titles(), titles);
    EXPECT_EQ(window_->open_document_count(), documents);
    EXPECT_EQ(window_->editor_area()->groups().size(), groups);
    EXPECT_EQ(current_text(), "second group");
    const json seen = run("anvil.workspace.report", json::array({"/x"}));
    std::set<std::string> texts;
    for (const json& document : seen["documents"]) {
        texts.insert(document["text"].get<std::string>());
    }
    EXPECT_TRUE(texts.contains("second group")) << seen["documents"].dump();
    EXPECT_TRUE(texts.contains(first_text)) << seen["documents"].dump();
    EXPECT_EQ(seen["activeEditor"], window_->current_uri().to_string());
}

TEST_F(MainWindowTest, ARequestInFlightWhenTheHostRestartsFailsWithAnErrorThatSaysSo) {
    std::filesystem::create_directories(scratch_ / "project");
    extension_dirs_ = {ANVIL_WORKSPACE_EXTENSION};
    create_window(true);
    if (!window_) {
        return;
    }
    std::optional<exthost::CallResult> result;
    window_->bridge().host()->commands().execute(
        "anvil.workspace.hang", "[]", [&](exthost::CallResult r) { result = std::move(r); });
    ASSERT_TRUE(
        wait_until([&] { return window_->bridge().host()->state() == exthost::HostState::ready; }));
    QTest::qWait(300);
    ASSERT_FALSE(result.has_value());

    ASSERT_TRUE(window_->open_folder(scratch_ / "project"));

    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(result->has_value());
    EXPECT_NE(result->error().find("restarted"), std::string::npos) << result->error();
}

TEST_F(MainWindowTest, OpenFolderAsksForAFolderAndCancellingChangesNothing) {
    std::filesystem::create_directories(scratch_ / "project");
    extension_dirs_ = {ANVIL_WORKSPACE_EXTENSION};
    int asked = 0;
    std::optional<std::filesystem::path> answer;
    choose_folder_ = [&] {
        ++asked;
        return answer;
    };
    create_window(true);
    if (!window_) {
        return;
    }
    const long pid = run("anvil.workspace.report", json::array({"/x"}))["pid"].get<long>();

    ASSERT_TRUE(window_->commands().execute("workbench.action.files.openFolder"));
    EXPECT_EQ(asked, 0) << "the command must return before the host it runs in can be replaced";
    ASSERT_TRUE(wait_until([&] { return asked == 1; }));
    QTest::qWait(100);
    EXPECT_FALSE(window_->workspace().has_value());
    EXPECT_EQ(run("anvil.workspace.report", json::array({"/x"}))["pid"].get<long>(), pid);

    answer = scratch_ / "project";
    ASSERT_TRUE(window_->commands().execute("workbench.action.files.openFolder"));
    ASSERT_TRUE(wait_until([&] { return window_->workspace().has_value(); }));
    ASSERT_TRUE(wait_host_ready());
    EXPECT_EQ(run("anvil.workspace.report", json::array({"/x"}))["folders"].size(), 1u);
}

TEST_F(MainWindowTest, OpenFolderAndCloseFolderAreInTheMenuAndFollowTheWorkspace) {
    std::filesystem::create_directories(scratch_ / "project");
    create_window();
    QAction* open = menu_action("workbench.action.files.openFolder");
    ASSERT_NE(open, nullptr);
    EXPECT_TRUE(open->isEnabled());
    EXPECT_EQ(menu_action("workbench.action.closeFolder"), nullptr);

    ASSERT_TRUE(window_->open_folder(scratch_ / "project"));
    QAction* close = menu_action("workbench.action.closeFolder");
    ASSERT_NE(close, nullptr);
    EXPECT_TRUE(close->isEnabled());
    EXPECT_EQ(window_->windowTitle(), "Untitled-1 — project — Anvil[*]");

    close->trigger();
    EXPECT_TRUE(window_->workspace().has_value())
        << "the command must return before the host it runs in can be replaced";
    ASSERT_TRUE(wait_until([&] { return !window_->workspace().has_value(); }));
    EXPECT_EQ(window_->windowTitle(), "Untitled-1 — Anvil[*]");
    EXPECT_EQ(menu_action("workbench.action.closeFolder"), nullptr);
}

TEST_F(MainWindowTest, OpeningAFileInsteadOfAFolderChangesNothing) {
    create_window();
    const auto file = write("a.txt", "x");
    EXPECT_FALSE(window_->open_folder(file));
    EXPECT_FALSE(window_->open_folder(scratch_ / "missing"));
    EXPECT_FALSE(window_->workspace().has_value());
}

TEST_F(MainWindowTest, AnExtensionHostThatStoppedStartsAgainWhenAFolderIsOpened) {
    std::filesystem::create_directories(scratch_ / "project");
    extension_dirs_ = {ANVIL_WORKSPACE_EXTENSION};
    create_window(true);
    if (!window_) {
        return;
    }
    window_->bridge().host()->commands().execute("anvil.workspace.die", "[]",
                                                 [](exthost::CallResult) {});
    ASSERT_TRUE(wait_until([&] { return !window_->bridge().running(); }));

    ASSERT_TRUE(window_->open_folder(scratch_ / "project"));
    ASSERT_TRUE(window_->bridge().running());
    ASSERT_TRUE(wait_host_ready());
    EXPECT_EQ(run("anvil.workspace.report", json::array({"/x"}))["folders"].size(), 1u);
}

TEST_F(MainWindowTest, OpeningAFolderWithoutAnExtensionHostShowsItsName) {
    std::filesystem::create_directories(scratch_ / "project");
    create_window();
    ASSERT_TRUE(window_->open_folder(scratch_ / "project"));
    EXPECT_EQ(window_->windowTitle(), "Untitled-1 — project — Anvil[*]");
    EXPECT_FALSE(window_->bridge().running());
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
    for (const QAction* menu : window_->menu_bar()->actions()) {
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

TEST_F(MainWindowTest, ChangeSettingEditsTheFileAndTheWindowFollowsAtOnce) {
    settings_file_ = write("settings.json", "{\n    // my size\n    \"editor.fontSize\": 14\n}\n");
    create_window();
    EXPECT_EQ(window_->view()->font().pixelSize(), 14);

    const auto result = window_->change_setting("editor.fontSize", 20, "Font Size");
    EXPECT_EQ(result.outcome, ui::SettingsStore::ChangeResult::Outcome::written);
    EXPECT_EQ(*platform::read_file(settings_file_),
              "{\n    // my size\n    \"editor.fontSize\": 20\n}\n");
    EXPECT_EQ(window_->view()->font().pixelSize(), 20);
    EXPECT_EQ(window_->status_bar().message(), "");
}

TEST_F(MainWindowTest, ChangeSettingSaysWhyWhenTheFileHasASyntaxError) {
    const std::string broken = "{\n    \"editor.tabSize\": 2,\n    \"a\": \n}\n";
    settings_file_ = write("settings.json", broken);
    create_window();

    const auto result = window_->change_setting("editor.fontSize", 22, "Font Size");
    EXPECT_EQ(result.outcome, ui::SettingsStore::ChangeResult::Outcome::session);
    EXPECT_EQ(*platform::read_file(settings_file_), broken);
    EXPECT_EQ(window_->view()->font().pixelSize(), 22);
    const QString message = window_->status_bar().message();
    EXPECT_TRUE(
        message.startsWith("Font Size is changed for this session only: settings.json: line 4"))
        << message.toStdString();
}

TEST_F(MainWindowTest, ChangeSettingNamesTheKeyWithoutADescription) {
    create_window();
    settings_file_.clear();
    const auto result = window_->change_setting("editor.fontSize", 22);
    EXPECT_EQ(result.outcome, ui::SettingsStore::ChangeResult::Outcome::session);
    EXPECT_EQ(window_->status_bar().message(),
              "editor.fontSize is changed for this session only: there is no settings file");
}

TEST_F(MainWindowTest, ChangeSettingRejectsAValueThatIsNotValid) {
    settings_file_ = write("settings.json", R"j({"editor.fontSize": 14})j");
    create_window();
    const auto result = window_->change_setting("editor.fontSize", "big", "Font Size");
    EXPECT_EQ(result.outcome, ui::SettingsStore::ChangeResult::Outcome::rejected);
    EXPECT_TRUE(window_->status_bar().message().startsWith("Font Size was not changed: "));
    EXPECT_EQ(window_->view()->font().pixelSize(), 14);
    EXPECT_EQ(*platform::read_file(settings_file_), R"j({"editor.fontSize": 14})j");
}

TEST_F(MainWindowTest, ChangeSettingDoesNotWriteOverASettingsFileOpenWithUnsavedChanges) {
    settings_file_ = write("settings.json", "{\n    \"editor.fontSize\": 14\n}\n");
    create_window();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(settings_file_.string())));
    type("// my settings");
    QTest::keyClick(window_->view(), Qt::Key_Return);
    ASSERT_TRUE(window_->current_document()->is_dirty());

    const auto result = window_->change_setting("editor.fontSize", 20, "Font Size");
    EXPECT_EQ(result.outcome, ui::SettingsStore::ChangeResult::Outcome::session);
    EXPECT_EQ(result.reason, "settings.json has unsaved changes in an editor");
    EXPECT_EQ(*platform::read_file(settings_file_), "{\n    \"editor.fontSize\": 14\n}\n");
    EXPECT_TRUE(window_->status_bar().message().contains("unsaved changes"));

    // Once the tab is saved, a change goes to the file again.
    ASSERT_TRUE(window_->save());
    EXPECT_EQ(window_->change_setting("editor.fontSize", 18).outcome,
              ui::SettingsStore::ChangeResult::Outcome::written);
    EXPECT_EQ(*platform::read_file(settings_file_),
              "// my settings\n{\n    \"editor.fontSize\": 18\n}\n");
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
                                          "workbench.action.editor.changeLanguageMode",
                                          "saveAll",
                                          "workbench.action.files.revert",
                                          "workbench.action.files.openFile",
                                          "workbench.action.toggleAutoSave",
                                          "workbench.action.editorLayoutSingle",
                                          "workbench.action.editorLayoutTwoColumns",
                                          "workbench.action.editorLayoutThreeColumns",
                                          "workbench.action.editorLayoutTwoRows",
                                          "workbench.action.editorLayoutThreeRows",
                                          "workbench.action.editorLayoutTwoByTwoGrid",
                                          "workbench.action.editorLayoutTwoRowsRight",
                                          "workbench.action.editorLayoutTwoColumnsBottom",
                                          "workbench.action.toggleEditorGroupLayout",
                                          "workbench.action.nextEditor",
                                          "workbench.action.previousEditor",
                                          "workbench.action.nextEditorInGroup",
                                          "workbench.action.previousEditorInGroup",
                                          "workbench.action.openNextRecentlyUsedEditor",
                                          "workbench.action.openPreviousRecentlyUsedEditor",
                                          "workbench.action.openNextRecentlyUsedEditorInGroup",
                                          "workbench.action.openPreviousRecentlyUsedEditorInGroup",
                                          "workbench.action.focusFirstEditorGroup",
                                          "workbench.action.focusSecondEditorGroup",
                                          "workbench.action.focusThirdEditorGroup",
                                          "workbench.action.focusFourthEditorGroup",
                                          "workbench.action.focusFifthEditorGroup",
                                          "workbench.action.focusSixthEditorGroup",
                                          "workbench.action.focusSeventhEditorGroup",
                                          "workbench.action.focusEighthEditorGroup",
                                          "workbench.action.focusLeftGroup",
                                          "workbench.action.focusRightGroup",
                                          "workbench.action.focusAboveGroup",
                                          "workbench.action.focusBelowGroup",
                                          "editor.action.insertCursorAbove",
                                          "editor.action.insertCursorBelow",
                                          "editor.action.insertCursorAtEndOfEachLineSelected",
                                          "editor.action.addSelectionToPreviousFindMatch",
                                          "editor.action.copyLinesUpAction",
                                          "editor.action.copyLinesDownAction",
                                          "editor.action.moveLinesUpAction",
                                          "editor.action.moveLinesDownAction",
                                          "editor.action.duplicateSelection",
                                          "workbench.action.toggleMultiCursorModifier",
                                          "editor.action.toggleColumnSelection",
                                          "editor.action.renderWhitespace.none",
                                          "editor.action.renderWhitespace.boundary",
                                          "editor.action.renderWhitespace.selection",
                                          "editor.action.renderWhitespace.trailing",
                                          "editor.action.renderWhitespace.all",
                                          "editor.action.toggleRenderControlCharacter",
                                          "workbench.action.closeActiveEditor",
                                          "actions.find",
                                          "editor.action.nextMatchFindAction",
                                          "editor.action.previousMatchFindAction",
                                          "editor.action.startFindReplaceAction",
                                          "workbench.action.showCommands",
                                          "workbench.action.toggleSidebarVisibility",
                                          "workbench.action.togglePanel",
                                          "workbench.action.toggleActivityBarVisibility",
                                          "workbench.action.navigateBack",
                                          "workbench.action.navigateForward",
                                          "workbench.action.navigateToLastEditLocation",
                                          "workbench.action.reopenClosedEditor",
                                          "workbench.action.selectTheme",
                                          "workbench.action.newWindow",
                                          "workbench.action.gotoLine",
                                          "editor.action.jumpToBracket",
                                          "workbench.action.openSettings",
                                          "workbench.action.openSettingsJson",
                                          "workbench.action.openGlobalKeybindings",
                                          "workbench.action.openGlobalKeybindingsFile",
                                          "workbench.action.activityBarLocation.default",
                                          "workbench.action.activityBarLocation.hide",
                                          "workbench.action.toggleStatusbarVisibility",
                                          "workbench.action.toggleSidebarPosition",
                                          "workbench.action.positionPanelTop",
                                          "workbench.action.positionPanelLeft",
                                          "workbench.action.positionPanelRight",
                                          "workbench.action.positionPanelBottom",
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
                                          "workbench.action.files.openFolder",
                                          "workbench.action.closeFolder",
                                          "workbench.action.quit",
                                          "workbench.action.closeWindow",
                                          "workbench.action.toggleFullScreen",
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
    window_->commands().execute("workbench.action.splitEditorRight");
    window_->commands().execute("workbench.action.focusPreviousGroup");
    // Save All needs a document with unsaved changes, and Revert File one with a file.
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    type("x");
    const std::vector<std::string> command_ids = {
        "workbench.action.files.newUntitledFile",
        "workbench.action.files.openFile",
        "workbench.action.files.save",
        "workbench.action.files.saveAs",
        "saveAll",
        "workbench.action.files.revert",
        "workbench.action.toggleAutoSave",
        "workbench.action.editorLayoutSingle",
        "workbench.action.editorLayoutTwoColumns",
        "workbench.action.editorLayoutThreeColumns",
        "workbench.action.editorLayoutTwoRows",
        "workbench.action.editorLayoutThreeRows",
        "workbench.action.editorLayoutTwoByTwoGrid",
        "workbench.action.editorLayoutTwoRowsRight",
        "workbench.action.editorLayoutTwoColumnsBottom",
        "workbench.action.toggleEditorGroupLayout",
        "workbench.action.nextEditor",
        "workbench.action.previousEditor",
        "workbench.action.nextEditorInGroup",
        "workbench.action.previousEditorInGroup",
        "workbench.action.openNextRecentlyUsedEditor",
        "workbench.action.openPreviousRecentlyUsedEditor",
        "workbench.action.openNextRecentlyUsedEditorInGroup",
        "workbench.action.openPreviousRecentlyUsedEditorInGroup",
        "workbench.action.focusFirstEditorGroup",
        "workbench.action.focusSecondEditorGroup",
        "workbench.action.focusThirdEditorGroup",
        "workbench.action.focusFourthEditorGroup",
        "workbench.action.focusFifthEditorGroup",
        "workbench.action.focusLeftGroup",
        "workbench.action.focusRightGroup",
        "workbench.action.focusAboveGroup",
        "workbench.action.focusBelowGroup",
        "editor.action.insertCursorAbove",
        "editor.action.insertCursorBelow",
        "editor.action.insertCursorAtEndOfEachLineSelected",
        "editor.action.addSelectionToPreviousFindMatch",
        "editor.action.copyLinesUpAction",
        "editor.action.copyLinesDownAction",
        "editor.action.moveLinesUpAction",
        "editor.action.moveLinesDownAction",
        "editor.action.duplicateSelection",
        "workbench.action.toggleMultiCursorModifier",
        "editor.action.toggleColumnSelection",
        "editor.action.renderWhitespace.none",
        "editor.action.renderWhitespace.boundary",
        "editor.action.renderWhitespace.selection",
        "editor.action.renderWhitespace.trailing",
        "editor.action.renderWhitespace.all",
        "editor.action.toggleRenderControlCharacter",
        "workbench.action.closeActiveEditor",
        "workbench.action.closeWindow",
        "workbench.action.quit",
        "workbench.action.toggleFullScreen",
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
        "workbench.action.toggleStatusbarVisibility",
        "workbench.action.toggleSidebarPosition",
        "workbench.action.gotoLine",
        "editor.action.jumpToBracket",
        "workbench.action.openSettings",
        "workbench.action.openGlobalKeybindings",
        "workbench.action.selectTheme",
        "workbench.action.newWindow",
        "workbench.action.activityBarLocation.default",
        "workbench.action.activityBarLocation.hide",
        "workbench.action.positionPanelTop",
        "workbench.action.positionPanelLeft",
        "workbench.action.positionPanelRight",
        "workbench.action.positionPanelBottom",
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

TEST_F(MainWindowTest, ToggleFullScreenEntersAndLeavesFullscreenAndKeepsAMaximizedWindow) {
    create_window();
    window_->showMaximized();
    QApplication::processEvents();
    ASSERT_TRUE(window_->isMaximized());

    window_->commands().execute("workbench.action.toggleFullScreen");
    QApplication::processEvents();
    EXPECT_TRUE(window_->isFullScreen());
    window_->commands().execute("workbench.action.toggleFullScreen");
    QApplication::processEvents();
    EXPECT_FALSE(window_->isFullScreen());
    EXPECT_TRUE(window_->isMaximized());
}

// --- title bar ---

class MainWindowTitleBarTest : public MainWindowTest {
protected:
    void SetUp() override {
        MainWindowTest::SetUp();
        custom_title_bar_ = true;
    }
    void TearDown() override {
        qunsetenv("ANVIL_NATIVE_FRAME");
        MainWindowTest::TearDown();
    }

    // A mouse move over the editor, at the position in the window.
    void move_pointer(QPoint in_window) {
        QWidget* target = window_->view();
        QMouseEvent event(QEvent::MouseMove, target->mapFrom(window_.get(), in_window),
                          window_->mapToGlobal(in_window), Qt::NoButton, Qt::NoButton,
                          Qt::NoModifier);
        QApplication::sendEvent(target, &event);
    }
};

TEST_F(MainWindowTitleBarTest, TheWindowHasNoFrameAndTheTitleBarIsItsTopRow) {
    create_window();
    ASSERT_TRUE(window_->has_custom_title_bar());
    EXPECT_TRUE(window_->windowFlags().testFlag(Qt::FramelessWindowHint));
    ASSERT_NE(window_->title_bar(), nullptr);
    EXPECT_TRUE(window_->workbench()->isAncestorOf(window_->title_bar()));
    EXPECT_TRUE(window_->workbench()->is_visible(Part::title_bar));
    EXPECT_EQ(window_->title_bar()->mapTo(window_.get(), QPoint(0, 0)), QPoint(0, 0));
    EXPECT_EQ(window_->title_bar()->width(), window_->width());
    EXPECT_EQ(window_->title_bar()->height(), 35);
}

TEST_F(MainWindowTitleBarTest, TheMenuBarIsInTheTitleBarAndItsMenusWork) {
    create_window();
    ASSERT_TRUE(window_->has_custom_title_bar());
    EXPECT_EQ(window_->menu_bar()->parentWidget(), window_->title_bar());
    EXPECT_EQ(window_->menu_bar()->actions().size(), 8);

    int saved = 0;
    window_->commands().add("workbench.action.files.save", "Save",
                            [&saved](const json&) { ++saved; });
    QAction* save = menu_action("workbench.action.files.save");
    ASSERT_NE(save, nullptr);
    save->trigger();
    EXPECT_EQ(saved, 1);
}

TEST_F(MainWindowTitleBarTest, WithoutTheOptionTheWindowHasTheSystemsFrame) {
    custom_title_bar_ = false;
    create_window();
    EXPECT_FALSE(window_->has_custom_title_bar());
    EXPECT_FALSE(window_->windowFlags().testFlag(Qt::FramelessWindowHint));
    EXPECT_EQ(window_->title_bar(), nullptr);
    EXPECT_FALSE(window_->workbench()->is_visible(Part::title_bar));
    EXPECT_EQ(window_->menu_bar()->parentWidget(), window_.get());
}

TEST_F(MainWindowTitleBarTest, TheEnvironmentCanKeepTheSystemsFrame) {
    qputenv("ANVIL_NATIVE_FRAME", "1");
    create_window();
    EXPECT_FALSE(window_->has_custom_title_bar());
    EXPECT_FALSE(window_->windowFlags().testFlag(Qt::FramelessWindowHint));
}

TEST_F(MainWindowTitleBarTest, TheTitleBarStyleSettingChoosesTheFrame) {
    settings_file_ = write("settings.json", R"j({"window.titleBarStyle": "native"})j");
    create_window();
    EXPECT_FALSE(window_->has_custom_title_bar());
    EXPECT_FALSE(window_->windowFlags().testFlag(Qt::FramelessWindowHint));
    EXPECT_EQ(window_->menu_bar()->parentWidget(), window_.get());

    settings_file_ = write("settings.json", R"j({"window.titleBarStyle": "custom"})j");
    create_window();
    EXPECT_TRUE(window_->has_custom_title_bar());
    EXPECT_EQ(window_->menu_bar()->parentWidget(), window_->title_bar());
}

TEST_F(MainWindowTitleBarTest, ChangingTheStyleAppliesAfterARestart) {
    settings_file_ = write("settings.json", R"j({"window.titleBarStyle": "custom"})j");
    create_window();
    ASSERT_TRUE(window_->has_custom_title_bar());

    write("settings.json", R"j({"window.titleBarStyle": "native"})j");
    ASSERT_TRUE(wait_until([&] {
        return window_->settings().editor().title_bar_style == core::TitleBarStyle::native;
    }));
    QApplication::processEvents();
    EXPECT_TRUE(window_->has_custom_title_bar());

    create_window();
    EXPECT_FALSE(window_->has_custom_title_bar());
}

TEST_F(MainWindowTitleBarTest, ASettingThatIsNotAStyleLeavesTheCustomTitleBar) {
    settings_file_ = write("settings.json", R"j({"window.titleBarStyle": "inherit"})j");
    create_window();
    EXPECT_TRUE(window_->has_custom_title_bar());
    EXPECT_TRUE(window_->status_bar().message().contains("window.titleBarStyle"));
}

TEST_F(MainWindowTitleBarTest, TheCommandCenterAndTheLayoutControlsAreShownByDefault) {
    create_window();
    ASSERT_TRUE(window_->has_custom_title_bar());
    EXPECT_TRUE(window_->title_bar()->command_center_visible());
    EXPECT_TRUE(window_->title_bar()->command_center()->isVisible());
    EXPECT_EQ(window_->title_bar()->command_center()->text(), "Anvil");
    EXPECT_TRUE(window_->title_bar()->layout_controls_visible());
    EXPECT_TRUE(window_->title_bar()->sidebar_button()->isVisible());
    EXPECT_TRUE(window_->title_bar()->panel_button()->isVisible());
}

TEST_F(MainWindowTitleBarTest, ClickingTheCommandCenterOpensTheCommandPalette) {
    create_window();
    ASSERT_TRUE(window_->command_palette()->isHidden());
    window_->title_bar()->command_center()->click();
    EXPECT_TRUE(window_->command_palette()->isVisible());
}

TEST_F(MainWindowTitleBarTest, TheLayoutButtonsToggleThePartsAndFollowThemWhateverChangedThem) {
    create_window();
    ui::TitleBar* bar = window_->title_bar();
    ASSERT_NE(bar, nullptr);
    EXPECT_FALSE(window_->workbench()->is_visible(Part::sidebar));
    EXPECT_FALSE(bar->sidebar_button()->isChecked());
    EXPECT_FALSE(bar->panel_button()->isChecked());

    bar->sidebar_button()->click();
    EXPECT_TRUE(window_->workbench()->is_visible(Part::sidebar));
    EXPECT_TRUE(bar->sidebar_button()->isChecked());
    EXPECT_FALSE(bar->panel_button()->isChecked());

    bar->panel_button()->click();
    EXPECT_TRUE(window_->workbench()->is_visible(Part::panel));
    EXPECT_TRUE(bar->panel_button()->isChecked());

    window_->commands().execute("workbench.action.toggleSidebarVisibility");
    EXPECT_FALSE(bar->sidebar_button()->isChecked());
    window_->commands().execute("workbench.action.togglePanel");
    EXPECT_FALSE(bar->panel_button()->isChecked());

    window_->workbench()->set_visible(Part::sidebar, true);
    EXPECT_TRUE(bar->sidebar_button()->isChecked());
    bar->sidebar_button()->click();
    EXPECT_FALSE(window_->workbench()->is_visible(Part::sidebar));
    EXPECT_FALSE(bar->sidebar_button()->isChecked());
}

TEST_F(MainWindowTitleBarTest, TheButtonsAndTheCommandCenterNameTheirKeys) {
    keybindings_file_ = scratch_ / "keybindings.json";
    create_window();
    ui::TitleBar* bar = window_->title_bar();
    EXPECT_EQ(bar->sidebar_button()->toolTip(), "Toggle Primary Side Bar (Ctrl+B)");
    EXPECT_EQ(bar->panel_button()->toolTip(), "Toggle Panel (Ctrl+J)");
    EXPECT_EQ(bar->command_center()->toolTip(), "Show All Commands (Ctrl+Shift+P)");

    write("keybindings.json",
          R"j([{"key": "ctrl+alt+b", "command": "workbench.action.toggleSidebarVisibility"}])j");
    ASSERT_TRUE(wait_until([&] {
        return bar->sidebar_button()->toolTip() == "Toggle Primary Side Bar (Ctrl+Alt+B)";
    }));
    EXPECT_EQ(bar->panel_button()->toolTip(), "Toggle Panel (Ctrl+J)");
}

TEST_F(MainWindowTitleBarTest, TheSettingsTurnTheCommandCenterAndTheLayoutControlsOff) {
    settings_file_ = write("settings.json",
                           R"j({"window.commandCenter": false,
                                "workbench.layoutControl.enabled": false})j");
    create_window();
    ui::TitleBar* bar = window_->title_bar();
    ASSERT_NE(bar, nullptr);
    EXPECT_FALSE(bar->command_center_visible());
    EXPECT_FALSE(bar->command_center()->isVisible());
    EXPECT_FALSE(bar->layout_controls_visible());
    EXPECT_FALSE(bar->sidebar_button()->isVisible());
    EXPECT_FALSE(bar->panel_button()->isVisible());
}

TEST_F(MainWindowTitleBarTest,
       EachSettingTurnsOffItsOwnPartAndTheyFollowChangesWhileTheWindowRuns) {
    settings_file_ = write("settings.json", R"j({"window.commandCenter": false})j");
    create_window();
    ui::TitleBar* bar = window_->title_bar();
    EXPECT_FALSE(bar->command_center()->isVisible());
    EXPECT_TRUE(bar->sidebar_button()->isVisible());

    write("settings.json", R"j({"workbench.layoutControl.enabled": false})j");
    ASSERT_TRUE(wait_until([&] { return !bar->sidebar_button()->isVisible(); }));
    EXPECT_TRUE(bar->command_center()->isVisible());
    EXPECT_FALSE(bar->panel_button()->isVisible());

    write("settings.json", "{}");
    ASSERT_TRUE(wait_until([&] { return bar->sidebar_button()->isVisible(); }));
    EXPECT_TRUE(bar->command_center()->isVisible());
}

TEST_F(MainWindowTitleBarTest, WithoutTheCommandCenterTheBarShowsTheWindowTitleAgain) {
    settings_file_ = write("settings.json", R"j({"window.commandCenter": false})j");
    create_window();
    ui::TitleBar* bar = window_->title_bar();
    const QImage untitled = bar->grab().toImage();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "abc").string())));
    EXPECT_NE(bar->grab().toImage(), untitled);
}

TEST_F(MainWindowTitleBarTest, TheTitleFollowsTheShownDocumentAndItsChanges) {
    create_window();
    ASSERT_TRUE(window_->has_custom_title_bar());
    EXPECT_EQ(window_->title_bar()->title(), "Untitled-1 — Anvil");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "abc").string())));
    EXPECT_EQ(window_->title_bar()->title(), "a.txt — Anvil");
    type("X");
    EXPECT_EQ(window_->title_bar()->title(), "a.txt — Anvil*");
}

TEST_F(MainWindowTitleBarTest, TheTitleBarShowsWhetherTheWindowIsActive) {
    create_window();
    const QPoint at(window_->width() / 2, 4);
    const auto background = [&] { return window_->title_bar()->grab().toImage().pixelColor(at); };
    window_->activateWindow();
    ASSERT_TRUE(QTest::qWaitForWindowActive(window_.get()));
    const QColor active = background();
    EXPECT_EQ(active, window_->colors().color("titleBar.activeBackground"));

    QWidget other;
    other.show();
    other.activateWindow();
    ASSERT_TRUE(QTest::qWaitForWindowActive(&other));
    QApplication::processEvents();
    EXPECT_EQ(background(), window_->colors().color("titleBar.inactiveBackground"));
}

TEST_F(MainWindowTitleBarTest, TheMaximizeButtonMaximizesAndRestoresTheWindow) {
    create_window();
    auto* border = window_->findChild<ui::FrameBorder*>();
    ASSERT_NE(border, nullptr);
    EXPECT_TRUE(border->isVisible());

    window_->title_bar()->maximize_button()->click();
    QApplication::processEvents();
    EXPECT_TRUE(window_->isMaximized());
    EXPECT_EQ(window_->title_bar()->maximize_button()->accessibleName(), "Restore");
    EXPECT_FALSE(border->isVisible());

    window_->title_bar()->maximize_button()->click();
    QApplication::processEvents();
    EXPECT_FALSE(window_->isMaximized());
    EXPECT_EQ(window_->title_bar()->maximize_button()->accessibleName(), "Maximize");
    EXPECT_TRUE(border->isVisible());
}

TEST_F(MainWindowTitleBarTest, ADoubleClickOnTheTitleMaximizesTheWindow) {
    create_window();
    QMouseEvent double_click(QEvent::MouseButtonDblClick, QPointF(window_->width() / 2, 4),
                             window_->title_bar()->mapToGlobal(QPointF(window_->width() / 2, 4)),
                             Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(window_->title_bar(), &double_click);
    QApplication::processEvents();
    EXPECT_TRUE(window_->isMaximized());
}

TEST_F(MainWindowTitleBarTest, TheCloseButtonClosesTheWindow) {
    create_window();
    ASSERT_TRUE(window_->isVisible());
    window_->title_bar()->close_button()->click();
    EXPECT_FALSE(window_->isVisible());
}

TEST_F(MainWindowTitleBarTest, TheMinimizeButtonMinimizesTheWindow) {
    create_window();
    window_->title_bar()->minimize_button()->click();
    QApplication::processEvents();
    EXPECT_TRUE(window_->isMinimized());
}

TEST_F(MainWindowTitleBarTest, FullscreenHidesTheTitleBarUntilThePointerReachesTheTopEdge) {
    create_window();
    window_->showFullScreen();
    QApplication::processEvents();
    ASSERT_TRUE(window_->isFullScreen());
    EXPECT_FALSE(window_->workbench()->is_visible(Part::title_bar));

    move_pointer(QPoint(200, 300));
    EXPECT_FALSE(window_->workbench()->is_visible(Part::title_bar));
    move_pointer(QPoint(200, 0));
    EXPECT_TRUE(window_->workbench()->is_visible(Part::title_bar));
    move_pointer(QPoint(200, 20));
    EXPECT_TRUE(window_->workbench()->is_visible(Part::title_bar));
    move_pointer(QPoint(200, 300));
    EXPECT_FALSE(window_->workbench()->is_visible(Part::title_bar));

    window_->showNormal();
    QApplication::processEvents();
    EXPECT_TRUE(window_->workbench()->is_visible(Part::title_bar));
}

// The menu bar in the title bar is used with the keyboard and the pointer as VS Code's is.
class MainWindowMenuBarTest : public MainWindowTitleBarTest {
protected:
    void create_active_window() {
        create_window();
        ASSERT_TRUE(window_->has_custom_title_bar());
        window_->activateWindow();
        ASSERT_TRUE(QTest::qWaitForWindowActive(window_.get()));
        window_->view()->setFocus();
        QApplication::processEvents();
    }
    QMenu* menu(int index) const {
        return window_->menu_bar()->actions().at(index)->menu();
    }
    QWidget* popup() const {
        return QApplication::activePopupWidget();
    }
    // Opens a menu as the user does with Alt and its mnemonic.
    void open_with_mnemonic(Qt::Key key) {
        QTest::keyClick(window_.get(), key, Qt::AltModifier);
        QApplication::processEvents();
    }
};

TEST_F(MainWindowMenuBarTest, AltAndAMnemonicOpenAMenuAndEscapeClosesIt) {
    create_active_window();
    EXPECT_EQ(popup(), nullptr);
    open_with_mnemonic(Qt::Key_F);
    EXPECT_EQ(popup(), menu(0));
    QTest::keyClick(popup(), Qt::Key_Escape);
    QApplication::processEvents();
    EXPECT_EQ(popup(), nullptr);

    open_with_mnemonic(Qt::Key_E);
    EXPECT_EQ(popup(), menu(1));
    QTest::keyClick(popup(), Qt::Key_Escape);
    open_with_mnemonic(Qt::Key_H);
    EXPECT_EQ(popup(), menu(7));
    QTest::keyClick(popup(), Qt::Key_Escape);
}

TEST_F(MainWindowMenuBarTest, TheArrowKeysMoveBetweenTheMenusAndTheirItems) {
    create_active_window();
    open_with_mnemonic(Qt::Key_F);
    ASSERT_EQ(popup(), menu(0));

    QTest::keyClick(popup(), Qt::Key_Right);
    QApplication::processEvents();
    EXPECT_EQ(popup(), menu(1));
    QTest::keyClick(popup(), Qt::Key_Right);
    QApplication::processEvents();
    EXPECT_EQ(popup(), menu(2));
    QTest::keyClick(popup(), Qt::Key_Left);
    QApplication::processEvents();
    EXPECT_EQ(popup(), menu(1));

    // Arriving by key chooses the first item.
    const QAction* first = menu(1)->activeAction();
    ASSERT_NE(first, nullptr);
    QTest::keyClick(popup(), Qt::Key_Down);
    EXPECT_NE(menu(1)->activeAction(), first);
    QTest::keyClick(popup(), Qt::Key_Up);
    EXPECT_EQ(menu(1)->activeAction(), first);
    QTest::keyClick(popup(), Qt::Key_Escape);
}

TEST_F(MainWindowMenuBarTest, AltAloneFocusesTheFirstMenuAndEscapeLeavesTheBar) {
    create_active_window();
    QTest::keyClick(window_.get(), Qt::Key_Alt);
    QApplication::processEvents();
    EXPECT_EQ(window_->menu_bar()->activeAction(), window_->menu_bar()->actions().at(0));
    QTest::keyClick(window_->menu_bar(), Qt::Key_Right);
    EXPECT_EQ(window_->menu_bar()->activeAction(), window_->menu_bar()->actions().at(1));
    QTest::keyClick(window_->menu_bar(), Qt::Key_Escape);
    QApplication::processEvents();
    EXPECT_EQ(window_->menu_bar()->activeAction(), nullptr);
}

TEST_F(MainWindowMenuBarTest, WhileAMenuIsOpenThePointerOpensTheMenuItIsOver) {
    create_active_window();
    open_with_mnemonic(Qt::Key_F);
    ASSERT_EQ(popup(), menu(0));

    QAction* edit = window_->menu_bar()->actions().at(1);
    const QPoint over_edit =
        window_->menu_bar()->mapToGlobal(window_->menu_bar()->actionGeometry(edit).center());
    QMouseEvent move(QEvent::MouseMove, menu(0)->mapFromGlobal(over_edit), over_edit, Qt::NoButton,
                     Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(popup(), &move);
    QApplication::processEvents();
    EXPECT_EQ(popup(), menu(1));
    QTest::keyClick(popup(), Qt::Key_Escape);
}

TEST_F(MainWindowMenuBarTest, TheMenusFollowTheMenuColorsOfTheTheme) {
    create_active_window();
    window_->colors().set_colors({{"menu.background", QColor("#202020")},
                                  {"menu.foreground", QColor("#dddddd")},
                                  {"menu.border", QColor("#404040")},
                                  {"menu.selectionBackground", QColor("#0078d4")},
                                  {"menu.selectionForeground", QColor("#ffffff")},
                                  {"menu.separatorBackground", QColor("#ff00ff")}});
    open_with_mnemonic(Qt::Key_F);
    ASSERT_EQ(popup(), menu(0));

    const auto first_separator = [&]() -> QAction* {
        for (QAction* action : menu(0)->actions()) {
            if (action->isSeparator()) {
                return action;
            }
        }
        return nullptr;
    };
    QImage image = menu(0)->grab().toImage();
    EXPECT_EQ(image.pixelColor(image.width() / 2, 3), QColor("#202020"));
    EXPECT_EQ(image.pixelColor(0, image.height() / 2), QColor("#404040"));
    QAction* separator = first_separator();
    ASSERT_NE(separator, nullptr);
    EXPECT_EQ(image.pixelColor(image.width() / 2, menu(0)->actionGeometry(separator).center().y()),
              QColor("#ff00ff"));

    QTest::keyClick(popup(), Qt::Key_Down);
    ASSERT_NE(menu(0)->activeAction(), nullptr);
    image = menu(0)->grab().toImage();
    const QRect chosen = menu(0)->actionGeometry(menu(0)->activeAction());
    EXPECT_EQ(image.pixelColor(chosen.right() - 4, chosen.center().y()), QColor("#0078d4"));
    QTest::keyClick(popup(), Qt::Key_Escape);
    QApplication::processEvents();

    window_->colors().set_colors({{"menu.background", QColor("#ff0000")}});
    open_with_mnemonic(Qt::Key_F);
    image = menu(0)->grab().toImage();
    EXPECT_EQ(image.pixelColor(image.width() / 2, 3), QColor("#ff0000"));
    QTest::keyClick(popup(), Qt::Key_Escape);
}

TEST_F(MainWindowMenuBarTest, ADisabledItemIsDimmerThanAnEnabledOne) {
    create_active_window();
    window_->colors().set_colors(
        {{"menu.background", QColor("#202020")}, {"menu.foreground", QColor("#dddddd")}});
    open_with_mnemonic(Qt::Key_F);
    const QImage image = menu(0)->grab().toImage();
    const auto brightest = [&](const std::string& command) {
        QAction* action = nullptr;
        for (QAction* candidate : menu(0)->actions()) {
            if (candidate->data().toString().toStdString() == command) {
                action = candidate;
            }
        }
        EXPECT_NE(action, nullptr) << command;
        int best = 0;
        const QRect row = menu(0)->actionGeometry(action);
        for (int y = row.top(); y <= row.bottom(); ++y) {
            for (int x = row.left(); x < row.left() + row.width() / 2; ++x) {
                best = std::max(best, image.pixelColor(x, y).lightness());
            }
        }
        return best;
    };
    // Open File exists and Open Workspace from File does not.
    const int enabled = brightest("workbench.action.files.openFile");
    const int disabled = brightest("workbench.action.openWorkspace");
    EXPECT_GT(enabled, 180);
    EXPECT_LT(disabled, enabled - 60);
    QTest::keyClick(popup(), Qt::Key_Escape);
}

// The action for `command` in the menu `index` as it is when the menu opens.
QAction* opened_action(QMenu* menu, const std::string& command) {
    emit menu->aboutToShow();
    std::vector<QMenu*> pending{menu};
    while (!pending.empty()) {
        QMenu* current = pending.back();
        pending.pop_back();
        for (QAction* action : current->actions()) {
            if (action->menu()) {
                pending.push_back(action->menu());
            } else if (action->data().toString().toStdString() == command) {
                return action;
            }
        }
    }
    return nullptr;
}

TEST_F(MainWindowMenuBarTest, AnItemWithAToggledConditionShowsTheSettingAsACheckMark) {
    settings_file_ = write("settings.json", R"j({"files.autoSave": "afterDelay"})j");
    create_window();
    QAction* auto_save = opened_action(menu(0), "workbench.action.toggleAutoSave");
    ASSERT_NE(auto_save, nullptr);
    EXPECT_TRUE(auto_save->isCheckable());
    EXPECT_TRUE(auto_save->isChecked());

    QAction* minimap = opened_action(menu(3), "editor.action.toggleMinimap");
    ASSERT_NE(minimap, nullptr);
    EXPECT_TRUE(minimap->isCheckable());
    EXPECT_TRUE(minimap->isChecked()) << "VS Code's default is on";

    window_->change_setting("editor.minimap.enabled", false);
    window_->change_setting("files.autoSave", "off");
    EXPECT_FALSE(opened_action(menu(3), "editor.action.toggleMinimap")->isChecked());
    EXPECT_FALSE(opened_action(menu(0), "workbench.action.toggleAutoSave")->isChecked());
}

TEST_F(MainWindowMenuBarTest, TheAutoSaveItemIsEnabledAndItsCheckMarkFollowsTheCommand) {
    settings_file_ = write("settings.json", "{}");
    create_window();
    QAction* item = opened_action(menu(0), "workbench.action.toggleAutoSave");
    ASSERT_NE(item, nullptr);
    EXPECT_TRUE(item->isEnabled());
    EXPECT_FALSE(item->isChecked());
    item->trigger();
    EXPECT_TRUE(opened_action(menu(0), "workbench.action.toggleAutoSave")->isChecked());
}

TEST_F(MainWindowMenuBarTest, TheRenderWhitespaceCommandsSetTheSettingAndTheMenuChecksTheMode) {
    settings_file_ = write("settings.json", "{}");
    create_window();
    const std::vector<std::pair<std::string, core::RenderWhitespace>> modes = {
        {"none", core::RenderWhitespace::none},
        {"boundary", core::RenderWhitespace::boundary},
        {"selection", core::RenderWhitespace::selection},
        {"trailing", core::RenderWhitespace::trailing},
        {"all", core::RenderWhitespace::all}};
    for (const auto& [name, mode] : modes) {
        window_->commands().execute("editor.action.renderWhitespace." + name);
        EXPECT_EQ(window_->settings().editor().render_whitespace, mode) << name;
        EXPECT_NE(platform::read_file(settings_file_)->find("\"" + name + "\""), std::string::npos)
            << name;
        for (const auto& [other, unused] : modes) {
            QAction* item = opened_action(menu(3), "editor.action.renderWhitespace." + other);
            ASSERT_NE(item, nullptr) << other;
            EXPECT_TRUE(item->isEnabled()) << other;
            EXPECT_EQ(item->isChecked(), other == name) << "after " << name << ", item " << other;
        }
    }
}

TEST_F(MainWindowMenuBarTest, TheControlCharacterCommandFlipsTheSettingAndTheMenuChecksIt) {
    settings_file_ = write("settings.json", "{}");
    create_window();
    EXPECT_TRUE(window_->settings().editor().render_control_characters);
    EXPECT_TRUE(opened_action(menu(3), "editor.action.toggleRenderControlCharacter")->isChecked());
    window_->commands().execute("editor.action.toggleRenderControlCharacter");
    EXPECT_FALSE(window_->settings().editor().render_control_characters);
    EXPECT_FALSE(opened_action(menu(3), "editor.action.toggleRenderControlCharacter")->isChecked());
    window_->commands().execute("editor.action.toggleRenderControlCharacter");
    EXPECT_TRUE(window_->settings().editor().render_control_characters);
}

TEST_F(MainWindowMenuBarTest, TheMultiCursorModifierItemOffersTheOtherModifierAndSwitchesIt) {
    settings_file_ = write("settings.json", "{}");
    create_window();
    QAction* item = opened_action(menu(2), "workbench.action.toggleMultiCursorModifier");
    ASSERT_NE(item, nullptr);
    EXPECT_TRUE(item->isEnabled());
    EXPECT_TRUE(item->text().contains("Ctrl+Click")) << item->text().toStdString();
    EXPECT_EQ(window_->settings().editor().multi_cursor_modifier, core::MultiCursorModifier::alt);

    item->trigger();
    EXPECT_EQ(window_->settings().editor().multi_cursor_modifier,
              core::MultiCursorModifier::ctrl_cmd);
    EXPECT_NE(platform::read_file(settings_file_)->find("\"ctrlCmd\""), std::string::npos);
    item = opened_action(menu(2), "workbench.action.toggleMultiCursorModifier");
    EXPECT_TRUE(item->text().contains("Alt+Click")) << item->text().toStdString();

    item->trigger();
    EXPECT_EQ(window_->settings().editor().multi_cursor_modifier, core::MultiCursorModifier::alt);
}

TEST_F(MainWindowMenuBarTest, TheSwitchedModifierIsTheOneThatAddsACursor) {
    settings_file_ = write("settings.json", "{}");
    create_window();
    type("ab\ncd");
    const auto click_at = [&](std::size_t offset, Qt::KeyboardModifiers modifiers) {
        const QPoint point = window_->view()->cursor_rect(offset).center();
        for (const QEvent::Type type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
            QMouseEvent event(
                type, QPointF(point), window_->view()->viewport()->mapToGlobal(QPointF(point)),
                Qt::LeftButton, type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                modifiers);
            QApplication::sendEvent(window_->view()->viewport(), &event);
        }
    };
    const auto cursors = [&] { return window_->current_document()->selections().all().size(); };
    click_at(4, Qt::NoModifier);
    click_at(0, Qt::ControlModifier);
    EXPECT_EQ(cursors(), 1u) << "Ctrl+click is a plain click while Alt is the modifier";
    click_at(4, Qt::NoModifier);
    click_at(0, Qt::AltModifier);
    EXPECT_EQ(cursors(), 2u);

    window_->commands().execute("workbench.action.toggleMultiCursorModifier");
    click_at(4, Qt::NoModifier);
    click_at(0, Qt::AltModifier);
    EXPECT_EQ(cursors(), 1u);
    click_at(4, Qt::NoModifier);
    click_at(0, Qt::ControlModifier);
    EXPECT_EQ(cursors(), 2u);
}

TEST_F(MainWindowMenuBarTest, TheColumnSelectionItemIsCheckedWhileTheModeIsOn) {
    settings_file_ = write("settings.json", "{}");
    create_window();
    QAction* item = opened_action(menu(2), "editor.action.toggleColumnSelection");
    ASSERT_NE(item, nullptr);
    EXPECT_TRUE(item->isEnabled());
    EXPECT_FALSE(item->isChecked());
    item->trigger();
    EXPECT_TRUE(window_->settings().editor().column_selection);
    EXPECT_TRUE(opened_action(menu(2), "editor.action.toggleColumnSelection")->isChecked());
    window_->commands().execute("editor.action.toggleColumnSelection");
    EXPECT_FALSE(window_->settings().editor().column_selection);
}

TEST_F(MainWindowMenuBarTest, SaveAllWaitsForUnsavedChangesAndRevertForAFile) {
    settings_file_ = write("settings.json", "{}");
    create_window();
    EXPECT_FALSE(opened_action(menu(0), "saveAll")->isEnabled()) << "nothing is unsaved";
    EXPECT_FALSE(opened_action(menu(0), "workbench.action.files.revert")->isEnabled())
        << "an untitled document has no file";
    window_->view()->setFocus();
    QKeyEvent press(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier, "x");
    QApplication::sendEvent(window_->view(), &press);
    EXPECT_TRUE(opened_action(menu(0), "saveAll")->isEnabled());
    EXPECT_FALSE(opened_action(menu(0), "workbench.action.files.revert")->isEnabled());

    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "A").string())));
    EXPECT_TRUE(opened_action(menu(0), "workbench.action.files.revert")->isEnabled());
}

TEST_F(MainWindowMenuBarTest, TheGoMenuSwitchesEditorsAndGroupsAndTheThirdGroupWaitsForGroups) {
    settings_file_ = write("settings.json", "{}");
    create_window();
    for (const char* id :
         {"workbench.action.nextEditor", "workbench.action.previousEditor",
          "workbench.action.openNextRecentlyUsedEditor",
          "workbench.action.openPreviousRecentlyUsedEditorInGroup",
          "workbench.action.focusFirstEditorGroup", "workbench.action.focusSecondEditorGroup"}) {
        QAction* item = opened_action(menu(4), id);
        ASSERT_NE(item, nullptr) << id;
        EXPECT_TRUE(item->isEnabled()) << id;
    }
    EXPECT_FALSE(opened_action(menu(4), "workbench.action.focusThirdEditorGroup")->isEnabled());
    EXPECT_FALSE(opened_action(menu(4), "workbench.action.focusLeftGroup")->isEnabled());
    window_->commands().execute("workbench.action.splitEditorRight");
    window_->commands().execute("workbench.action.splitEditorRight");
    for (const char* id : {"workbench.action.focusThirdEditorGroup",
                           "workbench.action.focusLeftGroup", "workbench.action.focusBelowGroup"}) {
        EXPECT_TRUE(opened_action(menu(4), id)->isEnabled()) << id;
    }
}

TEST_F(MainWindowMenuBarTest, TheEditorLayoutItemsOfTheViewMenuArrangeTheGroups) {
    settings_file_ = write("settings.json", "{}");
    create_window();
    QAction* two = opened_action(menu(3), "workbench.action.editorLayoutTwoColumns");
    ASSERT_NE(two, nullptr);
    EXPECT_TRUE(two->isEnabled());
    EXPECT_TRUE(opened_action(menu(3), "workbench.action.toggleEditorGroupLayout")->isEnabled());
    two->trigger();
    EXPECT_EQ(window_->editor_layout().group_ids().size(), 2u);
    opened_action(menu(3), "workbench.action.editorLayoutSingle")->trigger();
    EXPECT_EQ(window_->editor_layout().group_ids().size(), 1u);
}

TEST_F(MainWindowMenuBarTest, AnItemThatIsNotToggledIsNotCheckable) {
    create_window();
    QAction* open = opened_action(menu(0), "workbench.action.files.openFile");
    ASSERT_NE(open, nullptr);
    EXPECT_FALSE(open->isCheckable());
}

TEST_F(MainWindowMenuBarTest, TheCheckMarksOfThePartsThatTheWindowHasFollowThem) {
    create_window();
    for (const bool shown : {true, false, true}) {
        window_->workbench()->set_visible(Part::sidebar, shown);
        window_->workbench()->set_visible(Part::panel, shown);
        EXPECT_EQ(opened_action(menu(3), "workbench.action.toggleSidebarVisibility")->isChecked(),
                  shown);
        EXPECT_EQ(opened_action(menu(3), "workbench.action.togglePanel")->isChecked(), shown);
    }
}

TEST_F(MainWindowMenuBarTest,
       AnItemWhosePreconditionDoesNotHoldIsDisabledAndEnabledAgainWhenItDoes) {
    create_window();
    EXPECT_TRUE(opened_action(menu(0), "workbench.action.files.save")->isEnabled());
    EXPECT_TRUE(opened_action(menu(0), "workbench.action.files.saveAs")->isEnabled());

    window_->commands().execute("workbench.action.splitEditorRight");
    window_->commands().execute("workbench.action.closeActiveEditor");
    ASSERT_TRUE(window_->editor_layout().group(window_->editor_layout().focused())->tabs.empty());
    EXPECT_FALSE(opened_action(menu(0), "workbench.action.files.save")->isEnabled());
    EXPECT_FALSE(opened_action(menu(0), "workbench.action.files.saveAs")->isEnabled());
    EXPECT_FALSE(opened_action(menu(0), "workbench.action.closeActiveEditor")->isEnabled());

    window_->commands().execute("workbench.action.files.newUntitledFile");
    EXPECT_TRUE(opened_action(menu(0), "workbench.action.files.save")->isEnabled());
}

class MenuStyleTest : public MainWindowMenuBarTest {
protected:
    static constexpr const char* background = "#202020";

    // Opens the File menu with known colors; the first item is not the one that is chosen.
    void open_file_menu(const char* settings = "{}") {
        settings_file_ = write("settings.json", settings);
        create_active_window();
        window_->colors().set_colors(
            {{"menu.background", QColor(background)}, {"menu.foreground", QColor("#dddddd")}});
        open_with_mnemonic(Qt::Key_F);
        ASSERT_EQ(popup(), menu(0));
        menu(0)->setActiveAction(nullptr);
        image_ = menu(0)->grab().toImage();
    }
    QAction* action(const std::string& command) const {
        for (QAction* candidate : menu(0)->actions()) {
            if (candidate->data().toString().toStdString() == command) {
                return candidate;
            }
        }
        for (QAction* candidate : menu(0)->actions()) {
            if (candidate->menu() && candidate->text().contains(QString::fromStdString(command))) {
                return candidate;
            }
        }
        ADD_FAILURE() << "no item " << command;
        return nullptr;
    }
    // The columns in [from, to) of the item's row that differ from the background.
    std::vector<int> ink(QAction* item, int from, int to) const {
        const QRect row = menu(0)->actionGeometry(item);
        std::vector<int> columns;
        for (int x = from; x < to; ++x) {
            for (int y = row.top(); y <= row.bottom(); ++y) {
                if (image_.pixelColor(x, y) != QColor(background)) {
                    columns.push_back(x);
                    break;
                }
            }
        }
        return columns;
    }
    int rightmost_ink(QAction* item) const {
        const auto columns = ink(item, 1, image_.width() - 1);
        return columns.empty() ? -1 : columns.back();
    }

    QImage image_;
};

TEST_F(MenuStyleTest, ACheckedItemDrawsTheCheckInTheColumnAtTheLeft) {
    open_file_menu(R"j({"files.autoSave": "afterDelay"})j");
    QAction* checked = action("workbench.action.toggleAutoSave");
    QAction* unchecked = action("workbench.action.files.save");
    ASSERT_TRUE(checked->isChecked());
    const auto marks = ink(checked, 1, ui::MenuStyle::label_left - 4);
    EXPECT_GE(marks.size(), 6u) << "the check";
    EXPECT_TRUE(ink(unchecked, 1, ui::MenuStyle::label_left - 4).empty());
    if (const char* directory = std::getenv("ANVIL_SCREENSHOT_DIR")) {
        image_.save(QString::fromUtf8(directory) + "/file_menu.png");
    }
}

TEST_F(MenuStyleTest, ASubmenuItemDrawsTheChevronAtTheRightEdge) {
    open_file_menu();
    QAction* submenu = action("Recent");
    QAction* plain = action("workbench.action.files.openFolder");
    const int from = image_.width() - ui::MenuStyle::chevron_center_from_right - 8;
    const auto chevron = ink(submenu, from, from + 16);
    EXPECT_GE(chevron.size(), 3u);
    EXPECT_TRUE(ink(plain, from, from + 16).empty());
}

TEST_F(MenuStyleTest, ShortcutsEndAtTheSameDistanceFromTheRightEdgeWhateverTheirWidth) {
    open_file_menu();
    const int expected = image_.width() - 1 - ui::MenuStyle::shortcut_right;
    QAction* short_one = action("workbench.action.files.openFile");
    QAction* long_one = action("workbench.action.files.saveAs");
    ASSERT_FALSE(menu(0)->actionGeometry(short_one).isEmpty());
    for (QAction* item : {short_one, long_one}) {
        EXPECT_NEAR(rightmost_ink(item), expected, 3) << item->text().toStdString();
    }
    EXPECT_NEAR(rightmost_ink(short_one), rightmost_ink(long_one), 2);
}

TEST_F(MenuStyleTest, TheItemsAndSeparatorsHaveVSCodesHeights) {
    open_file_menu();
    EXPECT_EQ(menu(0)->actionGeometry(action("workbench.action.files.openFile")).height(),
              ui::MenuStyle::item_height);
    for (QAction* item : menu(0)->actions()) {
        if (item->isSeparator()) {
            EXPECT_EQ(menu(0)->actionGeometry(item).height(), ui::MenuStyle::separator_height);
        }
    }
    const QRect open = menu(0)->actionGeometry(action("workbench.action.files.openFile"));
    const auto text = ink(action("workbench.action.files.openFile"), 1, 60);
    ASSERT_FALSE(text.empty());
    EXPECT_NEAR(text.front(), ui::MenuStyle::label_left, 3);
    EXPECT_GE(open.width(), 200);
}

TEST_F(MenuStyleTest, ASubmenuIsDrawnByTheStyleOfItsMenu) {
    open_file_menu();
    QMenu* recent = action("Recent")->menu();
    ASSERT_FALSE(recent->actions().isEmpty());
    recent->popup(menu(0)->mapToGlobal(menu(0)->actionGeometry(action("Recent")).topRight()));
    QApplication::processEvents();
    const QAction* first = recent->actions().first();
    EXPECT_EQ(recent->actionGeometry(const_cast<QAction*>(first)).height(),
              ui::MenuStyle::item_height);
    recent->close();
    QTest::keyClick(popup(), Qt::Key_Escape);
}

TEST_F(MenuStyleTest, AMenuTakesItsColorsFromTheThemeWhenItIsDrawn) {
    open_file_menu();
    EXPECT_EQ(image_.pixelColor(image_.width() / 2, 3), QColor(background));
    window_->colors().set_colors({{"menu.background", QColor("#ff0000")}});
    QApplication::processEvents();
    EXPECT_EQ(menu(0)->grab().toImage().pixelColor(image_.width() / 2, 3), QColor("#ff0000"));
    QTest::keyClick(popup(), Qt::Key_Escape);
}

TEST_F(MainWindowMenuBarTest, TheSubmenusFollowTheirMenu) {
    create_active_window();
    window_->colors().set_colors({{"menu.background", QColor("#202020")}});
    open_with_mnemonic(Qt::Key_F);
    QAction* recent = nullptr;
    for (QAction* action : menu(0)->actions()) {
        if (action->menu() && action->text().contains("Recent")) {
            recent = action;
        }
    }
    ASSERT_NE(recent, nullptr);
    recent->menu()->popup(menu(0)->mapToGlobal(menu(0)->actionGeometry(recent).topRight()));
    QApplication::processEvents();
    const QImage image = recent->menu()->grab().toImage();
    EXPECT_EQ(image.pixelColor(image.width() / 2, 3), QColor("#202020"));
    recent->menu()->close();
    QTest::keyClick(popup(), Qt::Key_Escape);
}

TEST_F(MainWindowMenuBarTest, MenusThatDoNotFitCollapseIntoTheExtensionMenu) {
    create_active_window();
    const QMenuBar* bar = window_->menu_bar();
    const auto extension_shown = [&] {
        for (const QToolButton* button : bar->findChildren<QToolButton*>()) {
            if (button->isVisible()) {
                return true;
            }
        }
        return false;
    };
    window_->resize(1000, 600);
    QApplication::processEvents();
    EXPECT_FALSE(extension_shown());

    window_->resize(420, 600);
    QApplication::processEvents();
    EXPECT_TRUE(extension_shown());
    EXPECT_LE(bar->x() + bar->width(), window_->title_bar()->minimize_button()->x());
}

TEST_F(MainWindowTitleBarTest, TheSystemsFrameReturnsWhenTheWindowSystemCannotMoveTheWindow) {
    create_window();
    ASSERT_TRUE(window_->has_custom_title_bar());
    QMenuBar* menu_bar = window_->menu_bar();
    // The offscreen platform has no window manager to ask.
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(window_->width() / 2, 4),
                      window_->title_bar()->mapToGlobal(QPointF(window_->width() / 2, 4)),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(window_->title_bar(), &press);
    QApplication::processEvents();

    EXPECT_FALSE(window_->has_custom_title_bar());
    EXPECT_FALSE(window_->windowFlags().testFlag(Qt::FramelessWindowHint));
    EXPECT_FALSE(window_->workbench()->is_visible(Part::title_bar));
    EXPECT_EQ(window_->menu_bar(), menu_bar);
    EXPECT_EQ(menu_bar->parentWidget(), window_.get());
    EXPECT_TRUE(window_->isVisible());
    EXPECT_EQ(menu_bar->actions().size(), 8);
}

TEST_F(MainWindowTitleBarTest, TheSystemsFrameReturnsWhenTheWindowSystemCannotResizeTheWindow) {
    create_window();
    ASSERT_TRUE(window_->has_custom_title_bar());
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(2, 300),
                      window_->mapToGlobal(QPointF(2, 300)), Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QApplication::sendEvent(window_->view(), &press);
    QApplication::processEvents();
    EXPECT_FALSE(window_->has_custom_title_bar());
}

TEST_F(MainWindowTest, TheMenuBarHasVSCodesMenus) {
    create_window();
    QStringList titles;
    for (const QAction* top : window_->menu_bar()->actions()) {
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
    EXPECT_EQ(first_title(), first_command_title());
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
    EXPECT_EQ(window_->settings().editor().activity_bar_location,
              core::ActivityBarLocation::hidden);
    window_->commands().execute("workbench.action.toggleActivityBarVisibility");
    EXPECT_EQ(window_->settings().editor().activity_bar_location,
              core::ActivityBarLocation::standard);
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

class MainWindowGroupKeysTest : public MainWindowKeysTest {
protected:
    // Three files open in the first group, and the focus on it.
    void open_files() {
        create_active_window();
        for (const char* name : {"a.txt", "b.txt", "c.txt"}) {
            ASSERT_TRUE(window_->open_file(QString::fromStdString(write(name, name).string())));
        }
    }
    std::string shown() const {
        return window_->current_uri().path.substr(window_->current_uri().path.rfind('/') + 1);
    }
    std::size_t group_count() const {
        return window_->editor_layout().group_ids().size();
    }
    void key(Qt::Key key, Qt::KeyboardModifiers modifiers) {
        QTest::keyClick(window_->view(), key, modifiers);
    }
    void chord(Qt::Key second) {
        QTest::keyClick(window_->view(), Qt::Key_K, Qt::ControlModifier);
        QTest::keyClick(window_->view(), second, Qt::ControlModifier);
    }
};

// Where the groups are on the screen, found from the widgets.
class MainWindowLayoutTest : public MainWindowGroupKeysTest {
protected:
    // The top left corner and size of each group of the window, in the order the layout shows them.
    std::vector<QRect> group_rects() {
        std::vector<QRect> rects;
        for (const workbench::GroupId id : window_->editor_layout().group_ids()) {
            QWidget* widget = window_->editor_area()->group(id);
            rects.push_back(QRect(widget->mapTo(window_.get(), QPoint(0, 0)), widget->size()));
        }
        return rects;
    }
    void apply(const char* command) {
        window_->commands().execute(command);
        QApplication::processEvents();
    }
    static std::set<int> distinct(const std::vector<QRect>& rects,
                                  int (QRect::*coordinate)() const) {
        std::set<int> values;
        for (const QRect& rect : rects) {
            values.insert((rect.*coordinate)() / 8);
        }
        return values;
    }
};

TEST_F(MainWindowLayoutTest, TheColumnLayoutsPutTheGroupsSideBySideWithEqualWidths) {
    open_files();
    apply("workbench.action.editorLayoutThreeColumns");
    const auto rects = group_rects();
    ASSERT_EQ(rects.size(), 3u);
    EXPECT_EQ(distinct(rects, &QRect::top).size(), 1u);
    EXPECT_EQ(distinct(rects, &QRect::left).size(), 3u);
    for (const QRect& rect : rects) {
        EXPECT_NEAR(rect.width(), rects[0].width(), 12);
    }
    apply("workbench.action.editorLayoutSingle");
    EXPECT_EQ(group_count(), 1u);
}

TEST_F(MainWindowLayoutTest, TheRowLayoutsStackTheGroups) {
    open_files();
    apply("workbench.action.editorLayoutTwoRows");
    auto rects = group_rects();
    ASSERT_EQ(rects.size(), 2u);
    EXPECT_EQ(distinct(rects, &QRect::left).size(), 1u);
    EXPECT_EQ(distinct(rects, &QRect::top).size(), 2u);
    apply("workbench.action.editorLayoutThreeRows");
    EXPECT_EQ(group_count(), 3u);
}

TEST_F(MainWindowLayoutTest, TheGridHasTwoColumnsOfTwoRows) {
    open_files();
    apply("workbench.action.editorLayoutTwoByTwoGrid");
    const auto rects = group_rects();
    ASSERT_EQ(rects.size(), 4u);
    EXPECT_EQ(distinct(rects, &QRect::left).size(), 2u);
    EXPECT_EQ(distinct(rects, &QRect::top).size(), 2u);
}

TEST_F(MainWindowLayoutTest, TwoRowsRightAndTwoColumnsBottomHaveOneGroupAlongAnEdge) {
    open_files();
    apply("workbench.action.editorLayoutTwoRowsRight");
    auto rects = group_rects();
    ASSERT_EQ(rects.size(), 3u);
    EXPECT_GE(rects[0].height(), rects[1].height() + rects[2].height() - 12)
        << "the first group is as tall as the other two together";
    EXPECT_EQ(rects[1].left(), rects[2].left());
    EXPECT_LT(rects[1].top(), rects[2].top());
    apply("workbench.action.editorLayoutTwoColumnsBottom");
    rects = group_rects();
    ASSERT_EQ(rects.size(), 3u);
    EXPECT_GE(rects[0].width(), rects[1].width() + rects[2].width() - 12);
    EXPECT_EQ(rects[1].top(), rects[2].top());
}

TEST_F(MainWindowLayoutTest, FewerGroupsKeepEveryDocumentOpenInTheLastGroupThatStays) {
    open_files();
    apply("workbench.action.editorLayoutThreeColumns");
    window_->commands().execute("workbench.action.focusSecondEditorGroup");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("d.txt", "d").string())));
    const std::size_t documents = window_->open_document_count();
    apply("workbench.action.editorLayoutTwoColumns");
    EXPECT_EQ(group_count(), 2u);
    EXPECT_EQ(window_->open_document_count(), documents);
}

TEST_F(MainWindowLayoutTest, FlipLayoutTurnsColumnsIntoRowsWithTheShiftAltZeroKey) {
    open_files();
    apply("workbench.action.editorLayoutTwoColumns");
    ASSERT_EQ(distinct(group_rects(), &QRect::top).size(), 1u);
    key(Qt::Key_0, Qt::ShiftModifier | Qt::AltModifier);
    QApplication::processEvents();
    const auto rects = group_rects();
    EXPECT_EQ(distinct(rects, &QRect::left).size(), 1u);
    EXPECT_EQ(distinct(rects, &QRect::top).size(), 2u);
    key(Qt::Key_0, Qt::ShiftModifier | Qt::AltModifier);
    QApplication::processEvents();
    EXPECT_EQ(distinct(group_rects(), &QRect::top).size(), 1u);
}

TEST_F(MainWindowGroupKeysTest, ControlPageDownAndUpMoveThroughTheTabsAndWrap) {
    open_files();
    ASSERT_EQ(shown(), "c.txt");
    key(Qt::Key_PageDown, Qt::ControlModifier);
    EXPECT_NE(shown(), "c.txt");
    key(Qt::Key_PageUp, Qt::ControlModifier);
    EXPECT_EQ(shown(), "c.txt");
    key(Qt::Key_PageUp, Qt::ControlModifier);
    EXPECT_EQ(shown(), "b.txt");
}

TEST_F(MainWindowGroupKeysTest, TheNextEditorCrossesIntoTheNextGroupAndTheInGroupChordDoesNot) {
    open_files();
    window_->commands().execute("workbench.action.splitEditorRight");
    ASSERT_EQ(group_count(), 2u);
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("d.txt", "d").string())));
    window_->commands().execute("workbench.action.focusFirstEditorGroup");
    window_->commands().execute("workbench.action.nextEditorInGroup");
    EXPECT_EQ(window_->editor_layout().focused(), window_->editor_layout().group_ids()[0]);
    window_->commands().execute("workbench.action.previousEditorInGroup");
    // The last tab of the first group, then the next editor is in the second group.
    ASSERT_EQ(shown(), "c.txt");
    window_->commands().execute("workbench.action.nextEditor");
    EXPECT_EQ(window_->editor_layout().focused(), window_->editor_layout().group_ids()[1]);
    EXPECT_EQ(shown(), "d.txt");
    window_->commands().execute("workbench.action.nextEditor");
    EXPECT_EQ(window_->editor_layout().focused(), window_->editor_layout().group_ids()[0]);
    EXPECT_EQ(shown(), "Untitled-1") << "the first tab of the first group, after the last tab";
}

TEST_F(MainWindowGroupKeysTest, ControlTwoMakesTheSecondGroupAndControlOneGoesBack) {
    open_files();
    ASSERT_EQ(group_count(), 1u);
    key(Qt::Key_2, Qt::ControlModifier);
    EXPECT_EQ(group_count(), 2u);
    EXPECT_EQ(window_->editor_layout().focused(), window_->editor_layout().group_ids()[1]);
    key(Qt::Key_1, Qt::ControlModifier);
    EXPECT_EQ(window_->editor_layout().focused(), window_->editor_layout().group_ids()[0]);
    key(Qt::Key_5, Qt::ControlModifier);
    EXPECT_EQ(group_count(), 2u) << "only the group after the last one is made";
    key(Qt::Key_3, Qt::ControlModifier);
    EXPECT_EQ(group_count(), 3u);
    key(Qt::Key_2, Qt::ControlModifier);
    EXPECT_EQ(group_count(), 3u);
    EXPECT_EQ(window_->editor_layout().focused(), window_->editor_layout().group_ids()[1]);
}

TEST_F(MainWindowGroupKeysTest, TheChordsMoveTheFocusToTheGroupBesideTheFocusedOne) {
    open_files();
    key(Qt::Key_2, Qt::ControlModifier);
    const auto ids = window_->editor_layout().group_ids();
    ASSERT_EQ(ids.size(), 2u);
    chord(Qt::Key_Left);
    EXPECT_EQ(window_->editor_layout().focused(), ids[0]);
    chord(Qt::Key_Right);
    EXPECT_EQ(window_->editor_layout().focused(), ids[1]);
    chord(Qt::Key_Up);
    EXPECT_EQ(window_->editor_layout().focused(), ids[1]) << "no group above";
    window_->commands().execute("workbench.action.splitEditorDown");
    const auto three = window_->editor_layout().group_ids();
    ASSERT_EQ(three.size(), 3u);
    chord(Qt::Key_Up);
    EXPECT_EQ(window_->editor_layout().focused(), ids[1]);
    chord(Qt::Key_Down);
    EXPECT_EQ(window_->editor_layout().focused(), three[2]);
}

TEST_F(MainWindowGroupKeysTest, TheRecentlyUsedCommandsWalkBackThroughTheTabsInTheOrderOfUse) {
    open_files();
    window_->commands().execute("workbench.action.openPreviousRecentlyUsedEditor");
    EXPECT_EQ(shown(), "b.txt");
    window_->commands().execute("workbench.action.openPreviousRecentlyUsedEditor");
    EXPECT_EQ(shown(), "a.txt");
    window_->commands().execute("workbench.action.openNextRecentlyUsedEditor");
    EXPECT_EQ(shown(), "b.txt");
    window_->commands().execute("workbench.action.openNextRecentlyUsedEditorInGroup");
    EXPECT_EQ(shown(), "b.txt") << "a history of the group starts at the tab that is shown";
    window_->commands().execute("workbench.action.openPreviousRecentlyUsedEditorInGroup");
    EXPECT_NE(shown(), "b.txt");
}

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

class MainWindowLinesTest : public MainWindowOccurrencesTest {
protected:
    void open_lines(const std::string& text) {
        create_active_window();
        ASSERT_TRUE(window_->open_file(QString::fromStdString(write("lines.txt", text).string())));
        window_->view()->setFocus();
    }
};

TEST_F(MainWindowLinesTest, ShiftAltDownAndUpAddCursorsAndTypingEditsEveryLine) {
    open_lines("abc\ndef\nghi");
    put_caret(1);
    QTest::keyClick(window_->view(), Qt::Key_Down, Qt::ShiftModifier | Qt::AltModifier);
    QTest::keyClick(window_->view(), Qt::Key_Down, Qt::ShiftModifier | Qt::AltModifier);
    EXPECT_EQ(selections(),
              (std::vector<core::Selection>{core::Selection::caret(1), core::Selection::caret(5),
                                            core::Selection::caret(9)}));
    type("-");
    EXPECT_EQ(current_text(), "a-bc\nd-ef\ng-hi");
}

TEST_F(MainWindowLinesTest, CtrlShiftUpIsTheSecondKeyOfAddCursorAbove) {
    open_lines("abc\ndef");
    put_caret(5);
    QTest::keyClick(window_->view(), Qt::Key_Up, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_EQ(selections(),
              (std::vector<core::Selection>{core::Selection::caret(1), core::Selection::caret(5)}));
}

TEST_F(MainWindowLinesTest, ShiftAltIPutsACaretAtTheEndOfEachSelectedLine) {
    open_lines("ab\ncd\nef");
    ASSERT_TRUE(window_->current_document()
                    ->set_selections(core::SelectionSet::from({core::Selection{1, 7}}))
                    .has_value());
    window_->view()->document_changed_elsewhere();
    QTest::keyClick(window_->view(), Qt::Key_I, Qt::ShiftModifier | Qt::AltModifier);
    EXPECT_EQ(selections(),
              (std::vector<core::Selection>{core::Selection::caret(2), core::Selection::caret(5),
                                            core::Selection::caret(7)}));
}

TEST_F(MainWindowLinesTest, AddPreviousOccurrenceSelectsTheWordThenAddsTheMatchesBeforeIt) {
    open_lines("foo bar foo baz foo");
    put_caret(17);
    window_->commands().execute("editor.action.addSelectionToPreviousFindMatch");
    EXPECT_EQ(selections(), (std::vector<core::Selection>{{16, 19}}));
    window_->commands().execute("editor.action.addSelectionToPreviousFindMatch");
    EXPECT_EQ(selections(), (std::vector<core::Selection>{{8, 11}, {16, 19}}));
    EXPECT_EQ(window_->current_document()->selections().primary(), (core::Selection{8, 11}));
    window_->commands().execute("editor.action.addSelectionToPreviousFindMatch");
    window_->commands().execute("editor.action.addSelectionToPreviousFindMatch");
    EXPECT_EQ(selections(), (std::vector<core::Selection>{{0, 3}, {8, 11}, {16, 19}}))
        << "every match is taken";
}

TEST_F(MainWindowLinesTest, AltUpAndAltDownMoveTheLineAndEachMoveIsOneUndoStep) {
    open_lines("a\nb\nc\nd");
    put_caret(2);
    QTest::keyClick(window_->view(), Qt::Key_Down, Qt::AltModifier);
    EXPECT_EQ(current_text(), "a\nc\nb\nd");
    EXPECT_EQ(selections(), (std::vector<core::Selection>{core::Selection::caret(4)}));
    QTest::keyClick(window_->view(), Qt::Key_Down, Qt::AltModifier);
    EXPECT_EQ(current_text(), "a\nc\nd\nb");
    QTest::keyClick(window_->view(), Qt::Key_Up, Qt::AltModifier);
    EXPECT_EQ(current_text(), "a\nc\nb\nd");
    window_->view()->undo();
    EXPECT_EQ(current_text(), "a\nc\nd\nb");
    window_->view()->undo();
    EXPECT_EQ(current_text(), "a\nc\nb\nd");
}

TEST_F(MainWindowLinesTest, TheCopyKeysCopyTheLineUpOrDown) {
    open_lines("a\nb\nc");
    put_caret(2);
    // VS Code's Linux keys add Ctrl to Shift+Alt, which other systems use for something else.
    QTest::keyClick(window_->view(), Qt::Key_Down,
                    Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier);
    EXPECT_EQ(current_text(), "a\nb\nb\nc");
    EXPECT_EQ(selections(), (std::vector<core::Selection>{core::Selection::caret(4)}));
    QTest::keyClick(window_->view(), Qt::Key_Up,
                    Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier);
    EXPECT_EQ(current_text(), "a\nb\nb\nb\nc");
    EXPECT_EQ(selections(), (std::vector<core::Selection>{core::Selection::caret(4)}));
}

TEST_F(MainWindowLinesTest,
       DuplicateSelectionCopiesTheTextOrTheLineAndTheCommandsWorkOnEveryCursor) {
    open_lines("one\ntwo");
    ASSERT_TRUE(window_->current_document()
                    ->set_selections(core::SelectionSet::from({core::Selection{0, 3}}))
                    .has_value());
    window_->view()->document_changed_elsewhere();
    window_->commands().execute("editor.action.duplicateSelection");
    EXPECT_EQ(current_text(), "oneone\ntwo");
    EXPECT_EQ(selections(), (std::vector<core::Selection>{{3, 6}}));
    put_caret(8);
    window_->commands().execute("editor.action.duplicateSelection");
    EXPECT_EQ(current_text(), "oneone\ntwo\ntwo");
    EXPECT_EQ(selections(), (std::vector<core::Selection>{core::Selection::caret(12)}));

    ASSERT_TRUE(window_->current_document()
                    ->set_selections(core::SelectionSet::from(
                        {core::Selection::caret(1), core::Selection::caret(8)}))
                    .has_value());
    window_->view()->document_changed_elsewhere();
    window_->commands().execute("editor.action.moveLinesDownAction");
    EXPECT_EQ(current_text(), "two\noneone\ntwo");
}

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

TEST_F(MainWindowKeysTest, F11TogglesFullscreenAndTheViewMenuItemRunsTheSameCommand) {
    create_active_window();
    QTest::keyClick(window_->view(), Qt::Key_F11);
    QApplication::processEvents();
    EXPECT_TRUE(window_->isFullScreen());
    QAction* item = menu_action("workbench.action.toggleFullScreen");
    ASSERT_NE(item, nullptr);
    EXPECT_TRUE(item->isEnabled());
    EXPECT_TRUE(item->text().endsWith("\tF11")) << item->text().toStdString();
    item->trigger();
    QApplication::processEvents();
    EXPECT_FALSE(window_->isFullScreen());
}

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

TEST_F(MainWindowContributionsTest,
       TheContributionsOfTheExtensionsAreReadAgainWhenTheHostRestarts) {
    const auto copy = scratch_ / "contributions";
    std::filesystem::copy(ANVIL_CONTRIBUTIONS_EXTENSION, copy,
                          std::filesystem::copy_options::recursive);
    extension_dirs_ = {copy};
    std::filesystem::create_directories(scratch_ / "project");
    create_window(true);
    if (!window_) {
        return;
    }
    ASSERT_TRUE(shows(palette_labels(), "Keyed"));
    window_->command_palette()->close();

    std::string manifest = *platform::read_file(copy / "package.json");
    const auto replace = [&](const std::string& from, const std::string& to) {
        const auto at = manifest.find(from);
        ASSERT_NE(at, std::string::npos) << from;
        manifest.replace(at, from.size(), to);
    };
    replace(R"("title": "Keyed")", R"("title": "Keyed Renamed")");
    replace(R"("linux": "ctrl+alt+8")", R"("linux": "ctrl+alt+6")");
    ASSERT_TRUE(platform::write_file(copy / "package.json", manifest).has_value());

    ASSERT_TRUE(window_->open_folder(scratch_ / "project"));
    ASSERT_TRUE(wait_host_ready());

    const auto labels = palette_labels();
    EXPECT_TRUE(shows(labels, "Keyed Renamed"));
    EXPECT_FALSE(shows(labels, "Keyed"));
    window_->command_palette()->close();
    window_->activateWindow();
    ASSERT_TRUE(QTest::qWaitForWindowActive(window_.get()));
    window_->view()->setFocus();
    QTest::keyClick(window_->view(), Qt::Key_6, Qt::ControlModifier | Qt::AltModifier);
    ASSERT_TRUE(wait_until([&] { return current_text() == "[keyed]"; })) << current_text();
}

TEST_F(MainWindowContributionsTest, TheCommandsOfTheWindowAreSharedWithTheNewHost) {
    std::filesystem::create_directories(scratch_ / "project");
    create_window(true);
    if (!window_) {
        return;
    }
    ASSERT_TRUE(window_->open_folder(scratch_ / "project"));
    ASSERT_TRUE(wait_host_ready());

    std::optional<exthost::CallResult> result;
    window_->bridge().host()->commands().execute(
        "workbench.action.files.newUntitledFile", "[]",
        [&](exthost::CallResult r) { result = std::move(r); });
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->has_value());
    EXPECT_EQ(window_->open_document_count(), 2U);
}

TEST_F(MainWindowTest, ThemesOfExtensionsAreListedOnceAfterARestart) {
    extension_dirs_ = {ANVIL_THEMES_EXTENSION};
    std::filesystem::create_directories(scratch_ / "project");
    create_window(true);
    if (!window_) {
        return;
    }
    const std::size_t count = window_->themes().themes().size();
    const std::string current = window_->themes().current();
    const auto theme_warnings = [&] {
        return std::ranges::count(logger_->entries(), "theme",
                                  &core::RecordingLogger::Entry::source);
    };
    const auto warnings = theme_warnings();

    ASSERT_TRUE(window_->open_folder(scratch_ / "project"));
    ASSERT_TRUE(wait_host_ready());

    EXPECT_EQ(window_->themes().themes().size(), count);
    EXPECT_EQ(window_->themes().current(), current);
    EXPECT_EQ(theme_warnings(), warnings);
}

class PartPositionsTest : public MainWindowTest {
protected:
    void start_with(const std::string& settings) {
        settings_file_ = write("settings.json", settings);
        create_window();
        window_->resize(1000, 700);
        window_->show();
        QApplication::processEvents();
    }

    void add_container() {
        QPixmap pixmap(24, 24);
        pixmap.fill(Qt::black);
        ui::ViewContainer container;
        container.id = "workbench.view.explorer";
        container.title = "Explorer";
        container.icon = QIcon(pixmap);
        container.create = [] { return new QLabel("files"); };
        window_->view_containers().add(std::move(container));
        QApplication::processEvents();
    }

    std::string settings_text() const {
        return *platform::read_file(settings_file_);
    }
    QString title_of(const std::string& command) {
        QAction* action = menu_action(command);
        return action ? action->property("anvil.title").toString().remove(QLatin1Char('&'))
                      : QString();
    }
};

TEST_F(PartPositionsTest, TheStatusBarMenuItemShowsAndHidesTheStatusBarAndWritesTheSetting) {
    start_with("{}");
    QAction* item = menu_action("workbench.action.toggleStatusbarVisibility");
    ASSERT_NE(item, nullptr);
    EXPECT_TRUE(item->isEnabled());
    EXPECT_TRUE(item->isChecked());
    EXPECT_TRUE(window_->workbench()->is_visible(ui::Part::status_bar));

    item->trigger();
    EXPECT_FALSE(window_->workbench()->is_visible(ui::Part::status_bar));
    EXPECT_NE(settings_text().find("\"workbench.statusBar.visible\": false"), std::string::npos);
    EXPECT_FALSE(menu_action("workbench.action.toggleStatusbarVisibility")->isChecked());

    menu_action("workbench.action.toggleStatusbarVisibility")->trigger();
    EXPECT_TRUE(window_->workbench()->is_visible(ui::Part::status_bar));
    EXPECT_TRUE(menu_action("workbench.action.toggleStatusbarVisibility")->isChecked());
}

TEST_F(PartPositionsTest, TheWindowStartsWithThePartsWhereTheSettingsPutThem) {
    start_with(R"j({"workbench.statusBar.visible": false,
                    "workbench.sideBar.location": "right",
                    "workbench.activityBar.location": "hidden"})j");
    add_container();
    EXPECT_FALSE(window_->workbench()->is_visible(ui::Part::status_bar));
    EXPECT_EQ(window_->workbench()->sidebar_side(), ui::Side::right);
    EXPECT_FALSE(window_->workbench()->is_visible(ui::Part::activity_bar));
    EXPECT_TRUE(menu_action("workbench.action.activityBarLocation.hide")->isChecked());
    EXPECT_FALSE(menu_action("workbench.action.activityBarLocation.default")->isChecked());
}

TEST_F(PartPositionsTest, ASettingsFileEditedByHandMovesTheParts) {
    start_with("{}");
    ASSERT_TRUE(window_->open_file(QString::fromStdString(settings_file_.string())));
    EXPECT_EQ(window_->workbench()->sidebar_side(), ui::Side::left);
    window_->change_setting("workbench.sideBar.location", "right");
    EXPECT_EQ(window_->workbench()->sidebar_side(), ui::Side::right);
    window_->change_setting("workbench.sideBar.location", "left");
    EXPECT_EQ(window_->workbench()->sidebar_side(), ui::Side::left);
}

TEST_F(PartPositionsTest, TheSideBarMenuItemNamesTheSideItMovesToAndMovesIt) {
    start_with("{}");
    add_container();
    window_->sidebar()->activate("workbench.view.explorer");
    QApplication::processEvents();
    EXPECT_EQ(title_of("workbench.action.toggleSidebarPosition"), "Move Primary Side Bar Right");

    menu_action("workbench.action.toggleSidebarPosition")->trigger();
    QApplication::processEvents();
    EXPECT_EQ(window_->workbench()->sidebar_side(), ui::Side::right);
    EXPECT_NE(settings_text().find("\"workbench.sideBar.location\": \"right\""), std::string::npos);
    EXPECT_EQ(title_of("workbench.action.toggleSidebarPosition"), "Move Primary Side Bar Left");
    const QWidget* sidebar = window_->workbench()->sidebar();
    const QWidget* activity = window_->workbench()->activity_bar();
    EXPECT_EQ(activity->mapTo(window_->workbench(), QPoint(activity->width(), 0)).x(),
              window_->workbench()->width());
    EXPECT_LT(sidebar->mapTo(window_->workbench(), QPoint(0, 0)).x(),
              activity->mapTo(window_->workbench(), QPoint(0, 0)).x());

    menu_action("workbench.action.toggleSidebarPosition")->trigger();
    EXPECT_EQ(window_->workbench()->sidebar_side(), ui::Side::left);
    EXPECT_EQ(title_of("workbench.action.toggleSidebarPosition"), "Move Primary Side Bar Right");
}

TEST_F(PartPositionsTest, TheActivityBarPositionItemsHideAndShowTheActivityBar) {
    start_with("{}");
    add_container();
    EXPECT_TRUE(window_->workbench()->is_visible(ui::Part::activity_bar));
    EXPECT_TRUE(menu_action("workbench.action.activityBarLocation.default")->isChecked());
    EXPECT_FALSE(menu_action("workbench.action.activityBarLocation.hide")->isChecked());

    menu_action("workbench.action.activityBarLocation.hide")->trigger();
    EXPECT_FALSE(window_->workbench()->is_visible(ui::Part::activity_bar));
    EXPECT_NE(settings_text().find("\"workbench.activityBar.location\": \"hidden\""),
              std::string::npos);
    EXPECT_TRUE(menu_action("workbench.action.activityBarLocation.hide")->isChecked());
    EXPECT_FALSE(menu_action("workbench.action.activityBarLocation.default")->isChecked());

    window_->sidebar()->activate("workbench.view.explorer");
    EXPECT_TRUE(window_->workbench()->is_visible(ui::Part::sidebar));
    EXPECT_FALSE(window_->workbench()->is_visible(ui::Part::activity_bar));

    menu_action("workbench.action.activityBarLocation.default")->trigger();
    EXPECT_TRUE(window_->workbench()->is_visible(ui::Part::activity_bar));
    EXPECT_TRUE(menu_action("workbench.action.activityBarLocation.default")->isChecked());
}

TEST_F(PartPositionsTest, TheToggleCommandSwitchesTheActivityBarBetweenHiddenAndTheDefault) {
    start_with("{}");
    add_container();
    window_->commands().execute("workbench.action.toggleActivityBarVisibility");
    EXPECT_FALSE(window_->workbench()->is_visible(ui::Part::activity_bar));
    window_->commands().execute("workbench.action.toggleActivityBarVisibility");
    EXPECT_TRUE(window_->workbench()->is_visible(ui::Part::activity_bar));
}

TEST_F(PartPositionsTest, TheTopAndBottomActivityBarItemsAreStillUnavailable) {
    start_with("{}");
    for (const char* id : {"workbench.action.activityBarLocation.top",
                           "workbench.action.activityBarLocation.bottom"}) {
        QAction* item = menu_action(id);
        ASSERT_NE(item, nullptr) << id;
        EXPECT_FALSE(item->isEnabled()) << id;
    }
}

TEST_F(PartPositionsTest, ThePanelPositionItemsMoveThePanelAndTheirCheckMarkFollows) {
    start_with("{}");
    window_->workbench()->set_visible(ui::Part::panel, true);
    QApplication::processEvents();

    struct Case {
        const char* id;
        ui::PanelPosition position;
    };
    const Case cases[] = {
        {"workbench.action.positionPanelTop", ui::PanelPosition::top},
        {"workbench.action.positionPanelLeft", ui::PanelPosition::left},
        {"workbench.action.positionPanelRight", ui::PanelPosition::right},
        {"workbench.action.positionPanelBottom", ui::PanelPosition::bottom},
    };
    EXPECT_TRUE(menu_action("workbench.action.positionPanelBottom")->isChecked());
    for (const Case& moved : cases) {
        QAction* item = menu_action(moved.id);
        ASSERT_NE(item, nullptr) << moved.id;
        EXPECT_TRUE(item->isEnabled()) << moved.id;
        item->trigger();
        QApplication::processEvents();
        EXPECT_EQ(window_->workbench()->panel_position(), moved.position) << moved.id;
        for (const Case& other : cases) {
            EXPECT_EQ(menu_action(other.id)->isChecked(), other.position == moved.position)
                << moved.id << " " << other.id;
        }
    }
}

TEST_F(PartPositionsTest, ThePanelCommandsPlaceThePanelAroundTheEditor) {
    start_with("{}");
    window_->workbench()->set_visible(ui::Part::panel, true);
    QApplication::processEvents();
    const auto panel = [&] { return window_->workbench()->panel(); };
    const auto editor = [&] { return window_->view(); };
    const auto origin = [&](const QWidget* widget) {
        return widget->mapTo(window_.get(), QPoint(0, 0));
    };

    window_->commands().execute("workbench.action.positionPanelTop");
    QApplication::processEvents();
    EXPECT_LT(origin(panel()).y(), origin(editor()).y());
    window_->commands().execute("workbench.action.positionPanelLeft");
    QApplication::processEvents();
    EXPECT_LT(origin(panel()).x(), origin(editor()).x());
    window_->commands().execute("workbench.action.positionPanelRight");
    QApplication::processEvents();
    EXPECT_GT(origin(panel()).x(), origin(editor()).x());
    window_->commands().execute("workbench.action.positionPanelBottom");
    QApplication::processEvents();
    EXPECT_GT(origin(panel()).y(), origin(editor()).y());
}

TEST_F(PartPositionsTest, MovingThePartsKeepsTheFocusInTheEditor) {
    start_with("{}");
    window_->activateWindow();
    ASSERT_TRUE(QTest::qWaitForWindowActive(window_.get()));
    window_->view()->setFocus();
    window_->workbench()->set_visible(ui::Part::panel, true);
    window_->workbench()->set_visible(ui::Part::sidebar, true);
    window_->view()->setFocus();
    ASSERT_TRUE(window_->view()->hasFocus());

    window_->commands().execute("workbench.action.toggleSidebarPosition");
    EXPECT_TRUE(window_->view()->hasFocus());
    window_->commands().execute("workbench.action.positionPanelLeft");
    EXPECT_TRUE(window_->view()->hasFocus());
    type("x");
    EXPECT_EQ(current_text(), "x");
}

TEST_F(PartPositionsTest, TheSideBarButtonOfTheTitleBarShowsTheSideTheSideBarIsOn) {
    custom_title_bar_ = true;
    start_with("{}");
    ASSERT_NE(window_->title_bar(), nullptr);
    ASSERT_TRUE(window_->title_bar()->sidebar_button()->isVisible());
    const auto icon_pixels = [&] {
        return window_->title_bar()->sidebar_button()->grab().toImage();
    };
    for (const bool shown : {false, true}) {
        window_->workbench()->set_visible(ui::Part::sidebar, shown);
        QApplication::processEvents();
        const QImage left = icon_pixels();
        window_->commands().execute("workbench.action.toggleSidebarPosition");
        QApplication::processEvents();
        EXPECT_NE(icon_pixels(), left) << shown;
        window_->commands().execute("workbench.action.toggleSidebarPosition");
        QApplication::processEvents();
        EXPECT_EQ(icon_pixels(), left) << shown;
    }
}

TEST_F(PartPositionsTest, RendersTheWindowWithTheSideBarAndThePanelOnTheRight) {
    start_with(R"j({"workbench.sideBar.location": "right"})j");
    ASSERT_TRUE(window_->open_file(
        QString::fromStdString(write("sample.txt", "hello, world\nsecond line\n").string())));
    add_container();
    window_->sidebar()->activate("workbench.view.explorer");
    window_->workbench()->set_visible(ui::Part::panel, true);
    window_->commands().execute("workbench.action.positionPanelRight");
    QApplication::processEvents();
    const QImage image = window_->grab().toImage();
    EXPECT_GT(image.width(), 100);
    if (const char* directory = std::getenv("ANVIL_SCREENSHOT_DIR")) {
        image.save(QString::fromUtf8(directory) + "/main_window_parts_on_the_right.png");
    }
}

class LanguageTest : public MainWindowTest {
protected:
    void SetUp() override {
        MainWindowTest::SetUp();
        extension_dirs_ = {ANVIL_DOCUMENTS_EXTENSION, ANVIL_LANGUAGES_EXTENSION};
    }

    // What the status bar shows for the language of the shown document.
    QString shown_language() const {
        const auto* item = window_->status_bar().item("status.editor.language");
        return item ? item->text : QString("(none)");
    }

    // The language id that extensions see for the active editor.
    std::string host_language() {
        return run("anvil.docs.active")["languageId"].get<std::string>();
    }

    bool open(const std::filesystem::path& path) {
        return window_->open_file(QString::fromStdString(path.string()));
    }

    // Waits for the language that extensions see to be `id`.
    bool host_language_becomes(const std::string& id) {
        return wait_until([&] { return host_language() == id; });
    }
};

TEST_F(LanguageTest, DocumentsHaveTheLanguageOfTheirFileForTheWindowAndForExtensions) {
    create_window(true);
    if (!window_) {
        return;
    }
    struct Case {
        const char* name;
        const char* content;
        const char* id;
        const char* shown;
    };
    const Case cases[] = {
        {"a.fixture", "", "fixturelang", "Fixture Language"},
        {"fixturefile", "", "fixturelang", "Fixture Language"},
        {"fixture-x.cfg", "", "fixturelang", "Fixture Language"},
        {"a.ts", "", "typescript", "TypeScript"},
        {"b.unknown", "", "plaintext", "Plain Text"},
        {"notes.txt", "", "plaintext", "Plain Text"},
        {"fixturescript", "#!fixture\nx", "fixturelang", "Fixture Language"},
        {"pyscript", "#!/usr/bin/env python3\nx", "python", "Python"},
        {"later", "x\n#!/usr/bin/env python3", "plaintext", "Plain Text"},
    };
    for (const Case& c : cases) {
        ASSERT_TRUE(open(write(c.name, c.content))) << c.name;
        EXPECT_EQ(host_language(), c.id) << c.name;
        EXPECT_EQ(shown_language(), c.shown) << c.name;
    }
}

TEST_F(LanguageTest, TheFirstLineOfEachDocumentIsTheOneThatIsRead) {
    create_window(true);
    if (!window_) {
        return;
    }
    ASSERT_TRUE(open(write("first", "#!/usr/bin/env python3\n")));
    ASSERT_TRUE(open(write("second", "plain\n")));
    EXPECT_EQ(host_language(), "plaintext");
    ASSERT_TRUE(open(write("first", "#!/usr/bin/env python3\n")));
    EXPECT_EQ(host_language(), "python");
}

TEST_F(LanguageTest, SavingAsAnotherNameChangesTheLanguageEverywhere) {
    create_window(true);
    if (!window_) {
        return;
    }
    ASSERT_TRUE(open(write("a.txt", "abc")));
    ASSERT_EQ(host_language(), "plaintext");
    ASSERT_TRUE(window_->save_as(QString::fromStdString((scratch_ / "b.fixture").string())));
    EXPECT_EQ(host_language(), "fixturelang");
    EXPECT_EQ(shown_language(), "Fixture Language");
    ASSERT_TRUE(window_->save_as(QString::fromStdString((scratch_ / "c.ts").string())));
    EXPECT_EQ(host_language(), "typescript");
    EXPECT_EQ(shown_language(), "TypeScript");
}

TEST_F(LanguageTest, AnAssociationOfTheUserOverridesTheExtensionsAndUpdatesOpenDocuments) {
    create_window(true);
    if (!window_) {
        return;
    }
    ASSERT_TRUE(open(write("a.fixture", "abc")));
    ASSERT_TRUE(open(write("b.txt", "abc")));
    EXPECT_EQ(shown_language(), "Plain Text");

    window_->change_setting("files.associations",
                            json{{"*.fixture", "markdown"}, {"*.txt", "python"}});
    EXPECT_TRUE(host_language_becomes("python"));
    EXPECT_EQ(shown_language(), "Python");
    ASSERT_TRUE(open(write("a.fixture", "abc")));
    EXPECT_EQ(host_language(), "markdown");
    EXPECT_EQ(shown_language(), "Markdown");

    window_->change_setting("files.associations", json::object());
    EXPECT_TRUE(host_language_becomes("fixturelang"));
    EXPECT_EQ(shown_language(), "Fixture Language");
    ASSERT_TRUE(open(write("b.txt", "abc")));
    EXPECT_EQ(host_language(), "plaintext");
}

TEST_F(LanguageTest, AnAssociationOfALanguageThatDoesNotExistGivesPlainText) {
    create_window(true);
    if (!window_) {
        return;
    }
    ASSERT_TRUE(open(write("a.fixture", "abc")));
    window_->change_setting("files.associations", json{{"*.fixture", "nosuchlanguage"}});
    EXPECT_TRUE(host_language_becomes("plaintext"));
    EXPECT_EQ(shown_language(), "Plain Text");
}

TEST_F(LanguageTest, AnEntryOfTheAssociationsThatIsNotALanguageIdIsLoggedAndTheOthersApply) {
    create_window(true);
    if (!window_) {
        return;
    }
    ASSERT_TRUE(open(write("a.txt", "abc")));
    window_->change_setting("files.associations", json{{"*.txt", "python"}, {"*.bad", 3}});
    EXPECT_TRUE(host_language_becomes("python"));
    EXPECT_TRUE(std::ranges::any_of(logger_->entries(), [](const auto& entry) {
        return entry.message.find("*.bad") != std::string::npos;
    }));
}

TEST_F(LanguageTest, ADocumentThatAnExtensionCreatesWithALanguageKeepsIt) {
    create_window(true);
    if (!window_) {
        return;
    }
    ASSERT_TRUE(open(write("a.txt", "abc")));
    // The window's own untitled document, which has the name that the extension's would take.
    ASSERT_TRUE(window_->close_document(0));
    const json created = run("anvil.docs.create", json::array({"python", "x = 1"}));
    ASSERT_EQ(created["languageId"], "python");
    ASSERT_TRUE(wait_until([&] { return shown_language() == "Python"; }));

    window_->change_setting("files.associations", json{{"*.txt", "markdown"}});
    ASSERT_TRUE(open(write("a.txt", "abc")));
    EXPECT_TRUE(host_language_becomes("markdown"));
    ASSERT_TRUE(window_->close_document(0));
    EXPECT_EQ(host_language(), "python");
    EXPECT_EQ(shown_language(), "Python");
}

TEST_F(LanguageTest, TheLanguageOfTheShownDocumentIsInTheStatusBarWithoutAnExtensionHost) {
    create_window();
    EXPECT_EQ(shown_language(), "Plain Text");
    ASSERT_TRUE(open(write("a.ts", "abc")));
    EXPECT_EQ(shown_language(), "TypeScript");
    window_->change_setting("files.associations", json{{"*.ts", "python"}});
    EXPECT_EQ(shown_language(), "Python");
    ASSERT_TRUE(window_->save_as(QString::fromStdString((scratch_ / "b.md").string())));
    EXPECT_EQ(shown_language(), "Markdown");
}

class NavigationHistoryTest : public MainWindowTest {
protected:
    static std::string numbered_lines(int count) {
        std::string text;
        for (int i = 0; i < count; ++i) {
            text += "line " + std::to_string(i) + "\n";
        }
        return text;
    }

    void open(const std::filesystem::path& path) {
        ASSERT_TRUE(window_->open_file(QString::fromStdString(path.string())));
    }

    std::size_t offset_of_line(std::size_t line) const {
        return window_->current_document()->text().offset_at({line, 0});
    }
    void go_to_line(std::size_t line) {
        window_->view()->select_range(core::Selection::caret(offset_of_line(line)));
    }
    std::size_t caret_line() const {
        const core::Document& document = *window_->current_document();
        return document.text().position_at(document.selections().primary().active).line;
    }
    std::string shown_name() const {
        return window_->current_uri().file_path()->filename().string();
    }
    bool close_tab_of(const core::Uri& uri) {
        const auto& tabs = window_->editor_layout().group(window_->editor_layout().focused())->tabs;
        const auto at = std::ranges::find(tabs, uri);
        return at != tabs.end() &&
               window_->close_document(static_cast<std::size_t>(at - tabs.begin()));
    }
    bool enabled(const char* command) {
        QAction* item = menu_action(command);
        return item && item->isEnabled();
    }
    void run(const char* command) {
        QAction* item = menu_action(command);
        ASSERT_NE(item, nullptr) << command;
        ASSERT_TRUE(item->isEnabled()) << command;
        item->trigger();
    }
};

TEST_F(NavigationHistoryTest, TheGoMenuItemsAreDisabledUntilThereIsAPlaceToGoTo) {
    create_window();
    for (const char* command : {"workbench.action.navigateBack", "workbench.action.navigateForward",
                                "workbench.action.navigateToLastEditLocation"}) {
        QAction* item = menu_action(command);
        ASSERT_NE(item, nullptr) << command;
        EXPECT_FALSE(item->isEnabled()) << command;
    }
}

TEST_F(NavigationHistoryTest, BackAndForwardReturnToTheLinesTheCaretWasLeftOn) {
    create_window();
    open(write("a.txt", numbered_lines(200)));
    go_to_line(80);
    go_to_line(150);
    ASSERT_EQ(caret_line(), 150U);

    run("workbench.action.navigateBack");
    EXPECT_EQ(shown_name(), "a.txt");
    EXPECT_EQ(caret_line(), 80U);
    run("workbench.action.navigateBack");
    EXPECT_EQ(caret_line(), 0U);
    EXPECT_TRUE(enabled("workbench.action.navigateBack"));

    run("workbench.action.navigateForward");
    EXPECT_EQ(caret_line(), 80U);
    run("workbench.action.navigateForward");
    EXPECT_EQ(caret_line(), 150U);
    EXPECT_FALSE(enabled("workbench.action.navigateForward"));
}

TEST_F(NavigationHistoryTest, SmallMovesInTheSameAreaAddNoPlace) {
    create_window();
    open(write("a.txt", numbered_lines(200)));
    const std::size_t before = window_->navigation_history().selections().entries().size();
    for (const std::size_t line : {3U, 7U, 12U, 5U, 9U}) {
        go_to_line(line);
    }
    EXPECT_EQ(window_->navigation_history().selections().entries().size(), before);
    go_to_line(60);
    EXPECT_EQ(window_->navigation_history().selections().entries().size(), before + 1);
}

TEST_F(NavigationHistoryTest, WalkingTheHistoryAddsNoPlaces) {
    create_window();
    open(write("a.txt", numbered_lines(200)));
    go_to_line(80);
    open(write("b.txt", numbered_lines(200)));
    go_to_line(120);
    const auto before = window_->navigation_history().selections().entries();

    for (int round = 0; round < 2; ++round) {
        while (enabled("workbench.action.navigateBack")) {
            run("workbench.action.navigateBack");
        }
        while (enabled("workbench.action.navigateForward")) {
            run("workbench.action.navigateForward");
        }
    }
    EXPECT_EQ(window_->navigation_history().selections().entries(), before);
    EXPECT_EQ(shown_name(), "b.txt");
    EXPECT_EQ(caret_line(), 120U);
}

TEST_F(NavigationHistoryTest, BackSwitchesDocumentsAndForwardComesBack) {
    create_window();
    open(write("a.txt", numbered_lines(200)));
    go_to_line(40);
    open(write("b.txt", numbered_lines(200)));
    go_to_line(90);

    run("workbench.action.navigateBack");
    EXPECT_EQ(shown_name(), "b.txt");
    EXPECT_EQ(caret_line(), 0U);
    run("workbench.action.navigateBack");
    EXPECT_EQ(shown_name(), "a.txt");
    EXPECT_EQ(caret_line(), 40U);
    run("workbench.action.navigateForward");
    EXPECT_EQ(shown_name(), "b.txt");
}

TEST_F(NavigationHistoryTest, ADocumentThatWasClosedIsOpenedAgain) {
    create_window();
    const auto a = write("a.txt", numbered_lines(200));
    open(a);
    go_to_line(70);
    open(write("b.txt", numbered_lines(10)));
    ASSERT_TRUE(close_tab_of(core::Uri::file(a)));
    ASSERT_TRUE(window_->editor_layout().locations(core::Uri::file(a)).empty());

    run("workbench.action.navigateBack");
    EXPECT_EQ(shown_name(), "a.txt");
    EXPECT_EQ(caret_line(), 70U);
}

TEST_F(NavigationHistoryTest, ADeletedFileIsForgottenAndTheWindowStaysWhereItIs) {
    create_window();
    const auto a = write("a.txt", numbered_lines(200));
    open(a);
    go_to_line(70);
    open(write("b.txt", numbered_lines(10)));
    ASSERT_TRUE(close_tab_of(core::Uri::file(a)));
    std::filesystem::remove(a);

    run("workbench.action.navigateBack");
    EXPECT_EQ(shown_name(), "b.txt");
    for (const auto& entry : window_->navigation_history().selections().entries()) {
        EXPECT_NE(entry.uri, core::Uri::file(a));
    }
}

TEST_F(NavigationHistoryTest, AClosedUntitledDocumentLeavesNoPlaces) {
    create_window();
    window_->commands().execute("workbench.action.files.newUntitledFile");
    const core::Uri untitled = window_->current_uri();
    ASSERT_FALSE(untitled.file_path());
    open(write("a.txt", numbered_lines(10)));
    ASSERT_TRUE(close_tab_of(untitled));
    for (const auto& entry : window_->navigation_history().selections().entries()) {
        EXPECT_NE(entry.uri, untitled);
    }
}

TEST_F(NavigationHistoryTest, ThePlacesOfAClosedGroupAreForgotten) {
    create_window();
    open(write("a.txt", numbered_lines(200)));
    go_to_line(60);
    window_->commands().execute("workbench.action.splitEditorRight");
    const workbench::GroupId second = window_->editor_layout().focused();
    open(write("b.txt", numbered_lines(200)));
    go_to_line(90);
    window_->editor_layout().focus(second);
    window_->commands().execute("workbench.action.closeGroup");
    ASSERT_EQ(window_->editor_layout().group_ids().size(), 1U);
    for (const auto& entry : window_->navigation_history().selections().entries()) {
        EXPECT_NE(entry.group, second);
    }
}

TEST_F(NavigationHistoryTest, BackShowsADocumentInTheGroupThatHadIt) {
    create_window();
    open(write("a.txt", numbered_lines(200)));
    go_to_line(60);
    const workbench::GroupId first = window_->editor_layout().focused();
    window_->commands().execute("workbench.action.splitEditorRight");
    const workbench::GroupId second = window_->editor_layout().focused();
    ASSERT_NE(first, second);
    open(write("b.txt", numbered_lines(200)));

    while (enabled("workbench.action.navigateBack") &&
           window_->editor_layout().focused() != first) {
        run("workbench.action.navigateBack");
    }
    EXPECT_EQ(window_->editor_layout().focused(), first);
    EXPECT_EQ(shown_name(), "a.txt");
}

TEST_F(NavigationHistoryTest, ATabDroppedOnAnotherGroupTakesItsPlacesAlong) {
    create_window();
    const auto a = write("a.txt", numbered_lines(200));
    open(a);
    go_to_line(60);
    const workbench::GroupId first = window_->editor_layout().focused();
    window_->commands().execute("workbench.action.splitEditorRight");
    const workbench::GroupId second = window_->editor_layout().focused();
    window_->editor_layout().focus(first);
    const int index = static_cast<int>(*window_->editor_layout().group(first)->active);

    emit window_->editor_area()->tab_dropped_on_group(first, index, second);
    for (const auto& entry : window_->navigation_history().selections().entries()) {
        if (entry.uri == core::Uri::file(a)) {
            EXPECT_EQ(entry.group, second);
        }
    }
    EXPECT_EQ(window_->editor_layout().group(second)->tabs.back(), core::Uri::file(a));
}

TEST_F(NavigationHistoryTest, LastEditLocationGoesToTheNewestEditAndBackReturns) {
    create_window();
    open(write("a.txt", numbered_lines(200)));
    go_to_line(70);
    type("X");
    open(write("b.txt", numbered_lines(200)));
    go_to_line(30);
    ASSERT_EQ(shown_name(), "b.txt");

    run("workbench.action.navigateToLastEditLocation");
    EXPECT_EQ(shown_name(), "a.txt");
    EXPECT_EQ(caret_line(), 70U);
    run("workbench.action.navigateBack");
    EXPECT_EQ(shown_name(), "b.txt");
    EXPECT_EQ(caret_line(), 30U);
}

TEST_F(NavigationHistoryTest, OnlyEditsMakeAnEditLocation) {
    create_window();
    open(write("a.txt", numbered_lines(200)));
    go_to_line(70);
    EXPECT_FALSE(enabled("workbench.action.navigateToLastEditLocation"));
    type("X");
    EXPECT_TRUE(enabled("workbench.action.navigateToLastEditLocation"));
}

TEST_F(NavigationHistoryTest, TheGoMenuItemsRunTheirCommands) {
    create_window();
    open(write("a.txt", numbered_lines(200)));
    go_to_line(80);
    type("x");
    run("workbench.action.navigateBack");
    const std::vector<std::string> ids = {"workbench.action.navigateBack",
                                          "workbench.action.navigateForward",
                                          "workbench.action.navigateToLastEditLocation"};
    std::vector<std::string> executed;
    for (const std::string& id : ids) {
        window_->commands().add(id, "recorded",
                                [&executed, id](const json&) { executed.push_back(id); });
    }
    for (const std::string& id : ids) {
        QAction* item = menu_action(id);
        ASSERT_NE(item, nullptr) << id;
        ASSERT_TRUE(item->isEnabled()) << id;
        executed.clear();
        item->trigger();
        EXPECT_EQ(executed, std::vector<std::string>{id}) << id;
    }
}

TEST_F(NavigationHistoryTest, AFileThatCannotBeReadAnymoreIsForgottenAfterTheWarning) {
    create_window();
    const auto a = write("a.txt", numbered_lines(200));
    open(a);
    go_to_line(70);
    open(write("b.txt", numbered_lines(10)));
    ASSERT_TRUE(close_tab_of(core::Uri::file(a)));
    write("a.txt", "\xff\xfe\xfd");

    answer_next_prompt(QMessageBox::Ok);
    run("workbench.action.navigateBack");
    EXPECT_TRUE(prompt_shown_);
    EXPECT_EQ(shown_name(), "b.txt");
    for (const auto& entry : window_->navigation_history().selections().entries()) {
        EXPECT_NE(entry.uri, core::Uri::file(a));
    }
}

TEST_F(NavigationHistoryTest, APlaceBeyondTheEndOfAShortenedDocumentIsClampedAndAddsNoPlace) {
    create_window();
    open(write("a.txt", numbered_lines(200)));
    go_to_line(150);
    go_to_line(20);
    const core::Document& document = *window_->current_document();
    const std::vector<core::Edit> cut = {{offset_of_line(100), document.text().size(), ""}};
    ASSERT_TRUE(window_->current_document()->apply_edits(cut).has_value());
    const auto before = window_->navigation_history().selections().entries();

    run("workbench.action.navigateBack");
    EXPECT_EQ(window_->navigation_history().selections().entries(), before);
    EXPECT_GE(caret_line(), 99U);
    EXPECT_TRUE(enabled("workbench.action.navigateForward"));
}

TEST_F(NavigationHistoryTest, AMoveNearAPlaceReachedByWalkingReplacesItAndKeepsTheRest) {
    create_window();
    open(write("a.txt", numbered_lines(200)));
    go_to_line(80);
    go_to_line(150);
    const std::size_t count = window_->navigation_history().selections().entries().size();

    run("workbench.action.navigateBack");
    ASSERT_EQ(caret_line(), 80U);
    go_to_line(84);
    EXPECT_EQ(window_->navigation_history().selections().entries().size(), count);
    EXPECT_EQ(window_->navigation_history().selections().current()->selection.line(), 84U);
    EXPECT_TRUE(enabled("workbench.action.navigateForward"));
}

TEST_F(NavigationHistoryTest, ASavedUntitledDocumentKeepsItsPlacesInTheOrderTheyWereVisited) {
    create_window();
    window_->commands().execute("workbench.action.files.newUntitledFile");
    const core::Uri untitled = window_->current_uri();
    type("hello");
    open(write("a.txt", numbered_lines(10)));
    window_->editor_layout().show(untitled);
    ASSERT_EQ(window_->current_uri(), untitled);
    const core::Uri saved = core::Uri::file(scratch_ / "saved.txt");
    ASSERT_TRUE(window_->save_as(QString::fromStdString((scratch_ / "saved.txt").string())));

    const auto& entries = window_->navigation_history().selections().entries();
    EXPECT_EQ(std::ranges::count(entries, saved, &workbench::NavigationEntry::uri), 2);
    EXPECT_EQ(std::ranges::count(entries, untitled, &workbench::NavigationEntry::uri), 0);
}

TEST_F(NavigationHistoryTest, TheMenuShowsTheKeysOfTheCommands) {
    create_window();
    QAction* last = menu_action("workbench.action.navigateToLastEditLocation");
    ASSERT_NE(last, nullptr);
    EXPECT_TRUE(last->text().endsWith("\tCtrl+K Ctrl+Q")) << last->text().toStdString();
}

class ChangeLanguageModeTest : public LanguageTest {
protected:
    static constexpr const char* command = "workbench.action.editor.changeLanguageMode";

    // The labels of the items of the pick that the command opened.
    std::vector<QString> labels() const {
        std::vector<QString> result;
        for (const ui::QuickPickItem& item : window_->command_palette()->items()) {
            result.push_back(item.label);
        }
        return result;
    }

    // Types `text` in the open pick and chooses the item that matches best.
    void choose(const QString& text) {
        ui::QuickPick* pick = window_->command_palette();
        ASSERT_TRUE(pick->isVisible());
        QTest::keyClicks(pick->filter_field(), text);
        QTest::keyClick(pick->filter_field(), Qt::Key_Return);
        QApplication::processEvents();
    }
};

TEST_F(ChangeLanguageModeTest, ListsTheEntriesThenTheLanguagesByNameWithTheCurrentOneMarked) {
    create_window();
    ASSERT_TRUE(open(write("a.ts", "abc")));
    window_->commands().execute(command);
    ui::QuickPick* pick = window_->command_palette();
    ASSERT_TRUE(pick->isVisible());

    const auto& items = pick->items();
    ASSERT_GT(items.size(), 70U);
    EXPECT_EQ(items[0].label, "Auto Detect");
    EXPECT_EQ(items[1].label, "Configure File Association for '.ts'…");
    EXPECT_TRUE(items[2].separator);
    EXPECT_EQ(items[2].label, "languages (identifier)");
    for (std::size_t i = 0; i < 2; ++i) {
        EXPECT_FALSE(items[i].separator);
        EXPECT_TRUE(items[i].description.isEmpty());
    }
    for (std::size_t i = 3; i < items.size(); ++i) {
        const ui::QuickPickItem& item = items[i];
        EXPECT_FALSE(item.separator) << item.label.toStdString();
        EXPECT_TRUE(item.detail.isEmpty()) << item.label.toStdString();
        EXPECT_TRUE(item.description.startsWith("(") && item.description.contains(")"))
            << item.label.toStdString();
        if (i > 3) {
            EXPECT_LE(items[i - 1].label.compare(item.label, Qt::CaseInsensitive), 0)
                << item.label.toStdString();
        }
        EXPECT_EQ(item.description.endsWith("Configured Language"), item.label == "TypeScript")
            << item.label.toStdString();
    }
    const auto typescript =
        std::ranges::find(items, QString("TypeScript"), &ui::QuickPickItem::label);
    ASSERT_NE(typescript, items.end());
    EXPECT_EQ(typescript->description, "(typescript) - Configured Language");
    const auto plain = std::ranges::find(items, QString("Plain Text"), &ui::QuickPickItem::label);
    ASSERT_NE(plain, items.end());
    EXPECT_EQ(plain->description, "(plaintext)");
    EXPECT_EQ(pick->filter_field()->text(), "");
}

TEST_F(ChangeLanguageModeTest,
       AnEmptyDocumentHasNoAutoDetectAndADocumentWithoutAFileNoAssociation) {
    create_window();
    window_->commands().execute(command);
    const auto& items = window_->command_palette()->items();
    ASSERT_GT(items.size(), 70U);
    EXPECT_TRUE(items[0].separator) << "no Auto Detect for an empty document, no association";
    const auto plain = std::ranges::find(items, QString("Plain Text"), &ui::QuickPickItem::label);
    ASSERT_NE(plain, items.end());
    EXPECT_EQ(plain->description, "(plaintext) - Configured Language");
    const auto starts_with_configure = [&] {
        return std::ranges::any_of(
            labels(), [](const QString& label) { return label.startsWith("Configure"); });
    };
    EXPECT_FALSE(starts_with_configure());

    type("x");
    window_->command_palette()->cancel();
    window_->commands().execute(command);
    EXPECT_EQ(window_->command_palette()->items()[0].label, "Auto Detect");
    EXPECT_FALSE(starts_with_configure()) << "an untitled document has no file extension";
    ASSERT_TRUE(open(write("Makefile", "all:")));
    window_->command_palette()->cancel();
    window_->commands().execute(command);
    EXPECT_FALSE(starts_with_configure()) << "a file without an extension";
}

TEST_F(ChangeLanguageModeTest, AutoDetectGivesUpAPickedLanguageAndTheFileDecidesAgain) {
    create_window(true);
    if (!window_) {
        return;
    }
    ASSERT_TRUE(open(write("a.ts", "abc")));
    window_->commands().execute(command);
    choose("Python");
    ASSERT_TRUE(host_language_becomes("python"));

    window_->commands().execute(command);
    choose("Auto Detect");
    EXPECT_TRUE(host_language_becomes("typescript"));
    EXPECT_EQ(shown_language(), "TypeScript");

    // It follows the associations again.
    window_->change_setting("files.associations", json{{"*.ts", "markdown"}});
    EXPECT_TRUE(host_language_becomes("markdown"));
    EXPECT_EQ(shown_language(), "Markdown");
}

TEST_F(ChangeLanguageModeTest, APickedLanguageThatSurvivedSaveAsIsGivenUpByAutoDetectToo) {
    create_window(true);
    if (!window_) {
        return;
    }
    ASSERT_TRUE(open(write("a.txt", "abc")));
    window_->commands().execute(command);
    choose("Python");
    ASSERT_TRUE(host_language_becomes("python"));
    ASSERT_TRUE(window_->save_as(QString::fromStdString((scratch_ / "b.ts").string())));
    ASSERT_EQ(host_language(), "python");

    window_->commands().execute(command);
    choose("Auto Detect");
    EXPECT_TRUE(host_language_becomes("typescript"));
    window_->change_setting("files.associations", json{{"*.ts", "markdown"}});
    EXPECT_TRUE(host_language_becomes("markdown"));
}

TEST_F(ChangeLanguageModeTest, AutoDetectReadsTheFirstLineOfAFileWithoutAKnownName) {
    create_window(true);
    if (!window_) {
        return;
    }
    ASSERT_TRUE(open(write("script", "#!/usr/bin/env python3\nx")));
    ASSERT_EQ(host_language(), "python");
    window_->commands().execute(command);
    choose("Markdown");
    ASSERT_TRUE(host_language_becomes("markdown"));
    window_->commands().execute(command);
    choose("Auto Detect");
    EXPECT_TRUE(host_language_becomes("python"));
}

TEST_F(ChangeLanguageModeTest, ChoosingALanguageChangesItForExtensionsAndTheStatusBar) {
    create_window(true);
    if (!window_) {
        return;
    }
    ASSERT_TRUE(open(write("a.ts", "abc")));
    ASSERT_EQ(host_language(), "typescript");
    window_->commands().execute(command);
    choose("Python");
    EXPECT_TRUE(window_->command_palette()->isHidden());
    EXPECT_TRUE(host_language_becomes("python"));
    EXPECT_EQ(shown_language(), "Python");

    window_->commands().execute(command);
    const auto& items = window_->command_palette()->items();
    const auto python = std::ranges::find(items, QString("Python"), &ui::QuickPickItem::label);
    ASSERT_NE(python, items.end());
    EXPECT_EQ(python->description, "(python) - Configured Language");
}

TEST_F(ChangeLanguageModeTest, CancellingChangesNothing) {
    create_window(true);
    if (!window_) {
        return;
    }
    ASSERT_TRUE(open(write("a.ts", "abc")));
    window_->commands().execute(command);
    QTest::keyClick(window_->command_palette()->filter_field(), Qt::Key_Escape);
    EXPECT_TRUE(window_->command_palette()->isHidden());
    EXPECT_EQ(host_language(), "typescript");
    EXPECT_EQ(shown_language(), "TypeScript");
    // The next use of the palette is the command palette again.
    window_->commands().execute("workbench.action.showCommands");
    EXPECT_EQ(window_->command_palette()->filter_field()->text(), ">");
    EXPECT_NE(window_->command_palette()->items().front().label, "TypeScript");
}

TEST_F(ChangeLanguageModeTest,
       TheChoiceSurvivesSaveAsAndAChangeOfTheAssociationsAndEndsWithTheDocument) {
    create_window(true);
    if (!window_) {
        return;
    }
    ASSERT_TRUE(open(write("a.txt", "abc")));
    window_->commands().execute(command);
    choose("Python");
    ASSERT_TRUE(host_language_becomes("python"));

    ASSERT_TRUE(window_->save_as(QString::fromStdString((scratch_ / "b.ts").string())));
    EXPECT_EQ(host_language(), "python");
    EXPECT_EQ(shown_language(), "Python");

    window_->change_setting("files.associations", json{{"*.ts", "markdown"}});
    ASSERT_TRUE(open(write("c.ts", "abc")));
    EXPECT_TRUE(host_language_becomes("markdown"));
    ASSERT_TRUE(open(scratch_ / "b.ts"));
    EXPECT_EQ(host_language(), "python");
    EXPECT_EQ(shown_language(), "Python");

    ASSERT_TRUE(window_->close_document(1));
    ASSERT_TRUE(open(scratch_ / "b.ts"));
    EXPECT_EQ(host_language(), "markdown");
    EXPECT_EQ(shown_language(), "Markdown");
}

TEST_F(ChangeLanguageModeTest, TheChoiceIsKeptWhenTheExtensionHostRestarts) {
    create_window(true);
    if (!window_) {
        return;
    }
    ASSERT_TRUE(open(write("a.ts", "abc")));
    window_->commands().execute(command);
    choose("Python");
    ASSERT_TRUE(host_language_becomes("python"));
    std::filesystem::create_directories(scratch_ / "project");
    ASSERT_TRUE(window_->open_folder(scratch_ / "project"));
    ASSERT_TRUE(wait_until([&] {
        return window_->bridge().host() &&
               window_->bridge().host()->state() == exthost::HostState::ready;
    }));
    EXPECT_EQ(host_language(), "python");
    EXPECT_EQ(shown_language(), "Python");
}

TEST_F(ChangeLanguageModeTest, TheChoiceIsNotWrittenToTheSettings) {
    settings_file_ = write("settings.json", "{}");
    create_window();
    ASSERT_TRUE(open(write("a.ts", "abc")));
    window_->commands().execute(command);
    choose("Python");
    EXPECT_EQ(shown_language(), "Python");
    EXPECT_EQ(platform::read_file(settings_file_).value_or("?"), "{}");
}

TEST_F(ChangeLanguageModeTest,
       TheAssociationEntryWritesTheSettingAndUpdatesTheOtherDocumentsOfTheExtension) {
    settings_file_ = write("settings.json", R"j({"files.autoSave": "off"})j");
    create_window(true);
    if (!window_) {
        return;
    }
    ASSERT_TRUE(open(write("a.ts", "abc")));
    ASSERT_TRUE(open(write("b.ts", "abc")));
    ASSERT_TRUE(open(write("c.txt", "abc")));
    ASSERT_TRUE(open(scratch_ / "a.ts"));
    window_->commands().execute(command);
    const std::size_t listed = labels().size();
    choose("Configure");
    // The second list opens once the first has closed.
    ASSERT_TRUE(wait_until([&] { return window_->command_palette()->isVisible(); }));
    EXPECT_EQ(window_->command_palette()->items().size(), listed - 3)
        << "the languages, without the entries and the separator";
    EXPECT_EQ(window_->command_palette()->filter_field()->placeholderText(),
              "Select Language Mode to Associate with '.ts'");
    choose("Python");

    const std::string text = platform::read_file(settings_file_).value_or("");
    EXPECT_NE(text.find(R"j("*.ts":"python")j"), std::string::npos) << text;
    EXPECT_NE(text.find("files.autoSave"), std::string::npos) << text;
    EXPECT_TRUE(wait_until([&] { return host_language() == "python"; }));
    ASSERT_TRUE(open(scratch_ / "b.ts"));
    EXPECT_EQ(host_language(), "python");
    ASSERT_TRUE(open(scratch_ / "c.txt"));
    EXPECT_EQ(host_language(), "plaintext");
}

TEST_F(ChangeLanguageModeTest, TheAssociationEntryKeepsTheAssociationsThatAreThere) {
    settings_file_ = write("settings.json", R"j({"files.associations": {"*.cfg": "ini"}})j");
    create_window();
    ASSERT_TRUE(open(write("a.ts", "abc")));
    window_->commands().execute(command);
    choose("Configure");
    ASSERT_TRUE(wait_until([&] { return window_->command_palette()->isVisible(); }));
    choose("Markdown");
    const std::string text = platform::read_file(settings_file_).value_or("");
    EXPECT_NE(text.find(R"j("*.cfg":"ini")j"), std::string::npos) << text;
    EXPECT_NE(text.find(R"j("*.ts":"markdown")j"), std::string::npos) << text;
    EXPECT_EQ(shown_language(), "Markdown");
}

TEST_F(ChangeLanguageModeTest, TheLanguageItemOfTheStatusBarRunsTheCommand) {
    create_window();
    EXPECT_EQ(window_->status_bar().item("status.editor.language")->command, command);
}

TEST_F(ChangeLanguageModeTest, TheCommandIsInTheCommandPalette) {
    create_window();
    window_->commands().execute("workbench.action.showCommands");
    const auto& items = window_->command_palette()->items();
    EXPECT_NE(std::ranges::find(items, QString("Change Language Mode"), &ui::QuickPickItem::label),
              items.end());
}

class GoToTest : public MainWindowTest {
protected:
    static std::string numbered_lines(int count) {
        std::string text;
        for (int i = 0; i < count; ++i) {
            text += "line " + std::to_string(i) + "\n";
        }
        return text;
    }

    void open(const std::string& text) {
        ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", text).string())));
        window_->resize(900, 600);
        window_->show();
        QApplication::processEvents();
    }

    ui::QuickPick* pick() {
        return window_->go_to_line_pick();
    }
    QString label() {
        return pick()->list()->model()->index(0, 0).data().toString();
    }
    void type_in_pick(const char* text) {
        QTest::keyClicks(pick()->filter_field(), text);
    }
    core::Position caret() const {
        const core::Document& document = *window_->current_document();
        return document.text().position_at(document.selections().primary().active);
    }
    std::size_t history_size() const {
        return window_->navigation_history().selections().entries().size();
    }
};

TEST_F(GoToTest, TheGoMenuItemsAreEnabledAndShowTheirKeys) {
    create_window();
    QAction* line = menu_action("workbench.action.gotoLine");
    ASSERT_NE(line, nullptr);
    EXPECT_TRUE(line->isEnabled());
    EXPECT_TRUE(line->text().endsWith("\tCtrl+G")) << line->text().toStdString();
    QAction* bracket = menu_action("editor.action.jumpToBracket");
    ASSERT_NE(bracket, nullptr);
    EXPECT_TRUE(bracket->isEnabled());
    EXPECT_TRUE(bracket->text().endsWith("\tCtrl+Shift+\\")) << bracket->text().toStdString();
}

TEST_F(GoToTest, TheCommandOpensTheQuickPickWithAPrompt) {
    create_window();
    open(numbered_lines(40));
    EXPECT_TRUE(pick()->isHidden());
    menu_action("workbench.action.gotoLine")->trigger();
    ASSERT_TRUE(pick()->isVisible());
    EXPECT_EQ(pick()->filter_field()->text(), ":");
    EXPECT_EQ(pick()->filter_field()->placeholderText(), "Type a line number, then : and a column");
    EXPECT_EQ(label(), "Type a line number to go to (from 1 to 41).");
}

TEST_F(GoToTest, TheCaretFollowsTheTextAsItIsTypedAndTheLabelSaysWhatEnterDoes) {
    create_window();
    open(numbered_lines(40));
    window_->commands().execute("workbench.action.gotoLine");

    type_in_pick("12");
    EXPECT_EQ(caret(), (core::Position{11, 0}));
    EXPECT_EQ(label(), "Press 'Enter' to go to line 12 or enter colon : to add a column number.");
    type_in_pick(":");
    EXPECT_EQ(caret(), (core::Position{11, 0}));
    EXPECT_EQ(label(), "Press 'Enter' to go to line 12 or enter a column number (from 1 to 8).");
    type_in_pick("4");
    EXPECT_EQ(caret(), (core::Position{11, 3}));
    EXPECT_EQ(label(), "Press 'Enter' to go to line 12 at column 4.");

    QTest::keyClick(pick()->filter_field(), Qt::Key_Backspace);
    QTest::keyClick(pick()->filter_field(), Qt::Key_Backspace);
    EXPECT_EQ(caret(), (core::Position{11, 0}));
    EXPECT_EQ(label(), "Press 'Enter' to go to line 12 or enter colon : to add a column number.");
}

TEST_F(GoToTest, TheLineAndTheColumnAreKeptWithinTheDocumentAndNegativeNumbersCountFromTheEnd) {
    create_window();
    open(numbered_lines(40));
    window_->commands().execute("workbench.action.gotoLine");
    type_in_pick("999:999");
    EXPECT_EQ(caret(), (core::Position{40, 0}));
    for (int i = 0; i < 7; ++i) {
        QTest::keyClick(pick()->filter_field(), Qt::Key_Backspace);
    }
    type_in_pick("-2");
    EXPECT_EQ(caret(), (core::Position{39, 0}));
    type_in_pick(":-1");
    EXPECT_EQ(caret(), (core::Position{39, 6}));
}

TEST_F(GoToTest, EnterKeepsTheCaretAndClosesAndTheNewPlaceIsInTheHistory) {
    create_window();
    open(numbered_lines(40));
    const std::size_t before = history_size();
    window_->commands().execute("workbench.action.gotoLine");
    type_in_pick("5:3");
    EXPECT_EQ(history_size(), before);

    QTest::keyClick(pick()->filter_field(), Qt::Key_Return);
    EXPECT_TRUE(pick()->isHidden());
    EXPECT_EQ(caret(), (core::Position{4, 2}));
    EXPECT_EQ(history_size(), before + 1);
    EXPECT_TRUE(window_->view()->hasFocus());

    menu_action("workbench.action.navigateBack")->trigger();
    EXPECT_EQ(caret(), (core::Position{0, 0}));
}

TEST_F(GoToTest, EscapePutsTheCaretAndTheScrollBack) {
    create_window();
    open(numbered_lines(400));
    window_->view()->select_range(
        core::Selection::caret(window_->current_document()->text().offset_at({30, 2})));
    window_->view()->verticalScrollBar()->setValue(window_->view()->line_height() * 20);
    const int scroll = window_->view()->verticalScrollBar()->value();
    const std::size_t before = history_size();

    window_->commands().execute("workbench.action.gotoLine");
    type_in_pick("350");
    EXPECT_EQ(caret().line, 349U);
    EXPECT_NE(window_->view()->verticalScrollBar()->value(), scroll);

    QTest::keyClick(pick()->filter_field(), Qt::Key_Escape);
    EXPECT_TRUE(pick()->isHidden());
    EXPECT_EQ(caret(), (core::Position{30, 2}));
    EXPECT_EQ(window_->view()->verticalScrollBar()->value(), scroll);
    EXPECT_EQ(history_size(), before);
}

TEST_F(GoToTest, EscapeBringsBackEveryCursor) {
    create_window();
    open(numbered_lines(40));
    const auto& text = window_->current_document()->text();
    window_->view()->select_ranges({core::Selection{text.offset_at({2, 1}), text.offset_at({2, 4})},
                                    core::Selection::caret(text.offset_at({9, 0}))},
                                   1);
    const auto before = window_->current_document()->selections();

    window_->commands().execute("workbench.action.gotoLine");
    type_in_pick("30");
    EXPECT_EQ(window_->current_document()->selections().all().size(), 1U);
    QTest::keyClick(pick()->filter_field(), Qt::Key_Escape);
    EXPECT_EQ(window_->current_document()->selections().all(), before.all());
    EXPECT_EQ(window_->current_document()->selections().primary_index(), before.primary_index());
}

TEST_F(GoToTest, EnterWithoutALineDoesNothingAndTheQuickPickStaysOpen) {
    create_window();
    open(numbered_lines(40));
    window_->view()->select_range(
        core::Selection::caret(window_->current_document()->text().offset_at({7, 0})));
    window_->commands().execute("workbench.action.gotoLine");
    QTest::keyClick(pick()->filter_field(), Qt::Key_Return);
    EXPECT_TRUE(pick()->isVisible());
    type_in_pick("x");
    QTest::keyClick(pick()->filter_field(), Qt::Key_Return);
    EXPECT_TRUE(pick()->isVisible());
    EXPECT_EQ(caret(), (core::Position{7, 0}));
}

TEST_F(GoToTest, ClearingTheNumberBringsTheCaretBackWhileTheQuickPickIsOpen) {
    create_window();
    open(numbered_lines(40));
    window_->view()->select_range(
        core::Selection::caret(window_->current_document()->text().offset_at({7, 0})));
    window_->commands().execute("workbench.action.gotoLine");
    type_in_pick("20");
    ASSERT_EQ(caret().line, 19U);
    QTest::keyClick(pick()->filter_field(), Qt::Key_Backspace);
    QTest::keyClick(pick()->filter_field(), Qt::Key_Backspace);
    EXPECT_EQ(caret(), (core::Position{7, 0}));
}

TEST_F(GoToTest, ThePreviewMakesNoPlaceOfItsOwnWhenItWasCancelled) {
    create_window();
    open(numbered_lines(400));
    window_->commands().execute("workbench.action.gotoLine");
    type_in_pick("300");
    QTest::keyClick(pick()->filter_field(), Qt::Key_Escape);
    for (const auto& entry : window_->navigation_history().selections().entries()) {
        EXPECT_LT(entry.selection.line(), 299U);
    }
}

TEST_F(GoToTest, AColumnCountsTabsAsFarAsTheNextTabStop) {
    create_window();
    open("\tindented\nplain\n");
    window_->commands().execute("workbench.action.gotoLine");
    type_in_pick("1:5");
    EXPECT_EQ(caret(), (core::Position{0, 1}));
    EXPECT_EQ(label(), "Press 'Enter' to go to line 1 at column 5.");
}

TEST_F(GoToTest, TheShortcutOpensTheQuickPickFromTheEditor) {
    create_window();
    open(numbered_lines(40));
    window_->activateWindow();
    ASSERT_TRUE(QTest::qWaitForWindowActive(window_.get()));
    window_->view()->setFocus();
    QTest::keyClick(window_->view(), Qt::Key_G, Qt::ControlModifier);
    EXPECT_TRUE(pick()->isVisible());
}

TEST_F(GoToTest, GoToBracketMovesTheCaretToThePartner) {
    create_window();
    open("f(a, [b])\n");
    window_->view()->select_range(core::Selection::caret(1));
    menu_action("editor.action.jumpToBracket")->trigger();
    EXPECT_EQ(caret(), (core::Position{0, 8}));
    window_->commands().execute("editor.action.jumpToBracket");
    EXPECT_EQ(caret(), (core::Position{0, 1}));
}

TEST_F(GoToTest, GoToBracketOnEveryCursorAndTheShortcut) {
    create_window();
    open("(a) [b]\n");
    window_->activateWindow();
    ASSERT_TRUE(QTest::qWaitForWindowActive(window_.get()));
    window_->view()->setFocus();
    window_->view()->select_ranges({core::Selection::caret(0), core::Selection::caret(4)}, 0);
    QTest::keyClick(window_->view(), Qt::Key_Backslash, Qt::ControlModifier | Qt::ShiftModifier);
    const auto carets = window_->current_document()->selections().all();
    ASSERT_EQ(carets.size(), 2U);
    EXPECT_EQ(carets[0], core::Selection::caret(2));
    EXPECT_EQ(carets[1], core::Selection::caret(6));
}

class UserFilesTest : public MainWindowTest {
protected:
    void SetUp() override {
        MainWindowTest::SetUp();
        settings_file_ = scratch_ / "config" / "settings.json";
        keybindings_file_ = scratch_ / "config" / "keybindings.json";
    }

    std::string shown_text() const {
        return current_text();
    }
    std::string shown_name() const {
        const auto path = window_->current_uri().file_path();
        return path ? path->filename().string() : "<not a file>";
    }
    std::string on_disk(const std::filesystem::path& file) const {
        return platform::read_file(file).value_or("<missing>");
    }
    std::size_t tabs_of(const std::filesystem::path& file) {
        return window_->editor_layout().locations(core::Uri::file(file)).size();
    }
};

TEST_F(UserFilesTest, TheItemsOfThePreferencesMenuAreEnabledAndShowTheirKeys) {
    create_window();
    QAction* settings = menu_action("workbench.action.openSettings");
    ASSERT_NE(settings, nullptr);
    EXPECT_TRUE(settings->isEnabled());
    EXPECT_TRUE(settings->text().endsWith("\tCtrl+,")) << settings->text().toStdString();
    QAction* shortcuts = menu_action("workbench.action.openGlobalKeybindings");
    ASSERT_NE(shortcuts, nullptr);
    EXPECT_TRUE(shortcuts->isEnabled());
    EXPECT_TRUE(shortcuts->text().endsWith("\tCtrl+K Ctrl+S")) << shortcuts->text().toStdString();
}

TEST_F(UserFilesTest, SettingsOpensTheSettingsFileWithItsText) {
    std::filesystem::create_directories(settings_file_.parent_path());
    write("config/settings.json", R"j({"editor.tabSize": 2})j");
    create_window();
    menu_action("workbench.action.openSettings")->trigger();
    EXPECT_EQ(shown_name(), "settings.json");
    EXPECT_EQ(shown_text(), R"j({"editor.tabSize": 2})j");
}

TEST_F(UserFilesTest, AMissingSettingsFileIsCreatedWithAnEmptyObjectEvenInAMissingDirectory) {
    create_window();
    ASSERT_FALSE(std::filesystem::exists(settings_file_.parent_path()));
    menu_action("workbench.action.openSettings")->trigger();
    EXPECT_EQ(on_disk(settings_file_), "{}\n");
    EXPECT_EQ(shown_name(), "settings.json");
    EXPECT_EQ(shown_text(), "{}\n");
    EXPECT_FALSE(window_->current_document()->is_dirty());
}

TEST_F(UserFilesTest, KeyboardShortcutsOpensTheKeybindingsFileAndCreatesItWithAnEmptyArray) {
    create_window();
    menu_action("workbench.action.openGlobalKeybindings")->trigger();
    EXPECT_EQ(on_disk(keybindings_file_), "[]\n");
    EXPECT_EQ(shown_name(), "keybindings.json");
    EXPECT_EQ(shown_text(), "[]\n");
}

TEST_F(UserFilesTest, AnExistingFileIsNotOverwritten) {
    std::filesystem::create_directories(keybindings_file_.parent_path());
    write("config/keybindings.json", "[ // mine\n]");
    create_window();
    window_->commands().execute("workbench.action.openGlobalKeybindings");
    EXPECT_EQ(on_disk(keybindings_file_), "[ // mine\n]");
    EXPECT_EQ(shown_text(), "[ // mine\n]");
}

TEST_F(UserFilesTest, OpeningTheFileAgainShowsTheTabItHas) {
    create_window();
    window_->commands().execute("workbench.action.openSettings");
    type("x");
    window_->commands().execute("workbench.action.files.newUntitledFile");
    window_->commands().execute("workbench.action.openSettings");
    EXPECT_EQ(tabs_of(settings_file_), 1U);
    EXPECT_EQ(shown_name(), "settings.json");
    EXPECT_EQ(shown_text().front(), 'x');
}

TEST_F(UserFilesTest, TheJsonCommandsOpenTheSameFilesAndAreInThePalette) {
    create_window();
    window_->commands().execute("workbench.action.openSettingsJson");
    EXPECT_EQ(shown_name(), "settings.json");
    window_->commands().execute("workbench.action.openGlobalKeybindingsFile");
    EXPECT_EQ(shown_name(), "keybindings.json");

    window_->commands().execute("workbench.action.showCommands");
    const auto& items = window_->command_palette()->items();
    for (const char* title : {"Open Settings", "Open User Settings (JSON)",
                              "Open Keyboard Shortcuts", "Open Keyboard Shortcuts (JSON)"}) {
        EXPECT_NE(std::ranges::find(items, QString(title), &ui::QuickPickItem::label), items.end())
            << title;
    }
}

TEST_F(UserFilesTest, WithoutAConfigurationFileTheWindowSaysSoAndOpensNothing) {
    settings_file_.clear();
    keybindings_file_.clear();
    create_window();
    const std::size_t tabs =
        window_->editor_layout().group(window_->editor_layout().focused())->tabs.size();
    window_->commands().execute("workbench.action.openSettings");
    window_->commands().execute("workbench.action.openGlobalKeybindings");
    EXPECT_EQ(window_->editor_layout().group(window_->editor_layout().focused())->tabs.size(),
              tabs);
    EXPECT_EQ(window_->status_bar().message(), "This window has no configuration directory");
}

TEST_F(UserFilesTest, AFileThatCannotBeCreatedIsReportedInTheStatusBar) {
    write("config", "this is a file, not a directory");
    create_window();
    window_->commands().execute("workbench.action.openSettings");
    EXPECT_TRUE(window_->status_bar().message().contains("cannot be created"));
    EXPECT_FALSE(std::filesystem::exists(settings_file_));
}

TEST_F(UserFilesTest, SavingTheSettingsFileChangesTheRunningWindow) {
    create_window();
    ASSERT_NE(window_->settings().editor().tab_size, 8);
    window_->commands().execute("workbench.action.openSettings");
    ASSERT_EQ(shown_name(), "settings.json");
    const core::Document& document = *window_->current_document();
    const std::vector<core::Edit> edits = {
        {0, document.text().size(), R"j({"editor.tabSize": 8})j"}};
    ASSERT_TRUE(window_->current_document()->apply_edits(edits).has_value());
    window_->commands().execute("workbench.action.files.save");
    EXPECT_TRUE(QTest::qWaitFor([&] { return window_->settings().editor().tab_size == 8; }, 5000));
}

TEST_F(UserFilesTest, SavingTheKeybindingsFileChangesTheMenusOfTheRunningWindow) {
    create_window();
    window_->commands().execute("workbench.action.openGlobalKeybindings");
    ASSERT_EQ(shown_name(), "keybindings.json");
    const core::Document& document = *window_->current_document();
    const std::vector<core::Edit> edits = {
        {0, document.text().size(),
         R"j([{"key": "ctrl+shift+9", "command": "workbench.action.togglePanel"}])j"}};
    ASSERT_TRUE(window_->current_document()->apply_edits(edits).has_value());
    window_->commands().execute("workbench.action.files.save");
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            return menu_action("workbench.action.togglePanel")->text().endsWith("\tCtrl+Shift+9");
        },
        5000));
}

class ReopenTest : public MainWindowTest {
protected:
    core::Uri open(const std::string& name, const std::string& text = "text") {
        const auto path = write(name, text);
        EXPECT_TRUE(window_->open_file(QString::fromStdString(path.string())));
        return core::Uri::file(path);
    }
    void close(const core::Uri& uri) {
        const auto& tabs = window_->editor_layout().group(window_->editor_layout().focused())->tabs;
        const auto at = std::ranges::find(tabs, uri);
        ASSERT_NE(at, tabs.end());
        ASSERT_TRUE(window_->close_document(static_cast<std::size_t>(at - tabs.begin())));
    }
    void reopen() {
        QAction* item = menu_action("workbench.action.reopenClosedEditor");
        ASSERT_NE(item, nullptr);
        ASSERT_TRUE(item->isEnabled());
        item->trigger();
    }
    bool can_reopen() {
        QAction* item = menu_action("workbench.action.reopenClosedEditor");
        return item && item->isEnabled();
    }
    std::vector<core::Uri> tabs(workbench::GroupId group) const {
        return window_->editor_layout().group(group)->tabs;
    }
    bool has(workbench::GroupId group, const core::Uri& uri) const {
        const auto all = tabs(group);
        return std::ranges::find(all, uri) != all.end();
    }
    workbench::GroupId focused() const {
        return window_->editor_layout().focused();
    }
    const core::Uri& shown() const {
        return window_->current_uri();
    }
    core::Position caret() const {
        const core::Document& document = *window_->current_document();
        return document.text().position_at(document.selections().primary().active);
    }
};

TEST_F(ReopenTest, TheItemIsDisabledUntilAnEditorIsClosedAndShowsItsKey) {
    create_window();
    EXPECT_FALSE(can_reopen());
    const core::Uri a = open("a.txt");
    EXPECT_FALSE(can_reopen());
    close(a);
    EXPECT_TRUE(can_reopen());
    EXPECT_TRUE(
        menu_action("workbench.action.reopenClosedEditor")->text().endsWith("\tCtrl+Shift+T"))
        << menu_action("workbench.action.reopenClosedEditor")->text().toStdString();
}

TEST_F(ReopenTest, TheClosedEditorComesBackAtItsPlaceWithItsSelection) {
    create_window();
    const core::Uri a = open("a.txt", "one\ntwo\nthree\n");
    const core::Uri b = open("b.txt");
    const core::Uri c = open("c.txt");
    window_->editor_layout().activate(focused(), 1);
    window_->view()->select_range(core::Selection{5, 9});
    close(b);
    ASSERT_EQ(std::ranges::count(tabs(focused()), b), 0);
    window_->editor_layout().activate(focused(), 0);

    reopen();
    const std::vector<core::Uri> now = tabs(focused());
    const auto at = std::ranges::find(now, b);
    ASSERT_NE(at, now.end());
    EXPECT_EQ(at - now.begin(), 2);
    EXPECT_EQ(shown(), b);
    EXPECT_FALSE(can_reopen());
    static_cast<void>(a);
    static_cast<void>(c);
}

TEST_F(ReopenTest, TheSelectionsOfTheClosedDocumentAreRestored) {
    create_window();
    const core::Uri a = open("a.txt", "one\ntwo\nthree\n");
    open("b.txt");
    window_->editor_layout().show(a);
    window_->view()->select_ranges({core::Selection{1, 2}, core::Selection{5, 9}}, 1);
    close(a);
    reopen();
    ASSERT_EQ(shown(), a);
    const core::Document& document = *window_->current_document();
    ASSERT_EQ(document.selections().all().size(), 2U);
    EXPECT_EQ(document.selections().all()[0], (core::Selection{1, 2}));
    EXPECT_EQ(document.selections().all()[1], (core::Selection{5, 9}));
    EXPECT_EQ(document.selections().primary_index(), 1U);
}

TEST_F(ReopenTest, RepeatingGoesFurtherBackInTheOrderTheEditorsWereClosed) {
    create_window();
    const core::Uri a = open("a.txt");
    const core::Uri b = open("b.txt");
    const core::Uri c = open("c.txt");
    close(a);
    close(c);
    close(b);

    reopen();
    EXPECT_EQ(shown(), b);
    reopen();
    EXPECT_EQ(shown(), c);
    reopen();
    EXPECT_EQ(shown(), a);
    EXPECT_FALSE(can_reopen());
}

TEST_F(ReopenTest, EditorsThatOneOperationClosedComeBackTogether) {
    create_window();
    const core::Uri a = open("a.txt");
    const core::Uri b = open("b.txt");
    const core::Uri c = open("c.txt");
    const core::Uri d = open("d.txt");
    close(d);
    window_->editor_layout().show(b);
    emit window_->editor_area()->close_others_requested(
        focused(), static_cast<int>(*window_->editor_layout().group(focused())->active));
    ASSERT_EQ(std::ranges::count(tabs(focused()), a), 0);

    reopen();
    const auto now = tabs(focused());
    EXPECT_NE(std::ranges::find(now, a), now.end());
    EXPECT_NE(std::ranges::find(now, c), now.end());
    EXPECT_EQ(std::ranges::count(now, d), 0);
    EXPECT_TRUE(can_reopen());
    reopen();
    EXPECT_TRUE(has(focused(), d));
}

TEST_F(ReopenTest, TheEditorGoesBackToItsGroupWhenTheGroupIsStillThere) {
    create_window();
    const core::Uri a = open("a.txt");
    const workbench::GroupId first = focused();
    window_->commands().execute("workbench.action.splitEditorRight");
    const workbench::GroupId second = focused();
    const core::Uri b = open("b.txt");
    open("c.txt");
    close(b);
    window_->editor_layout().focus(first);

    reopen();
    EXPECT_EQ(focused(), second);
    EXPECT_TRUE(has(second, b));
    EXPECT_EQ(std::ranges::count(tabs(first), b), 0);
    static_cast<void>(a);
}

TEST_F(ReopenTest, TheEditorsOfAClosedGroupComeBackInTheFocusedGroup) {
    create_window();
    open("a.txt");
    window_->commands().execute("workbench.action.splitEditorRight");
    const core::Uri b = open("b.txt");
    const workbench::GroupId second = focused();
    window_->commands().execute("workbench.action.closeGroup");
    ASSERT_EQ(window_->editor_layout().group_ids().size(), 1U);
    ASSERT_FALSE(window_->editor_layout().group(second));

    reopen();
    EXPECT_TRUE(has(focused(), b));
    EXPECT_EQ(shown(), b);
}

TEST_F(ReopenTest, AFileThatIsGoneIsSkippedWithAMessageAndTheOneBeforeItComesBack) {
    create_window();
    const core::Uri a = open("a.txt");
    const core::Uri b = open("b.txt");
    const core::Uri c = open("c.txt");
    close(a);
    close(b);
    std::filesystem::remove(*b.file_path());
    close(c);
    std::filesystem::remove(*c.file_path());

    reopen();
    EXPECT_EQ(shown(), a);
    EXPECT_EQ(window_->status_bar().message(), "b.txt no longer exists");
    EXPECT_FALSE(can_reopen());
}

TEST_F(ReopenTest, ADocumentThatIsOpenInItsGroupAgainIsSkippedForTheOneBeforeIt) {
    create_window();
    const core::Uri x = open("x.txt");
    const core::Uri a = open("a.txt");
    open("b.txt");
    close(x);
    close(a);
    open("a.txt");
    ASSERT_EQ(std::ranges::count(tabs(focused()), a), 1);

    reopen();
    EXPECT_EQ(std::ranges::count(tabs(focused()), a), 1);
    EXPECT_TRUE(has(focused(), x));
    EXPECT_EQ(shown(), x);
    EXPECT_FALSE(can_reopen());
}

TEST_F(ReopenTest, ChangesThatWereDiscardedWhenTheFileWasClosedAreNotBack) {
    create_window();
    const core::Uri a = open("a.txt", "abc");
    open("b.txt");
    window_->editor_layout().show(a);
    type("X");
    close(a);
    reopen();
    ASSERT_EQ(shown(), a);
    EXPECT_EQ(window_->current_document()->text().to_string(), "abc");
    EXPECT_FALSE(window_->current_document()->is_dirty());
}

TEST_F(ReopenTest, AnUntitledDocumentWithTextComesBackAsAnUnsavedOneWithTheSameText) {
    create_window();
    window_->commands().execute("workbench.action.files.newUntitledFile");
    const core::Uri untitled = shown();
    type("note");
    close(untitled);

    reopen();
    EXPECT_FALSE(shown().file_path());
    EXPECT_EQ(window_->current_document()->text().to_string(), "note");
    EXPECT_TRUE(window_->current_document()->is_dirty());
}

TEST_F(ReopenTest, AnEmptyUntitledDocumentIsNotRemembered) {
    create_window();
    window_->commands().execute("workbench.action.files.newUntitledFile");
    close(shown());
    EXPECT_FALSE(can_reopen());
}

TEST_F(ReopenTest, AtMostTwentyEditorsAreRemembered) {
    create_window();
    std::vector<core::Uri> files;
    for (int i = 0; i < 25; ++i) {
        files.push_back(open("f" + std::to_string(i) + ".txt"));
    }
    for (const core::Uri& file : files) {
        close(file);
    }
    int reopened = 0;
    while (can_reopen()) {
        reopen();
        ++reopened;
        ASSERT_LE(reopened, 30);
    }
    EXPECT_EQ(reopened, 20);
}

TEST_F(ReopenTest, TheShortcutReopensTheEditor) {
    create_window();
    window_->activateWindow();
    ASSERT_TRUE(QTest::qWaitForWindowActive(window_.get()));
    const core::Uri a = open("a.txt");
    open("b.txt");
    close(a);
    window_->view()->setFocus();
    QTest::keyClick(window_->view(), Qt::Key_T, Qt::ControlModifier | Qt::ShiftModifier);
    EXPECT_TRUE(has(focused(), a));
}

class ThemePickerTest : public MainWindowTest {
protected:
    void start() {
        settings_file_ = write("settings.json", "{}");
        create_window();
        window_->resize(900, 600);
        window_->show();
        QApplication::processEvents();
    }

    ui::QuickPick* pick() {
        return window_->command_palette();
    }
    QString label_of(std::size_t item) const {
        return window_->command_palette()->items()[item].label;
    }
    // The position in the list of the theme, or of nothing.
    std::optional<std::size_t> item_of(const QString& label) const {
        const auto& items = window_->command_palette()->items();
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (!items[i].separator && items[i].label == label) {
                return i;
            }
        }
        return std::nullopt;
    }
    QString chosen_label() const {
        const QModelIndex current = window_->command_palette()->list()->currentIndex();
        return current.data().toString();
    }
    void press(Qt::Key key) {
        QTest::keyClick(pick()->filter_field(), key);
    }
    std::string applied() const {
        return window_->themes().current();
    }
    QColor editor_background() const {
        return window_->colors().color("editor.background");
    }
    std::string settings_text() const {
        return platform::read_file(settings_file_).value_or("");
    }
};

TEST_F(ThemePickerTest, TheItemIsEnabledAndShowsItsKey) {
    start();
    QAction* item = menu_action("workbench.action.selectTheme");
    ASSERT_NE(item, nullptr);
    EXPECT_TRUE(item->isEnabled());
    EXPECT_TRUE(item->text().endsWith("\tCtrl+K Ctrl+T")) << item->text().toStdString();
}

TEST_F(ThemePickerTest, TheQuickPickListsTheThemesInGroupsAndChoosesTheCurrentOne) {
    start();
    ASSERT_EQ(applied(), "Dark 2026");
    window_->commands().execute("workbench.action.selectTheme");
    ASSERT_TRUE(pick()->isVisible());
    EXPECT_EQ(pick()->filter_field()->placeholderText(),
              "Select Color Theme (Up/Down Keys to Preview)");

    std::vector<QString> headings;
    for (const ui::QuickPickItem& item : pick()->items()) {
        if (item.separator) {
            headings.push_back(item.label);
        }
    }
    EXPECT_EQ(headings,
              (std::vector<QString>{"dark themes", "light themes", "high contrast themes"}));
    EXPECT_EQ(pick()->shown_items().size(), window_->themes().themes().size());
    EXPECT_EQ(chosen_label(), "Dark 2026");
    EXPECT_EQ(label_of(1), "Dark 2026");
}

TEST_F(ThemePickerTest, TheThemeOfTheSettingsIsTheChosenOne) {
    settings_file_ = write("settings.json", R"j({"workbench.colorTheme": "Light Modern"})j");
    create_window();
    window_->commands().execute("workbench.action.selectTheme");
    EXPECT_EQ(chosen_label(), "Light Modern");
}

TEST_F(ThemePickerTest, MovingTheChosenRowPreviewsTheThemeWithoutChangingTheSettings) {
    start();
    const auto id_of_label = [&](const QString& label) {
        for (const ui::ThemeStore::Entry& entry : window_->themes().themes()) {
            if (QString::fromStdString(entry.label) == label) {
                return entry.id;
            }
        }
        return std::string();
    };
    window_->commands().execute("workbench.action.selectTheme");
    const QColor before = editor_background();
    for (int i = 0; i < 6; ++i) {
        press(Qt::Key_Down);
        EXPECT_EQ(applied(), id_of_label(chosen_label())) << i;
    }
    EXPECT_NE(applied(), "Dark 2026");
    EXPECT_NE(editor_background(), before);
    EXPECT_EQ(settings_text(), "{}");
    EXPECT_EQ(window_->settings().editor().color_theme, "Dark 2026");
    press(Qt::Key_Escape);
}

TEST_F(ThemePickerTest, EnterKeepsTheThemeAndWritesItToTheSettings) {
    start();
    window_->commands().execute("workbench.action.selectTheme");
    QTest::keyClicks(pick()->filter_field(), "Light Modern");
    ASSERT_EQ(applied(), "Light Modern");
    press(Qt::Key_Return);
    EXPECT_TRUE(pick()->isHidden());
    EXPECT_EQ(applied(), "Light Modern");
    EXPECT_NE(settings_text().find("\"workbench.colorTheme\": \"Light Modern\""), std::string::npos)
        << settings_text();
    EXPECT_EQ(window_->settings().editor().color_theme, "Light Modern");
}

TEST_F(ThemePickerTest, EscapeBringsTheThemeAndTheColorsBack) {
    start();
    const QColor before = editor_background();
    window_->commands().execute("workbench.action.selectTheme");
    QTest::keyClicks(pick()->filter_field(), "Light Modern");
    ASSERT_EQ(applied(), "Light Modern");
    ASSERT_NE(editor_background(), before);

    press(Qt::Key_Escape);
    EXPECT_TRUE(pick()->isHidden());
    EXPECT_EQ(applied(), "Dark 2026");
    EXPECT_EQ(editor_background(), before);
    EXPECT_EQ(settings_text(), "{}");
}

TEST_F(ThemePickerTest, TheFocusLeavingTheQuickPickPutsTheThemeBackToo) {
    start();
    window_->commands().execute("workbench.action.selectTheme");
    QTest::keyClicks(pick()->filter_field(), "High Contrast");
    ASSERT_NE(applied(), "Dark 2026");
    pick()->cancel();
    EXPECT_EQ(applied(), "Dark 2026");
}

TEST_F(ThemePickerTest, AThemeOfAnExtensionIsInTheListAndAThemeWithoutAFileCannotBeChosen) {
    start();
    window_->themes().add_themes({{.id = "ext.theme",
                                   .label = "Extension Theme",
                                   .kind = core::ThemeKind::light,
                                   .file = scratch_ / "missing-theme.json"}});
    window_->status_bar().clear_message();
    window_->commands().execute("workbench.action.selectTheme");
    const auto item = item_of("Extension Theme");
    ASSERT_TRUE(item.has_value());
    EXPECT_TRUE(pick()->items()[*item].disabled);

    QTest::keyClicks(pick()->filter_field(), "Extension");
    EXPECT_EQ(applied(), "Dark 2026");
    EXPECT_TRUE(window_->status_bar().message().isEmpty())
        << window_->status_bar().message().toStdString();
    press(Qt::Key_Return);
    EXPECT_TRUE(pick()->isVisible());
    EXPECT_EQ(settings_text(), "{}");
}

TEST_F(ThemePickerTest, WithNoThemesTheStatusBarSaysSoAndNothingOpens) {
    themes_directory_ = scratch_ / "no-themes";
    start();
    window_->status_bar().clear_message();
    window_->commands().execute("workbench.action.selectTheme");
    EXPECT_FALSE(pick()->isVisible());
    EXPECT_EQ(window_->status_bar().message(), "There are no color themes to choose from");
}

TEST_F(ThemePickerTest, TheCommandIsInThePaletteAndTheShortcutOpensTheQuickPick) {
    start();
    window_->activateWindow();
    ASSERT_TRUE(QTest::qWaitForWindowActive(window_.get()));
    window_->view()->setFocus();
    QTest::keyClick(window_->view(), Qt::Key_K, Qt::ControlModifier);
    QTest::keyClick(window_->view(), Qt::Key_T, Qt::ControlModifier);
    EXPECT_TRUE(pick()->isVisible());
    EXPECT_EQ(label_of(1), "Dark 2026");
}

class NewWindowTest : public MainWindowTest {
protected:
    // Replaces the start of a process by a recorder of what would be started.
    void record_starts() {
        start_process_ =
            [this](const platform::ProcessSpec& spec) -> std::expected<std::int64_t, Error> {
            started_.push_back(spec);
            return 4242;
        };
    }

    std::vector<platform::ProcessSpec> started_;
};

TEST_F(NewWindowTest, TheItemIsEnabledAndShowsItsKey) {
    create_window();
    QAction* item = menu_action("workbench.action.newWindow");
    ASSERT_NE(item, nullptr);
    EXPECT_TRUE(item->isEnabled());
    EXPECT_TRUE(item->text().endsWith("\tCtrl+Shift+N")) << item->text().toStdString();
}

TEST_F(NewWindowTest, TheCommandStartsTheProgramWithNoArgumentsInADirectoryThatExists) {
    record_starts();
    executable_ = scratch_ / "stand-in";
    create_window();
    menu_action("workbench.action.newWindow")->trigger();

    ASSERT_EQ(started_.size(), 1U);
    const platform::ProcessSpec& spec = started_[0];
    EXPECT_EQ(spec.executable, scratch_ / "stand-in");
    EXPECT_TRUE(spec.arguments.empty());
    ASSERT_TRUE(spec.working_directory.has_value());
    EXPECT_TRUE(std::filesystem::is_directory(*spec.working_directory));
    EXPECT_EQ(spec.environment, platform::current_environment());
}

TEST_F(NewWindowTest, WithoutAProgramGivenTheRunningOneIsStarted) {
    record_starts();
    create_window();
    window_->commands().execute("workbench.action.newWindow");
    ASSERT_EQ(started_.size(), 1U);
    EXPECT_EQ(started_[0].executable.string(),
              QCoreApplication::applicationFilePath().toStdString());
}

TEST_F(NewWindowTest, EachRunStartsAnotherProcessAndTheWindowThatAskedIsNotAffected) {
    record_starts();
    create_window();
    window_->show();
    ASSERT_TRUE(window_->open_file(QString::fromStdString(write("a.txt", "abc").string())));
    type("x");
    const std::size_t documents = window_->open_document_count();

    window_->commands().execute("workbench.action.newWindow");
    window_->commands().execute("workbench.action.newWindow");
    EXPECT_EQ(started_.size(), 2U);
    EXPECT_TRUE(window_->isVisible());
    EXPECT_EQ(window_->open_document_count(), documents);
    EXPECT_EQ(current_text(), "xabc");
    EXPECT_TRUE(window_->current_document()->is_dirty());
}

TEST_F(NewWindowTest, AProgramThatCannotBeStartedIsReportedInTheStatusBar) {
    start_process_ = [](const platform::ProcessSpec&) -> std::expected<std::int64_t, Error> {
        return std::unexpected(Error{ErrorCode::io_error, "no such program"});
    };
    create_window();
    window_->commands().execute("workbench.action.newWindow");
    EXPECT_EQ(window_->status_bar().message(), "The new window cannot be started: no such program");
}

TEST_F(NewWindowTest, TheStandInProgramRunsWithNoArgumentsInADirectoryThatExists) {
    const auto output = scratch_ / "output.txt";
    const auto program = scratch_ / "stand-in.sh";
    ASSERT_TRUE(platform::write_file(program, "#!/bin/sh\necho \"$#\" > '" + output.string() +
                                                  "'\npwd >> '" + output.string() + "'\n")
                    .has_value());
    std::filesystem::permissions(program, std::filesystem::perms::owner_all);
    executable_ = program;
    create_window();
    window_->commands().execute("workbench.action.newWindow");

    std::vector<std::string> lines;
    EXPECT_TRUE(QTest::qWaitFor(
        [&] {
            lines.clear();
            std::ifstream in(output);
            for (std::string line; std::getline(in, line);) {
                lines.push_back(line);
            }
            return lines.size() == 2;
        },
        5000));
    ASSERT_EQ(lines.size(), 2U);
    EXPECT_EQ(lines[0], "0");
    EXPECT_TRUE(std::filesystem::is_directory(lines[1])) << lines[1];
}
