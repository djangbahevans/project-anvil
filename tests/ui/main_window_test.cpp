#include "anvil/platform/file.hpp"
#include "anvil/ui/main_window.hpp"

#include <QApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QScrollBar>
#include <QTest>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <unistd.h>

using namespace anvil;
using anvil::ui::MainWindow;
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

    void create_window(bool with_extension_host = false) {
        ui::MainWindowOptions options;
        options.confirm_unsaved_changes = false;
        if (with_extension_host) {
            const std::filesystem::path vscode_dir = ANVIL_VSCODE_DIR;
            const std::filesystem::path node = ANVIL_NODE_EXECUTABLE;
            if (node.empty() ||
                !std::filesystem::exists(vscode_dir / "out" / "bootstrap-fork.js")) {
                GTEST_SKIP() << "needs Node.js and the transpiled VS Code checkout "
                                "(scripts/fetch-vscode.sh)";
            }
            options.extension_host = ui::ExtensionHostBridge::Config{
                node, vscode_dir, {ANVIL_DOCUMENTS_EXTENSION}, scratch_ / "host"};
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
        for (const QLabel* label : window_->findChildren<QLabel*>()) {
            if (label->text().contains(fragment)) {
                return label->text();
            }
        }
        return {};
    }

    std::string current_text() const {
        return window_->current_document()->text().to_string();
    }

    std::filesystem::path scratch_;
    std::unique_ptr<MainWindow> window_;
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

TEST_F(MainWindowTest, ExtensionCommandsAreAvailableFromTheCommandList) {
    create_window(true);
    if (!window_) {
        return;
    }
    const auto ids = window_->bridge().host()->commands().command_ids();
    EXPECT_NE(std::find(ids.begin(), ids.end(), "anvil.docs.insert"), ids.end());
}
