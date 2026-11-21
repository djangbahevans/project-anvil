#include "anvil/ui/main_window.hpp"

#include "anvil/platform/editor_defaults.hpp"
#include "anvil/platform/file.hpp"

#include <QAction>
#include <QActionGroup>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTabBar>
#include <QVBoxLayout>
#include <algorithm>

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
    : QMainWindow(parent), options_(std::move(options)), bridge_(this) {
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
    layout->addWidget(tabs_);
    layout->addWidget(find_bar_);
    layout->addWidget(view_, 1);
    setCentralWidget(central);

    position_label_ = new QLabel(this);
    host_label_ = new QLabel(this);
    statusBar()->addPermanentWidget(host_label_);
    statusBar()->addPermanentWidget(position_label_);
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

void MainWindow::build_menus() {
    QMenu* file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("&Open…"), QKeySequence::Open, this, &MainWindow::open_dialog);
    file->addAction(tr("&Save"), QKeySequence::Save, this, [this] { save(); });
    file->addAction(tr("Save &As…"), QKeySequence::SaveAs, this, &MainWindow::save_as_dialog);
    file->addAction(tr("&Close Tab"), QKeySequence::Close, this,
                    [this] { close_document(current_); });
    file->addSeparator();
    file->addAction(tr("&Quit"), QKeySequence::Quit, this, &QWidget::close);

    QMenu* edit = menuBar()->addMenu(tr("&Edit"));
    edit->addAction(tr("&Undo"), QKeySequence::Undo, view_, &EditorView::undo);
    edit->addAction(tr("&Redo"), QKeySequence::Redo, view_, &EditorView::redo);
    edit->addSeparator();
    edit->addAction(tr("Cu&t"), QKeySequence::Cut, view_, &EditorView::cut);
    edit->addAction(tr("&Copy"), QKeySequence::Copy, view_, &EditorView::copy);
    edit->addAction(tr("&Paste"), QKeySequence::Paste, view_, &EditorView::paste);
    edit->addSeparator();
    edit->addAction(tr("Select &All"), QKeySequence::SelectAll, view_, &EditorView::select_all);
    edit->addSeparator();
    edit->addAction(tr("&Find…"), QKeySequence::Find, this, [this] { find_bar_->open(false); });
    edit->addAction(tr("Find &Next"), QKeySequence::FindNext, find_bar_, &FindBar::find_next);
    edit->addAction(tr("Find Pre&vious"), QKeySequence::FindPrevious, find_bar_,
                    &FindBar::find_previous);
    edit->addAction(tr("&Replace…"), QKeySequence::Replace, this,
                    [this] { find_bar_->open(true); });

    QMenu* commands = menuBar()->addMenu(tr("Comm&ands"));
    commands->addAction(tr("&Run Command…"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P), this,
                        &MainWindow::run_command);

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

void MainWindow::run_command() {
    if (!bridge_.running()) {
        statusBar()->showMessage(tr("No extension host is running."), 5000);
        return;
    }
    QStringList ids;
    for (const std::string& id : bridge_.host()->commands().command_ids()) {
        if (!id.starts_with("_")) {
            ids << QString::fromStdString(id);
        }
    }
    if (ids.isEmpty()) {
        statusBar()->showMessage(tr("No commands are available."), 5000);
        return;
    }
    bool accepted = false;
    const QString id =
        QInputDialog::getItem(this, tr("Run Command"), tr("Command:"), ids, 0, false, &accepted);
    if (!accepted) {
        return;
    }
    bridge_.host()->commands().execute(
        id.toStdString(), "[]", [this, id](exthost::CallResult result) {
            const QString shown =
                result ? (*result ? QString::fromStdString(**result) : tr("done"))
                       : tr("failed: %1").arg(QString::fromStdString(result.error()));
            statusBar()->showMessage(tr("%1: %2").arg(id, shown), 8000);
        });
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

void MainWindow::closeEvent(QCloseEvent* event) {
    if (resolve_unsaved_changes()) {
        event->accept();
    } else {
        event->ignore();
    }
}

} // namespace anvil::ui
