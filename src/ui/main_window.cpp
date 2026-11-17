#include "anvil/ui/main_window.hpp"

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
#include <QStatusBar>

namespace anvil::ui {
namespace {

QString display_name(const core::Uri& uri) {
    if (const auto path = uri.file_path()) {
        return QFileInfo(QString::fromStdString(path->string())).fileName();
    }
    return QString::fromStdString(uri.path);
}

} // namespace

MainWindow::MainWindow(Options options, QWidget* parent)
    : QMainWindow(parent), options_(std::move(options)), bridge_(this) {
    resize(1000, 700);
    view_ = new EditorView(this);
    setCentralWidget(view_);

    position_label_ = new QLabel(this);
    host_label_ = new QLabel(this);
    statusBar()->addPermanentWidget(host_label_);
    statusBar()->addPermanentWidget(position_label_);
    build_menus();

    connect(view_, &EditorView::text_changed, this, &MainWindow::on_text_changed);
    connect(view_, &EditorView::selections_changed, this, &MainWindow::on_selections_changed);

    if (options_.extension_host) {
        ExtensionHostBridge::Callbacks callbacks;
        callbacks.edited = [this](const core::Uri& uri, const core::DocumentChange&) {
            if (index_of(uri) == current_) {
                view_->document_changed_elsewhere();
                update_title();
                update_status();
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
        auto logger = options_.logger ? options_.logger : std::make_shared<core::NullLogger>();
        if (auto started =
                bridge_.start(*options_.extension_host, std::move(logger), std::move(callbacks));
            !started) {
            statusBar()->showMessage(tr("The extension host did not start: %1")
                                         .arg(QString::fromStdString(started.error().message)),
                                     15000);
        }
    }

    add_untitled_document();
}

MainWindow::~MainWindow() = default;

void MainWindow::build_menus() {
    QMenu* file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("&Open…"), QKeySequence::Open, this, &MainWindow::open_dialog);
    file->addAction(tr("&Save"), QKeySequence::Save, this, [this] { save(); });
    file->addAction(tr("Save &As…"), QKeySequence::SaveAs, this, &MainWindow::save_as_dialog);
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

    QMenu* commands = menuBar()->addMenu(tr("&Commands"));
    commands->addAction(tr("&Run Command…"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_P), this,
                        &MainWindow::run_command);

    window_menu_ = menuBar()->addMenu(tr("&Window"));
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
    auto document = std::make_shared<core::Document>();
    bridge_.add_document(uri, document);
    show_document(uri, std::move(document));
}

void MainWindow::show_document(const core::Uri& uri, std::shared_ptr<core::Document> document) {
    if (const auto index = index_of(uri)) {
        current_ = *index;
    } else {
        documents_.push_back({uri, std::move(document)});
        current_ = documents_.size() - 1;
    }
    view_->set_document(documents_[current_].document);
    bridge_.show(uri);
    update_title();
    update_status();
    view_->setFocus();
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

bool MainWindow::write_current(const std::filesystem::path& path) {
    auto& open = documents_[current_];
    if (auto written = platform::write_file(path, open.document->to_file_bytes()); !written) {
        QMessageBox::critical(this, tr("Save"), QString::fromStdString(written.error().message));
        return false;
    }
    open.document->mark_saved();
    bridge_.saved(open.uri);
    update_title();
    return true;
}

bool MainWindow::save() {
    if (const auto path = current_uri().file_path()) {
        return write_current(*path);
    }
    const QString chosen = QFileDialog::getSaveFileName(this, tr("Save As"));
    return !chosen.isEmpty() && save_as(chosen);
}

bool MainWindow::save_as(const QString& path) {
    const std::filesystem::path file = QFileInfo(path).absoluteFilePath().toStdString();
    const core::Uri new_uri = core::Uri::file(file);
    if (index_of(new_uri) && *index_of(new_uri) != current_) {
        QMessageBox::warning(this, tr("Save As"),
                             tr("%1 is already open in the editor.").arg(path));
        return false;
    }
    auto& open = documents_[current_];
    if (!write_current(file)) {
        return false;
    }
    if (!(open.uri == new_uri)) {
        // The document keeps its text and history; extensions see it under its new name.
        bridge_.remove_document(open.uri);
        open.uri = new_uri;
        bridge_.add_document(new_uri, open.document);
        bridge_.show(new_uri);
    }
    update_title();
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
    update_title();
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
    if (!options_.confirm_unsaved_changes) {
        return true;
    }
    for (std::size_t i = 0; i < documents_.size(); ++i) {
        if (!documents_[i].document->is_dirty()) {
            continue;
        }
        show_document(documents_[i].uri, documents_[i].document);
        const auto answer =
            QMessageBox::question(this, tr("Unsaved changes"),
                                  tr("Save changes to %1?").arg(display_name(documents_[i].uri)),
                                  QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        if (answer == QMessageBox::Cancel || (answer == QMessageBox::Save && !save())) {
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
