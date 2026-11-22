#include "anvil/ui/main_window.hpp"

#include "anvil/platform/editor_defaults.hpp"
#include "anvil/platform/file.hpp"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTabBar>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>

namespace anvil::ui {
namespace {

QString display_name(const core::Uri& uri) {
    if (const auto path = uri.file_path()) {
        return QFileInfo(QString::fromStdString(path->string())).fileName();
    }
    return QString::fromStdString(uri.path);
}

QString tab_title(const core::Uri& uri, const core::Document& document) {
    // A single ampersand would be taken as a keyboard mnemonic marker.
    QString name = display_name(uri).replace('&', "&&");
    return document.is_dirty() ? QString::fromUtf8("● ") + name : name;
}

} // namespace

MainWindow::MainWindow(Options options, QWidget* parent)
    : QMainWindow(parent), options_(std::move(options)), colors_(palette()), bridge_(this) {
    resize(1000, 700);
    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    tabs_ = new QTabBar(central);
    tabs_->setTabsClosable(true);
    tabs_->setMovable(true);
    tabs_->setExpanding(false);
    tabs_->setDocumentMode(true);
    tabs_->setFocusPolicy(Qt::NoFocus);
    view_ = new EditorView(central);
    find_bar_ = new FindBar(view_, central);
    palette_ = new QuickPick(this, colors_);
    palette_->set_ignored_prefix(">");
    palette_->set_placeholder(tr("Type the name of a command to run"));
    palette_->set_empty_message(tr("No matching commands"));
    connect(palette_, &QuickPick::accepted, this, [this](std::size_t item) {
        if (item < palette_ids_.size()) {
            run_palette_command(palette_ids_[item]);
        }
    });
    layout->addWidget(tabs_);
    layout->addWidget(find_bar_);
    layout->addWidget(view_, 1);
    workbench_ = new Workbench(colors_, this);
    workbench_->set_editor_area(central);
    workbench_->set_status_bar(statusBar());
    setCentralWidget(workbench_);
    sidebar_ = new Sidebar(*workbench_, view_containers_, colors_, this);
    panel_ = new Panel(*workbench_, view_containers_, colors_, this);

    position_label_ = new QLabel(this);
    host_label_ = new QLabel(this);
    statusBar()->addPermanentWidget(host_label_);
    statusBar()->addPermanentWidget(position_label_);
    register_commands();
    build_menus();

    auto logger = options_.logger ? options_.logger : std::make_shared<core::NullLogger>();
    settings_ = new SettingsStore(
        {.file = options_.settings_file, .defaults = platform::editor_defaults(), .logger = logger},
        this);
    connect(settings_, &SettingsStore::changed, this, &MainWindow::apply_settings);
    connect(settings_, &SettingsStore::load_failed, this, [this](const QString& message) {
        statusBar()->showMessage(
            tr("Settings not applied, keeping the previous ones: %1").arg(message), 15000);
    });
    settings_->start();

    connect(tabs_, &QTabBar::currentChanged, this, [this](int index) {
        if (index >= 0 && static_cast<std::size_t>(index) != current_) {
            activate(static_cast<std::size_t>(index));
        }
    });
    connect(tabs_, &QTabBar::tabMoved, this, &MainWindow::move_document);
    connect(tabs_, &QTabBar::tabCloseRequested, this,
            [this](int index) { close_document(static_cast<std::size_t>(index)); });
    connect(view_, &EditorView::text_changed, this, &MainWindow::on_text_changed);
    connect(view_, &EditorView::selections_changed, this, &MainWindow::on_selections_changed);

    if (options_.extension_host) {
        ExtensionHostBridge::Callbacks callbacks;
        callbacks.edited = [this](const core::Uri& uri, const core::DocumentChange&) {
            const auto index = index_of(uri);
            if (!index) {
                return;
            }
            update_document(*index);
            if (index == current_) {
                view_->document_changed_elsewhere();
                update_status();
            }
        };
        callbacks.saved = [this](const core::Uri& uri) {
            if (const auto index = index_of(uri)) {
                update_document(*index);
            }
        };
        callbacks.show_requested = [this](const core::Uri& uri) {
            if (auto document = bridge_.find(uri)) {
                show_document(uri, std::move(document));
            }
        };
        callbacks.stopped = [this](const std::string& reason) {
            host_label_->setText(tr("Extension host stopped"));
            statusBar()->showMessage(
                tr("The extension host stopped: %1").arg(QString::fromStdString(reason)), 10000);
        };
        if (auto started = bridge_.start(*options_.extension_host, logger, std::move(callbacks));
            !started) {
            statusBar()->showMessage(tr("The extension host did not start: %1")
                                         .arg(QString::fromStdString(started.error().message)),
                                     15000);
        } else {
            share_commands_with_extensions();
        }
    }

    apply_settings();
    add_untitled_document();
}

MainWindow::~MainWindow() = default;

void MainWindow::apply_settings() {
    const core::EditorSettings& editor = settings_->editor();
    view_->apply_settings(editor);
    bridge_.set_editor_options({.tab_size = static_cast<std::size_t>(editor.tab_size),
                                .insert_spaces = editor.insert_spaces});
}

void MainWindow::register_commands() {
    const auto add = [this](std::string id, const QString& title, std::function<void()> action) {
        commands_.add(std::move(id), title.toStdString(),
                      [action = std::move(action)](const nlohmann::json&) { action(); });
    };
    using core::Motion;

    add("undo", tr("Undo"), [this] { view_->undo(); });
    add("redo", tr("Redo"), [this] { view_->redo(); });
    add("cursorLeft", tr("Move Cursor Left"), [this] { view_->move(Motion::left); });
    add("cursorRight", tr("Move Cursor Right"), [this] { view_->move(Motion::right); });
    add("cursorUp", tr("Move Cursor Up"), [this] { view_->move(Motion::up); });
    add("cursorDown", tr("Move Cursor Down"), [this] { view_->move(Motion::down); });
    add("cursorHome", tr("Move Cursor to Line Start"), [this] { view_->move(Motion::line_start); });
    add("cursorEnd", tr("Move Cursor to Line End"), [this] { view_->move(Motion::line_end); });
    add("deleteLeft", tr("Delete Left"), [this] { view_->delete_backward(); });
    add("deleteRight", tr("Delete Right"), [this] { view_->delete_forward(); });
    add("editor.action.selectAll", tr("Select All"), [this] { view_->select_all(); });
    add("editor.action.clipboardCutAction", tr("Cut"), [this] { view_->cut(); });
    add("editor.action.clipboardCopyAction", tr("Copy"), [this] { view_->copy(); });
    add("editor.action.clipboardPasteAction", tr("Paste"), [this] { view_->paste(); });
    add("actions.find", tr("Find"), [this] { find_bar_->open(false); });
    add("editor.action.startFindReplace", tr("Replace"), [this] { find_bar_->open(true); });
    add("editor.action.nextMatchFindAction", tr("Find Next"), [this] { find_bar_->find_next(); });
    add("editor.action.previousMatchFindAction", tr("Find Previous"),
        [this] { find_bar_->find_previous(); });
    add("workbench.action.files.openFile", tr("Open File…"), [this] { open_dialog(); });
    add("workbench.action.files.save", tr("Save"), [this] { save(); });
    add("workbench.action.files.saveAs", tr("Save As…"), [this] { save_as_dialog(); });
    add("workbench.action.closeActiveEditor", tr("Close Editor"),
        [this] { close_document(current_); });
    add("workbench.action.showCommands", tr("Show All Commands"),
        [this] { show_command_palette(); });
    add("workbench.action.toggleSidebarVisibility", tr("Toggle Primary Side Bar Visibility"),
        [this] { workbench_->toggle(Part::sidebar); });
    add("workbench.action.togglePanel", tr("Toggle Panel Visibility"),
        [this] { workbench_->toggle(Part::panel); });
    add("workbench.action.toggleActivityBarVisibility", tr("Toggle Activity Bar Visibility"),
        [this] { workbench_->toggle(Part::activity_bar); });
    add("workbench.action.toggleMaximizedPanel", tr("Toggle Maximized Panel"),
        [this] { workbench_->set_maximized(Part::panel, !workbench_->is_maximized(Part::panel)); });
}

void MainWindow::share_commands_with_extensions() {
    for (const CommandRegistry::Command& command : commands_.commands()) {
        bridge_.host()->commands().add_builtin(
            command.id, [this, id = command.id](const nlohmann::json& args) {
                commands_.execute(id, args);
                return exthost::CallResult(std::optional<std::string>());
            });
    }
}

void MainWindow::build_menus() {
    const auto run = [this](std::string id) {
        return [this, id = std::move(id)] { commands_.execute(id); };
    };

    QMenu* file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("&Open…"), QKeySequence::Open, this, run("workbench.action.files.openFile"));
    file->addAction(tr("&Save"), QKeySequence::Save, this, run("workbench.action.files.save"));
    file->addAction(tr("Save &As…"), QKeySequence::SaveAs, this,
                    run("workbench.action.files.saveAs"));
    file->addAction(tr("&Close Tab"), QKeySequence::Close, this,
                    run("workbench.action.closeActiveEditor"));
    file->addSeparator();
    file->addAction(tr("&Quit"), QKeySequence::Quit, this, &QWidget::close);

    QMenu* edit = menuBar()->addMenu(tr("&Edit"));
    edit->addAction(tr("&Undo"), QKeySequence::Undo, this, run("undo"));
    edit->addAction(tr("&Redo"), QKeySequence::Redo, this, run("redo"));
    edit->addSeparator();
    edit->addAction(tr("Cu&t"), QKeySequence::Cut, this, run("editor.action.clipboardCutAction"));
    edit->addAction(tr("&Copy"), QKeySequence::Copy, this,
                    run("editor.action.clipboardCopyAction"));
    edit->addAction(tr("&Paste"), QKeySequence::Paste, this,
                    run("editor.action.clipboardPasteAction"));
    edit->addSeparator();
    edit->addAction(tr("Select &All"), QKeySequence::SelectAll, this,
                    run("editor.action.selectAll"));
    edit->addSeparator();
    edit->addAction(tr("&Find…"), QKeySequence::Find, this, run("actions.find"));
    edit->addAction(tr("Find &Next"), QKeySequence::FindNext, this,
                    run("editor.action.nextMatchFindAction"));
    edit->addAction(tr("Find Pre&vious"), QKeySequence::FindPrevious, this,
                    run("editor.action.previousMatchFindAction"));
    edit->addAction(tr("&Replace…"), QKeySequence::Replace, this,
                    run("editor.action.startFindReplace"));

    QMenu* view = menuBar()->addMenu(tr("&View"));
    view->addAction(tr("Toggle &Sidebar"), QKeySequence(Qt::CTRL | Qt::Key_B), this,
                    run("workbench.action.toggleSidebarVisibility"));
    view->addAction(tr("Toggle &Panel"), QKeySequence(Qt::CTRL | Qt::Key_J), this,
                    run("workbench.action.togglePanel"));

    QMenu* commands = menuBar()->addMenu(tr("Comm&ands"));
    QAction* show_commands =
        commands->addAction(tr("&Show All Commands…"), this, run("workbench.action.showCommands"));
    show_commands->setShortcuts(
        {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P), QKeySequence(Qt::Key_F1)});

    window_menu_ = menuBar()->addMenu(tr("Wi&ndow"));
    connect(window_menu_, &QMenu::aboutToShow, this, [this] {
        window_menu_->clear();
        auto* group = new QActionGroup(window_menu_);
        for (std::size_t i = 0; i < documents_.size(); ++i) {
            QAction* action = window_menu_->addAction(display_name(documents_[i].uri));
            action->setCheckable(true);
            action->setChecked(i == current_);
            group->addAction(action);
            connect(action, &QAction::triggered, this,
                    [this, i] { show_document(documents_[i].uri, documents_[i].document); });
        }
    });
}

std::optional<std::size_t> MainWindow::index_of(const core::Uri& uri) const {
    for (std::size_t i = 0; i < documents_.size(); ++i) {
        if (documents_[i].uri == uri) {
            return i;
        }
    }
    return std::nullopt;
}

void MainWindow::add_untitled_document() {
    const core::Uri uri{"untitled", "", "Untitled-" + std::to_string(next_untitled_++), "", ""};
    auto document = std::make_shared<core::Document>(core::Text{}, settings_->editor().line_ending);
    bridge_.add_document(uri, document);
    show_document(uri, std::move(document));
}

void MainWindow::show_document(const core::Uri& uri, std::shared_ptr<core::Document> document) {
    auto index = index_of(uri);
    if (!index) {
        documents_.push_back({uri, std::move(document)});
        {
            const QSignalBlocker blocker(tabs_);
            tabs_->addTab(QString());
        }
        index = documents_.size() - 1;
        update_tab(*index);
    }
    activate(*index);
}

void MainWindow::activate(std::size_t index) {
    current_ = index;
    {
        const QSignalBlocker blocker(tabs_);
        tabs_->setCurrentIndex(static_cast<int>(index));
    }
    view_->set_document(documents_[index].document);
    bridge_.show(documents_[index].uri);
    update_title();
    update_status();
    view_->setFocus();
}

void MainWindow::move_document(int from, int to) {
    const auto first = documents_.begin();
    if (from < to) {
        std::rotate(first + from, first + from + 1, first + to + 1);
    } else {
        std::rotate(first + to, first + from, first + from + 1);
    }
    current_ = static_cast<std::size_t>(tabs_->currentIndex());
}

bool MainWindow::close_document(std::size_t index) {
    if (index >= documents_.size()) {
        return false;
    }
    if (!confirm_close(index)) {
        return false;
    }
    // The replacement goes in first so the window never has nothing to show.
    if (documents_.size() == 1) {
        add_untitled_document();
    }
    bridge_.remove_document(documents_[index].uri);
    documents_.erase(documents_.begin() + static_cast<std::ptrdiff_t>(index));
    {
        const QSignalBlocker blocker(tabs_);
        tabs_->removeTab(static_cast<int>(index));
    }
    if (index < current_) {
        --current_;
    } else if (index == current_) {
        activate(std::min(index, documents_.size() - 1));
    }
    return true;
}

bool MainWindow::confirm_close(std::size_t index) {
    if (!documents_[index].document->is_dirty() || !options_.confirm_unsaved_changes) {
        return true;
    }
    activate(index);
    const auto answer =
        QMessageBox::question(this, tr("Unsaved changes"),
                              tr("Save changes to %1?").arg(display_name(documents_[index].uri)),
                              QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    return answer == QMessageBox::Discard || (answer == QMessageBox::Save && save_document(index));
}

void MainWindow::update_document(std::size_t index) {
    update_tab(index);
    if (index == current_) {
        update_title();
    }
}

void MainWindow::update_tab(std::size_t index) {
    const auto& open = documents_[index];
    const int tab = static_cast<int>(index);
    const QString title = tab_title(open.uri, *open.document);
    const QString tooltip = QString::fromStdString(
        open.uri.file_path() ? open.uri.file_path()->string() : open.uri.to_string());
    // Setting a tab property, even to its current value, makes Qt lay out the whole tab bar,
    // which would make every keystroke cost time proportional to the number of tabs.
    if (tabs_->tabText(tab) != title) {
        tabs_->setTabText(tab, title);
    }
    if (tabs_->tabToolTip(tab) != tooltip) {
        tabs_->setTabToolTip(tab, tooltip);
    }
}

bool MainWindow::open_file(const QString& path) {
    const std::filesystem::path file = QFileInfo(path).absoluteFilePath().toStdString();
    const core::Uri uri = core::Uri::file(file);
    if (const auto index = index_of(uri)) {
        show_document(uri, documents_[*index].document);
        return true;
    }
    // An extension may have opened the document already; share it so both see the same text.
    std::shared_ptr<core::Document> document = bridge_.find(uri);
    if (!document) {
        auto bytes = platform::read_file(file);
        if (!bytes) {
            QMessageBox::warning(this, tr("Open"), QString::fromStdString(bytes.error().message));
            return false;
        }
        auto decoded = core::Document::from_file_bytes(*bytes);
        if (!decoded) {
            QMessageBox::warning(this, tr("Open"), tr("%1 is not a UTF-8 text file.").arg(path));
            return false;
        }
        document = std::make_shared<core::Document>(std::move(*decoded));
        bridge_.add_document(uri, document);
    }
    show_document(uri, std::move(document));
    return true;
}

bool MainWindow::write_document(std::size_t index, const std::filesystem::path& path) {
    auto& open = documents_[index];
    if (auto written = platform::write_file(path, open.document->to_file_bytes()); !written) {
        QMessageBox::critical(this, tr("Save"), QString::fromStdString(written.error().message));
        return false;
    }
    open.document->mark_saved();
    bridge_.saved(open.uri);
    update_document(index);
    return true;
}

bool MainWindow::save() {
    return save_document(current_);
}

bool MainWindow::save_as(const QString& path) {
    return save_document_as(current_, path);
}

bool MainWindow::save_document(std::size_t index) {
    if (const auto path = documents_[index].uri.file_path()) {
        return write_document(index, *path);
    }
    const QString chosen = QFileDialog::getSaveFileName(this, tr("Save As"));
    return !chosen.isEmpty() && save_document_as(index, chosen);
}

bool MainWindow::save_document_as(std::size_t index, const QString& path) {
    const std::filesystem::path file = QFileInfo(path).absoluteFilePath().toStdString();
    const core::Uri new_uri = core::Uri::file(file);
    if (const auto existing = index_of(new_uri); existing && *existing != index) {
        QMessageBox::warning(this, tr("Save As"),
                             tr("%1 is already open in the editor.").arg(path));
        return false;
    }
    if (!write_document(index, file)) {
        return false;
    }
    auto& open = documents_[index];
    if (!(open.uri == new_uri)) {
        // The document keeps its text and history; extensions see it under its new name.
        bridge_.remove_document(open.uri);
        open.uri = new_uri;
        bridge_.add_document(new_uri, open.document);
        if (index == current_) {
            bridge_.show(new_uri);
        }
    }
    update_document(index);
    return true;
}

void MainWindow::save_as_dialog() {
    const QString chosen = QFileDialog::getSaveFileName(this, tr("Save As"));
    if (!chosen.isEmpty()) {
        save_as(chosen);
    }
}

void MainWindow::open_dialog() {
    const QString chosen = QFileDialog::getOpenFileName(this, tr("Open"));
    if (!chosen.isEmpty()) {
        open_file(chosen);
    }
}

void MainWindow::show_command_palette() {
    if (QApplication::activeModalWidget()) {
        return;
    }
    struct Entry {
        std::string id;
        QString title;
    };
    std::vector<Entry> entries;
    for (const CommandRegistry::Command& command : commands_.commands()) {
        entries.push_back({command.id, QString::fromStdString(command.title)});
    }
    if (bridge_.running()) {
        for (const std::string& id : bridge_.host()->commands().extension_commands()) {
            if (!id.starts_with("_") && !commands_.find(id)) {
                entries.push_back({id, QString::fromStdString(id)});
            }
        }
    }
    std::ranges::sort(entries, [](const Entry& a, const Entry& b) {
        return a.title.compare(b.title, Qt::CaseInsensitive) < 0;
    });
    for (auto recent = recent_commands_.rbegin(); recent != recent_commands_.rend(); ++recent) {
        const auto found = std::ranges::find(entries, *recent, &Entry::id);
        if (found != entries.end()) {
            std::rotate(entries.begin(), found, found + 1);
        }
    }

    palette_ids_.clear();
    std::vector<QuickPickItem> items;
    for (Entry& entry : entries) {
        palette_ids_.push_back(std::move(entry.id));
        items.push_back({entry.title, {}, {}, {}});
    }
    palette_->set_items(std::move(items));
    palette_->set_top_offset(menuBar()->height());
    palette_->open(">");
}

void MainWindow::run_palette_command(const std::string& id) {
    if (QApplication::activeModalWidget()) {
        return;
    }
    std::erase(recent_commands_, id);
    recent_commands_.insert(recent_commands_.begin(), id);
    constexpr std::size_t remembered = 30;
    if (recent_commands_.size() > remembered) {
        recent_commands_.resize(remembered);
    }

    if (bridge_.running()) {
        bridge_.host()->commands().execute(id, "[]", [this, id](exthost::CallResult result) {
            if (!result) {
                statusBar()->showMessage(
                    tr("%1 failed: %2")
                        .arg(QString::fromStdString(id), QString::fromStdString(result.error())),
                    8000);
            }
        });
    } else if (!commands_.execute(id)) {
        statusBar()->showMessage(tr("Command %1 not found").arg(QString::fromStdString(id)), 5000);
    }
}

void MainWindow::on_text_changed(const core::DocumentChange& change) {
    bridge_.text_changed(current_uri(), change);
    update_document(current_);
    update_status();
}

void MainWindow::on_selections_changed() {
    bridge_.selections_changed(current_uri());
    update_status();
}

void MainWindow::update_title() {
    const auto& open = documents_[current_];
    setWindowTitle(tr("%1 — Anvil[*]").arg(display_name(open.uri)));
    setWindowModified(open.document->is_dirty());
}

void MainWindow::update_status() {
    const auto& document = *documents_[current_].document;
    const core::Position position =
        document.text().position_at(document.selections().primary().active);
    QString text = tr("Ln %1, Col %2").arg(position.line + 1).arg(position.column + 1);
    if (document.selections().all().size() > 1) {
        text += tr("  (%1 cursors)").arg(document.selections().all().size());
    }
    position_label_->setText(text);
    host_label_->setText(bridge_.running() ? tr("Extension host: running")
                                           : tr("Extension host: off"));
}

bool MainWindow::resolve_unsaved_changes() {
    for (std::size_t i = 0; i < documents_.size(); ++i) {
        if (!confirm_close(i)) {
            return false;
        }
    }
    return true;
}

void MainWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::PaletteChange) {
        colors_.set_palette(palette());
    }
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (resolve_unsaved_changes()) {
        event->accept();
    } else {
        event->ignore();
    }
}

} // namespace anvil::ui
