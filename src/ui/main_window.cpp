#include "anvil/ui/main_window.hpp"

#include "anvil/core/editor_settings.hpp"
#include "anvil/core/go_to_line.hpp"
#include "anvil/core/untitled_title.hpp"
#include "anvil/core/zoom.hpp"
#include "anvil/exthost/contributions.hpp"
#include "anvil/exthost/documents_service.hpp"
#include "anvil/exthost/workspace_file_edit.hpp"
#include "anvil/platform/editor_defaults.hpp"
#include "anvil/platform/file.hpp"
#include "anvil/platform/file_system.hpp"
#include "anvil/ui/codicons.hpp"
#include "anvil/ui/default_keybindings.hpp"
#include "anvil/ui/keybindings_reference.hpp"
#include "anvil/ui/menu_style.hpp"
#include "anvil/ui/project_links.hpp"
#include "anvil/ui/resources.hpp"
#include "anvil/ui/seti_icons.hpp"

#include <QAbstractButton>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontInfo>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollBar>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>
#include <QWindow>
#include <algorithm>
#include <cmath>
#include <functional>
#include <set>
#include <system_error>
#include <utility>

namespace anvil::ui {
namespace {

// The folders that are expanded in the explorer, in the state of the workspace.
constexpr const char* expanded_folders_key = "explorer.expandedFolders";
// The folder of the window closed last, empty when it had none.
constexpr const char* last_folder_key = "window.lastFolder";
// The editor groups and their file tabs, in the state of the workspace.
constexpr const char* editor_layout_key = "editor.layout";

constexpr const char* position_item_id = "status.editor.position";
constexpr const char* language_item_id = "status.editor.language";
constexpr const char* host_item_id = "status.extensionHost";
constexpr int position_priority = 100;
constexpr int language_priority = 90;

core::KeyPlatform key_platform() {
#if defined(Q_OS_MACOS)
    return core::KeyPlatform::macos;
#elif defined(Q_OS_WIN)
    return core::KeyPlatform::windows;
#else
    return core::KeyPlatform::linux;
#endif
}

// Whether a `when` expression of a manifest holds; an empty one holds, and so does one that
// cannot be read, so that a typo in an extension does not take its command away.
bool holds(const std::string& expression, const core::ContextKeys& context) {
    if (expression.empty()) {
        return true;
    }
    const auto parsed = core::WhenExpression::parse(expression);
    return !parsed || parsed->evaluate(context);
}

// A command is in the palette unless it has rules for it and none of them holds.
bool shown_in_palette(const exthost::Contributions& contributions, const std::string& command,
                      const core::ContextKeys& context) {
    bool has_rule = false;
    for (const exthost::PaletteRule& rule : contributions.palette_rules) {
        if (rule.command == command) {
            has_rule = true;
            if (holds(rule.when, context)) {
                return true;
            }
        }
    }
    return !has_rule;
}

QString display_name(const core::Uri& uri) {
    if (const auto path = uri.file_path()) {
        return QFileInfo(QString::fromStdString(path->string())).fileName();
    }
    return QString::fromStdString(uri.path);
}

std::optional<QuickPickIcon> quick_pick_icon(const SetiIcon& icon) {
    const QString family = seti_font_family();
    if (family.isEmpty()) {
        return std::nullopt;
    }
    return QuickPickIcon{
        .font_family = family,
        .dark = {icon.dark.code, QColor::fromRgba(icon.dark.color)},
        .light = {icon.light.code, QColor::fromRgba(icon.light.color)},
    };
}

} // namespace

// A close operation, which the closed editors of one operation, such as Close Others, are batched
// by.
class MainWindow::ClosingScope {
public:
    explicit ClosingScope(MainWindow& window) : window_(window) {
        if (window_.closing_depth_++ == 0) {
            window_.closed_editors_.begin_batch();
        }
    }
    ~ClosingScope() {
        --window_.closing_depth_;
    }
    ClosingScope(const ClosingScope&) = delete;
    ClosingScope& operator=(const ClosingScope&) = delete;

private:
    MainWindow& window_;
};

class MainWindow::LayoutWatcher : public workbench::EditorLayout::Observer {
public:
    explicit LayoutWatcher(MainWindow& window) : window_(window) {}

    void structure_changed() override {
        window_.prune_navigation_history();
        window_.refresh_current();
        save_later();
    }
    void ratios_changed() override {
        save_later();
    }
    void tabs_changed(workbench::GroupId) override {
        window_.prune_navigation_history();
        window_.refresh_current();
        save_later();
    }
    void active_tab_changed(workbench::GroupId) override {
        window_.refresh_current();
        save_later();
    }
    void focus_changed(workbench::GroupId) override {
        window_.refresh_current();
        save_later();
    }

private:
    void save_later() {
        if (window_.state_keeper_) {
            window_.state_keeper_->schedule();
        }
    }

    MainWindow& window_;
};

MainWindow::MainWindow(Options options, QWidget* parent)
    : QMainWindow(parent), options_(std::move(options)), colors_(palette()), zoom_(this),
      bridge_(this) {
    about_versions_ =
        about_versions(options_.extension_host ? std::optional(options_.extension_host->vscode_dir)
                                               : std::nullopt);
    resize(1000, 700);
    palette_ = new QuickPick(this, colors_);
    palette_->set_zoom(&zoom_);
    palette_->set_ignored_prefix(">");
    palette_->set_placeholder(tr("Type the name of a command to run"));
    palette_->set_empty_message(tr("No matching commands"));
    connect(palette_, &QuickPick::accepted, this, [this](std::size_t item) {
        // Go to File answers for itself. It connects when it takes the popup over, so this runs
        // first and still sees it open.
        if (quick_open_->is_open()) {
            return;
        }
        if (quick_access_ == QuickAccess::line) {
            finish_go_to_line(true);
            return;
        }
        pick_highlighted_ = nullptr;
        pick_dismissed_ = nullptr;
        if (pick_action_) {
            const auto action = std::exchange(pick_action_, nullptr);
            action(item);
        } else if (item < palette_ids_.size()) {
            run_palette_command(palette_ids_[item]);
        }
    });
    connect(palette_, &QuickPick::active_changed, this, [this](std::size_t item) {
        if (pick_highlighted_) {
            pick_highlighted_(item);
        }
    });
    connect(palette_, &QuickPick::cancelled, this, [this] {
        finish_go_to_line(false);
        pick_action_ = nullptr;
        pick_highlighted_ = nullptr;
        if (const auto dismissed = std::exchange(pick_dismissed_, nullptr)) {
            dismissed();
        }
    });
    connect(palette_->filter_field(), &QLineEdit::textChanged, this,
            &MainWindow::follow_quick_access_text);
    quick_open_ = std::make_unique<QuickOpen>(*palette_);
    quick_open_->set_icon_source(
        [this](const core::Uri& uri) { return quick_pick_icon(file_icon_of(uri)); });
    connect(quick_open_.get(), &QuickOpen::chosen, this, &MainWindow::open_chosen);
    connect(quick_open_.get(), &QuickOpen::failed, this,
            [this](const QString& message) { status_bar_->show_message(message, 15000); });
    area_ = new EditorArea(
        layout_, colors_, [this](const core::Uri& uri) { return tab_info(uri); },
        [this](const core::Uri& uri) {
            const auto found = documents_.find(uri);
            return found == documents_.end() ? std::shared_ptr<core::Document>() : found->second;
        });
    highlighting_ = new Highlighting(this);
    area_->set_zoom(&zoom_);
    area_->set_wrapping_lookup(
        [this](const std::shared_ptr<core::Document>& document) { return wrapping_of(document); });
    area_->set_highlighter_lookup([this](const core::Uri& uri) { return highlighter_of(uri); });
    connect(highlighting_, &Highlighting::spans_arrived, this,
            [this](const core::Highlighter* highlighter) {
                for (EditorGroupWidget* group : area_->groups()) {
                    if (group->view()->highlighter().get() == highlighter) {
                        group->view()->refresh_highlighting();
                    }
                }
            });
    watcher_ = std::make_unique<LayoutWatcher>(*this);
    layout_.add_observer(watcher_.get());
    workbench_ = new Workbench(colors_, this);
    workbench_->set_zoom(&zoom_);
    workbench_->set_editor_area(area_);
    status_bar_ = new StatusBar(colors_, this);
    status_bar_->set_zoom(&zoom_);
    workbench_->set_status_bar(status_bar_);
    setCentralWidget(workbench_);

    // The window's frame is chosen when it is created, so the settings are read first.
    auto logger = options_.logger ? options_.logger : std::make_shared<core::NullLogger>();
    settings_ = new SettingsStore(
        {.file = options_.settings_file,
         .defaults = platform::editor_defaults(),
         .logger = logger,
         .is_unsaved =
             [this](const std::filesystem::path& file) { return has_unsaved_changes_in(file); }},
        this);
    connect(settings_, &SettingsStore::changed, this, &MainWindow::apply_settings);
    connect(settings_, &SettingsStore::load_failed, this, [this](const QString& message) {
        status_bar_->show_message(
            tr("Settings not applied, keeping the previous ones: %1").arg(message), 15000);
    });
    settings_->start();
    state_ = std::make_unique<workbench::StateStore>(
        options_.state_directory,
        options_.workspace ? std::optional(options_.workspace->id) : std::nullopt, logger);
    reopen_last_folder(*logger);

    custom_title_bar_ = options_.custom_title_bar &&
                        settings_->editor().title_bar_style == core::TitleBarStyle::custom &&
                        !qEnvironmentVariableIsSet("ANVIL_NATIVE_FRAME");
#if defined(Q_OS_MACOS)
    custom_title_bar_ = false;
#endif
    if (custom_title_bar_) {
        set_up_title_bar();
    } else {
        menu_bar_ = menuBar();
    }
    sidebar_ = new Sidebar(*workbench_, view_containers_, colors_, this);
    panel_ = new Panel(*workbench_, view_containers_, colors_, this);
    register_explorer();

    register_commands();
    keys_ = new KeybindingService(
        {.fill_context = [this](core::ContextKeys& keys) { fill_context(keys); },
         .run = [this](const std::string& command,
                       const nlohmann::json* args) { run_bound_command(command, args); },
         .composing =
             [] {
                 const auto* view = qobject_cast<EditorView*>(QApplication::focusWidget());
                 return view && view->composing();
             }},
        key_platform(), this);
    qApp->installEventFilter(keys_);
    connect(keys_, &KeybindingService::chord_message, this,
            [this](const QString& text, int duration) {
                if (text.isEmpty()) {
                    if (status_bar_->message() == chord_text_) {
                        status_bar_->clear_message();
                    }
                    chord_text_.clear();
                } else {
                    chord_text_ = duration == 0 ? text : QString();
                    status_bar_->show_message(text, duration);
                }
            });
    build_menus();

    user_keybindings_ =
        new KeybindingsStore({.file = options_.keybindings_file, .logger = logger}, this);
    connect(user_keybindings_, &KeybindingsStore::changed, this, &MainWindow::apply_keybindings);
    connect(
        user_keybindings_, &KeybindingsStore::load_failed, this, [this](const QString& message) {
            status_bar_->show_message(
                tr("Key bindings not applied, keeping the previous ones: %1").arg(message), 15000);
        });
    user_keybindings_->start();
    apply_keybindings();

    themes_ = new ThemeStore(colors_, options_.themes_directory.value_or(themes_directory()),
                             logger, this);
    connect(themes_, &ThemeStore::load_failed, this,
            [this](const QString& message) { status_bar_->show_message(message, 15000); });
    connect(status_bar_, &StatusBar::command_requested, this,
            [this](const QString& command) { execute_command(command.toStdString()); });

    connect(area_, &EditorArea::tab_selected, this, [this](workbench::GroupId group, int index) {
        activate_tab(group, static_cast<std::size_t>(index));
    });
    connect(
        area_, &EditorArea::tab_moved, this, [this](workbench::GroupId group, int from, int to) {
            move_tab(group, static_cast<std::size_t>(from), group, static_cast<std::size_t>(to));
        });
    connect(area_, &EditorArea::close_requested, this, [this](workbench::GroupId group, int index) {
        close_tab_at(group, static_cast<std::size_t>(index));
    });
    connect(area_, &EditorArea::close_others_requested, this,
            [this](workbench::GroupId group, int index) {
                close_tabs_where(
                    group, [index](std::size_t i) { return i != static_cast<std::size_t>(index); });
            });
    connect(area_, &EditorArea::close_to_the_right_requested, this,
            [this](workbench::GroupId group, int index) {
                close_tabs_where(
                    group, [index](std::size_t i) { return i > static_cast<std::size_t>(index); });
            });
    connect(area_, &EditorArea::close_saved_requested, this, [this](workbench::GroupId group) {
        close_tabs_where(group, [this, group](std::size_t i) {
            const core::Uri& uri = layout_.group(group)->tabs[i];
            return !documents_.at(uri)->is_dirty();
        });
    });
    connect(area_, &EditorArea::copy_path_requested, this,
            [this](workbench::GroupId group, int index) {
                const core::Uri& uri = layout_.group(group)->tabs[static_cast<std::size_t>(index)];
                if (const auto path = uri.file_path()) {
                    QApplication::clipboard()->setText(QString::fromStdString(path->string()));
                }
            });
    connect(area_, &EditorArea::text_changed, this, &MainWindow::on_text_changed);
    connect(area_, &EditorArea::new_file_requested, this, &MainWindow::add_untitled_tab);
    connect(&open_files_, &OpenFilesWatcher::changed, this, &MainWindow::files_changed);
    connect(qApp, &QApplication::focusChanged, this, &MainWindow::on_focus_changed);
    connect(area_, &EditorArea::selections_changed, this, &MainWindow::on_selections_changed);
    connect(area_, &EditorArea::tab_dropped_on_strip, this,
            [this](workbench::GroupId from, int index, workbench::GroupId to, int position) {
                move_tab(from, static_cast<std::size_t>(index), to,
                         static_cast<std::size_t>(position));
                focus_current_editor();
            });
    connect(area_, &EditorArea::tab_dropped_on_group, this,
            [this](workbench::GroupId from, int index, workbench::GroupId to) {
                const workbench::EditorGroup* target = layout_.group(to);
                move_tab(from, static_cast<std::size_t>(index), to,
                         target ? target->tabs.size() : 0);
                focus_current_editor();
            });
    connect(area_, &EditorArea::tab_dropped_on_edge, this,
            [this](workbench::GroupId from, int index, workbench::GroupId to,
                   workbench::Direction direction) {
                if (const auto created = layout_.split(to, direction)) {
                    move_tab(from, static_cast<std::size_t>(index), *created, 0);
                }
                focus_current_editor();
            });
    connect(area_, &EditorArea::focus_requested, this,
            [this](workbench::GroupId group) { layout_.focus(group); });

    bridge_.set_language_resolver([this](const core::Uri& uri, std::string_view first_line) {
        return resolve_language(uri, first_line);
    });
    if (options_.extension_host) {
        ExtensionHostBridge::Callbacks callbacks;
        callbacks.edited = [this](const core::Uri& uri, const core::DocumentChange& change) {
            const auto found = documents_.find(uri);
            if (found == documents_.end()) {
                return;
            }
            // The views follow the change before anything can ask them to paint.
            for (EditorGroupWidget* group : area_->groups()) {
                if (group->view()->document() == found->second) {
                    group->view()->document_changed_elsewhere(change);
                }
            }
            document_edited(uri, change);
            update_document(uri);
            const core::Uri* current = current_uri_ptr();
            if (current && *current == uri) {
                update_status();
            }
        };
        callbacks.saved = [this](const core::Uri& uri) { update_document(uri); };
        callbacks.renamed = [this](const core::Uri& from, const core::Uri& to) {
            document_renamed(from, to);
        };
        callbacks.deleted = [this](const core::Uri& uri) { document_deleted(uri); };
        callbacks.show_requested = [this](const core::Uri& uri) {
            if (auto document = bridge_.find(uri)) {
                show_document(uri, std::move(document));
            }
        };
        callbacks.stopped = [this](const std::string& reason) {
            status_bar_->set_item(host_item_id, StatusBarSide::right,
                                  {.text = tr("Extension host stopped")});
            status_bar_->show_message(
                tr("The extension host stopped: %1").arg(QString::fromStdString(reason)), 10000);
        };
        ExtensionHostBridge::Config host_config = *options_.extension_host;
        host_config.workspace = options_.workspace;
        // Extensions search as the editor does, and they are started by what they find, so the
        // host has the settings from its start.
        host_config.search = {.ripgrep = options_.ripgrep,
                              .options =
                                  core::FileSearchOptions::from_settings(settings_->settings())};
        if (auto started = bridge_.start(host_config, logger, std::move(callbacks)); !started) {
            status_bar_->show_message(tr("The extension host did not start: %1")
                                          .arg(QString::fromStdString(started.error().message)),
                                      15000);
        } else {
            share_commands_with_extensions();
            read_extension_keybindings(*logger);
            add_extension_themes();
        }
    }
    rebuild_languages();

    apply_settings();
    // After the settings, which say whether to. Files that are named are what opens, unless
    // `preserve` asks for the editors as well, and so is nothing in an empty window asked for.
    if ((options_.opens_files &&
         settings_->editor().restore_windows != core::RestoreWindows::preserve) ||
        (options_.empty_window && !options_.workspace) || !restore_editors()) {
        add_untitled_document();
    }

    // After the settings, whose zoom replaces a part size that is the default one.
    state_keeper_ =
        std::make_unique<WindowStateKeeper>(*state_, *this, *workbench_, *sidebar_, *panel_);
    state_keeper_->set_parts_outside_mode([this]() -> std::optional<WindowStateKeeper::PartsShown> {
        if (!zen_.active) {
            return std::nullopt;
        }
        return WindowStateKeeper::PartsShown{.sidebar = zen_.sidebar_was_visible,
                                             .panel = zen_.panel_was_visible};
    });
    state_keeper_->set_content_capture([this](workbench::StateStore& store) {
        store.set(workbench::StateScope::workspace, editor_layout_key, saved_editors().to_json());
        // An explorer that was never shown has nothing to say about what is expanded.
        if (explorer_ && explorer_->shows_folder()) {
            store.set(workbench::StateScope::workspace, expanded_folders_key,
                      explorer_->expanded_folders());
        }
    });
    state_keeper_->set_content_restore([this] {
        // At once: the view still shows the folder that was left, and the store holds the new
        // one's state.
        if (explorer_) {
            show_workspace_in(*explorer_);
        }
        switch_editors();
    });
    state_keeper_->restore();
}

MainWindow::~MainWindow() {
    // The menus are deleted with the window, after its members, and must not outlive their style.
    for (QMenu* menu : findChildren<QMenu*>()) {
        menu->setStyle(nullptr);
    }
    // Ends a listing that is running, while the window is whole.
    quick_open_->end();
    palette_->filter_field()->disconnect(this);
    // The pick closes when its widget is deleted, after the members that its handlers use are gone.
    palette_->disconnect(this);
    // Deleting the widgets moves the focus, which must not reach a window that is half destroyed.
    disconnect(qApp, &QApplication::focusChanged, this, &MainWindow::on_focus_changed);
    // The area observes the layout, which is destroyed before the child widgets are.
    delete area_;
}

QString MainWindow::workspace_label() const {
    return options_.workspace ? QString::fromStdString(options_.workspace->name) : tr("Anvil");
}

bool MainWindow::open_folder(const std::filesystem::path& folder) {
    auto workspace = core::Workspace::open(folder);
    if (!workspace) {
        status_bar_->show_message(QString::fromStdString(workspace.error().message), 8000);
        return false;
    }
    if (options_.workspace == *workspace) {
        return true;
    }
    // The editors are closed with the folder, so Cancel keeps both.
    if (!resolve_unsaved_changes()) {
        return false;
    }
    options_.workspace = std::move(*workspace);
    workspace_changed();
    return true;
}

void MainWindow::close_folder() {
    if (!options_.workspace || !resolve_unsaved_changes()) {
        return;
    }
    options_.workspace.reset();
    workspace_changed();
}

void MainWindow::open_folder_dialog() {
    std::optional<std::filesystem::path> chosen;
    if (options_.choose_folder) {
        chosen = options_.choose_folder();
    } else if (const QString folder = QFileDialog::getExistingDirectory(this, tr("Open Folder"));
               !folder.isEmpty()) {
        chosen = std::filesystem::path(folder.toStdString());
    }
    if (chosen) {
        open_folder(*chosen);
    }
}

void MainWindow::register_explorer() {
    ViewContainer explorer;
    explorer.id = "workbench.view.explorer";
    explorer.title = tr("Explorer");
    explorer.icon = codicon_icon("files", Qt::white, 24);
    explorer.order = 0;
    explorer.create = [this]() -> QWidget* {
        auto* view =
            new ExplorerView(colors_, nullptr, platform_directory_lister(),
                             options_.explorer_watch.value_or(platform_directory_watch()), nullptr,
                             options_.explorer_actions.value_or(default_explorer_actions()));
        connect(view, &ExplorerView::file_activated, this,
                [this](const QString& path) { open_file(path); });
        connect(view, &ExplorerView::created, this, [this](const QString& path, bool directory) {
            if (!directory) {
                open_file(path);
            }
        });
        connect(view, &ExplorerView::renamed, this, [this](const QString& from, const QString& to) {
            explorer_renamed(from.toStdString(), to.toStdString());
        });
        connect(
            view, &ExplorerView::deleted, this,
            [this](const std::vector<std::filesystem::path>& paths) { explorer_deleted(paths); });
        connect(view, &ExplorerView::operation_failed, this,
                [this](const QString& message) { status_bar_->show_message(message, 15000); });
        connect(view, &ExplorerView::context_menu_requested, this,
                [this](const QPoint& position) { build_explorer_menu()->exec(position); });
        connect(view, &ExplorerView::open_folder_requested, this,
                [this] { commands_.execute("workbench.action.files.openFolder"); });
        connect(view, &ExplorerView::watch_problem, this,
                [this](const QString& message) { status_bar_->show_message(message, 15000); });
        view->set_zoom(&zoom_);
        view->set_file_language([this](const std::filesystem::path& path) {
            return language_for_icon(core::Uri::file(path));
        });
        view->set_unsaved_changes_check(
            [this](const std::filesystem::path& path) { return has_unsaved_changes_within(path); });
        view->set_excludes(file_excludes_);
        connect(view, &ExplorerView::move_confirmation_turned_off, this,
                [this] { change_setting("explorer.confirmDragAndDrop", false); });
        view->set_confirm_delete(settings_->editor().explorer_confirm_delete);
        view->set_confirm_drag_and_drop(settings_->editor().explorer_confirm_drag_and_drop);
        view->set_trash_enabled(settings_->editor().enable_trash);
        connect(view, &ExplorerView::expanded_folders_changed, this, [this] {
            if (state_keeper_) {
                state_keeper_->schedule();
            }
        });
        show_workspace_in(*view);
        explorer_ = view;
        return view;
    };
    view_containers_.add(std::move(explorer));
}

void MainWindow::show_workspace_in(ExplorerView& explorer) {
    explorer.set_workspace(
        options_.workspace ? std::optional(options_.workspace->folder) : std::nullopt,
        options_.workspace ? QString::fromStdString(options_.workspace->name) : QString());
    if (!state_) {
        return;
    }
    if (const auto folders =
            state_->strings(workbench::StateScope::workspace, expanded_folders_key)) {
        explorer.restore_expanded(*folders);
    }
}

void MainWindow::apply_explorer_settings() {
    if (explorer_) {
        explorer_->set_confirm_delete(settings_->editor().explorer_confirm_delete);
        explorer_->set_confirm_drag_and_drop(settings_->editor().explorer_confirm_drag_and_drop);
        explorer_->set_trash_enabled(settings_->editor().enable_trash);
    }
}

ExplorerView* MainWindow::show_explorer() {
    sidebar_->activate("workbench.view.explorer");
    return explorer_;
}

namespace {

QString reveal_title() {
#if defined(Q_OS_WIN)
    return MainWindow::tr("Reveal in File Explorer");
#elif defined(Q_OS_MACOS)
    return MainWindow::tr("Reveal in Finder");
#else
    return MainWindow::tr("Open Containing Folder");
#endif
}

} // namespace

std::unique_ptr<QMenu> MainWindow::explorer_context_menu() {
    return build_explorer_menu();
}

std::unique_ptr<QMenu> MainWindow::build_explorer_menu() {
    auto menu = std::make_unique<QMenu>(this);
    style_menu(menu.get());
    const auto add = [&](const QString& title, const std::string& command,
                         std::function<void()> action = {}) {
        const QString shortcut = keys_->shortcut_label(command);
        QAction* item =
            menu->addAction(shortcut.isEmpty() ? title : title + QLatin1Char('\t') + shortcut);
        item->setData(QString::fromStdString(command));
        if (!action) {
            action = [this, command] { commands_.execute(command); };
        }
        connect(item, &QAction::triggered, this, std::move(action));
    };
    const std::vector<std::filesystem::path> selected =
        explorer_ ? explorer_->selected_paths() : std::vector<std::filesystem::path>();
    if (!explorer_ || explorer_->selection_is_directory()) {
        add(tr("New File..."), "explorer.newFile");
        add(tr("New Folder..."), "explorer.newFolder");
        menu->addSeparator();
    }
    // The menu takes the focus, so the command could not tell what to reveal.
    std::optional<std::filesystem::path> target =
        explorer_ ? explorer_->focused_path() : std::nullopt;
    if (!target && options_.workspace) {
        target = options_.workspace->folder;
    }
    add(reveal_title(), "revealFileInOS", [this, target] {
        if (target) {
            reveal_path(*target);
        }
    });
    if (!selected.empty()) {
        menu->addSeparator();
        add(tr("Rename..."), "renameFile");
        add(tr("Delete"), "moveFileToTrash");
    }
    return menu;
}

void MainWindow::reveal_in_file_manager() {
    std::optional<std::filesystem::path> path;
    QWidget* focus = QApplication::focusWidget();
    if (explorer_ && focus && explorer_->isAncestorOf(focus)) {
        path = explorer_->focused_path();
        if (!path && options_.workspace) {
            path = options_.workspace->folder;
        }
    }
    if (!path) {
        if (const core::Uri* uri = current_uri_ptr()) {
            path = uri->file_path();
        }
    }
    if (path) {
        reveal_path(*path);
    }
}

void MainWindow::reveal_path(const std::filesystem::path& path) {
    const QUrl url = QUrl::fromLocalFile(QString::fromStdString(path.parent_path().string()));
    const bool opened = options_.open_url ? options_.open_url(url) : QDesktopServices::openUrl(url);
    if (!opened) {
        status_bar_->show_message(tr("The folder %1 cannot be opened").arg(url.toLocalFile()),
                                  8000);
    }
}

void MainWindow::explorer_renamed(const std::filesystem::path& from,
                                  const std::filesystem::path& to) {
    // The extension host knows the documents it has, which are the open ones and the ones that
    // extensions opened; its callbacks move the open ones.
    bridge_.files_renamed(from, to);
    // What it did not know of, and a window without an extension host.
    std::vector<std::pair<core::Uri, core::Uri>> moves;
    for (const auto& [uri, document] : documents_) {
        if (const auto path = uri.file_path(); path && exthost::is_path_within(*path, from)) {
            moves.emplace_back(uri, core::Uri::file(exthost::path_moved(*path, from, to)));
        }
    }
    for (const auto& [uri, new_uri] : moves) {
        if (documents_.contains(uri) && !documents_.contains(new_uri)) {
            rekey_document(uri, new_uri, false);
            update_document(new_uri);
        }
    }
    area_->refresh_highlighters();
}

void MainWindow::explorer_deleted(const std::vector<std::filesystem::path>& paths) {
    for (const std::filesystem::path& path : paths) {
        bridge_.files_deleted(path);
        std::vector<core::Uri> gone;
        for (const auto& [uri, document] : documents_) {
            if (const auto file = uri.file_path();
                file && exthost::is_path_within(*file, path) && !document->is_dirty()) {
                gone.push_back(uri);
            }
        }
        for (const core::Uri& uri : gone) {
            bridge_.remove_document(uri);
            document_deleted(uri);
        }
    }
}

void MainWindow::workspace_changed() {
    // The mode is not saved, and would keep the layout of the new workspace from showing.
    if (zen_.active) {
        toggle_zen_mode();
    }
    const QPointer<QWidget> focused = focusWidget();
    state_keeper_->set_workspace(options_.workspace ? std::optional(options_.workspace->id)
                                                    : std::nullopt);
    // The parts are hidden for a moment while the layout changes, which moves the focus.
    if (focused && focused->isVisible()) {
        focused->setFocus();
    } else {
        view()->setFocus();
    }
    if (options_.extension_host) {
        core::NullLogger discarded;
        core::Logger& logger = options_.logger ? *options_.logger : discarded;
        bridge_.set_search_settings({.ripgrep = options_.ripgrep, .options = search_options_});
        auto restarted = bridge_.restart(options_.workspace);
        rebuild_languages();
        if (!restarted) {
            status_bar_->show_message(tr("The extension host did not start: %1")
                                          .arg(QString::fromStdString(restarted.error().message)),
                                      15000);
        } else {
            for (const auto& [uri, document] : documents_) {
                const auto chosen = chosen_languages_.find(uri);
                bridge_.add_document(uri, document,
                                     chosen == chosen_languages_.end()
                                         ? std::nullopt
                                         : std::optional<std::string>(chosen->second));
            }
            apply_extension_host_options();
            if (shown_uri_) {
                bridge_.show(*shown_uri_);
            }
            share_commands_with_extensions();
            read_extension_keybindings(logger);
            add_extension_themes();
        }
    }
    if (title_bar_) {
        title_bar_->set_command_center_text(workspace_label());
    }
    update_title();
    update_status();
}

void MainWindow::add_extension_themes() {
    std::vector<ThemeStore::Entry> entries;
    for (const exthost::ContributedTheme& theme : bridge_.contributions().themes) {
        // A restart reads the same manifests again, and a theme that was left out the first time
        // is not reported again.
        if (!offered_themes_.insert(theme.id + '\n' + theme.path.string()).second) {
            continue;
        }
        entries.push_back({.id = theme.id,
                           .label = theme.label,
                           .kind = core::theme_kind_from_ui_theme(theme.ui_theme),
                           .file = theme.path});
    }
    themes_->add_themes(std::move(entries));
}

void MainWindow::read_extension_keybindings(core::Logger& logger) {
    core::KeybindingFile file =
        exthost::extension_keybindings(bridge_.contributions(), key_platform());
    for (const std::string& problem : file.problems) {
        logger.log(core::LogLevel::warning, "extensions", problem);
    }
    extension_keybindings_ = std::move(file.bindings);
    apply_keybindings();
}

void MainWindow::apply_keybindings() {
    std::vector<core::Keybinding> bindings = default_keybindings(key_platform());
    bindings.insert(bindings.end(), extension_keybindings_.begin(), extension_keybindings_.end());
    const auto& user = user_keybindings_->bindings();
    bindings.insert(bindings.end(), user.begin(), user.end());
    keys_->set_bindings(std::move(bindings));
    update_title_bar_tooltips();
}

bool MainWindow::has_unsaved_changes_in(const std::filesystem::path& file) const {
    for (const auto& [uri, document] : documents_) {
        const auto path = uri.file_path();
        if (!path || !document->is_dirty()) {
            continue;
        }
        std::error_code error;
        if (std::filesystem::equivalent(*path, file, error) ||
            path->lexically_normal() == file.lexically_normal()) {
            return true;
        }
    }
    return false;
}

bool MainWindow::has_unsaved_changes_within(const std::filesystem::path& path) const {
    return std::ranges::any_of(documents_, [&](const auto& entry) {
        const auto file = entry.first.file_path();
        return file && entry.second->is_dirty() && exthost::is_path_within(*file, path);
    });
}

SettingsStore::ChangeResult MainWindow::change_setting(const std::string& key, nlohmann::json value,
                                                       const QString& description) {
    const auto result = settings_->change(key, std::move(value));
    const QString name = description.isEmpty() ? QString::fromStdString(key) : description;
    const QString reason = QString::fromStdString(result.reason);
    switch (result.outcome) {
    case SettingsStore::ChangeResult::Outcome::written:
        break;
    case SettingsStore::ChangeResult::Outcome::session:
        status_bar_->show_message(tr("%1 is changed for this session only: %2").arg(name, reason),
                                  15000);
        break;
    case SettingsStore::ChangeResult::Outcome::rejected:
        status_bar_->show_message(tr("%1 was not changed: %2").arg(name, reason), 15000);
        break;
    }
    return result;
}

void MainWindow::apply_settings() {
    const core::EditorSettings& editor = settings_->editor();
    zoom_.set_level(editor.zoom_level);
    area_->apply_settings(editor);
    area_->refresh_wrapping();
    if (title_bar_) {
        title_bar_->set_command_center_visible(editor.command_center);
        title_bar_->set_layout_controls_visible(editor.layout_controls);
    }
    layout_.set_focus_recent_after_close(editor.focus_recent_editor_after_close);
    workbench_->set_sidebar_side(
        editor.side_bar_location == core::SideBarLocation::right ? Side::right : Side::left);
    apply_part_visibility();
    const bool auto_save_changed =
        editor.auto_save != auto_save_ || editor.auto_save_delay != auto_save_delay_;
    auto_save_ = editor.auto_save;
    auto_save_delay_ = editor.auto_save_delay;
    if (auto_save_changed) {
        cancel_scheduled_auto_saves();
        if (auto_save_ != core::AutoSave::off) {
            auto_save_all();
        }
    }
    themes_->select(editor.color_theme, platform::editor_defaults().color_theme);
    apply_file_associations();
    apply_file_excludes();
    apply_search_settings();
    apply_explorer_settings();
    apply_extension_host_options();
    // The label format says whether untitled documents go by their titles.
    for (const auto& [uri, title] : untitled_titles_) {
        update_document(uri);
    }
}

void MainWindow::rebuild_languages() {
    core::NullLogger discarded;
    core::Logger& logger = options_.logger ? *options_.logger : discarded;
    languages_ = core::LanguageRegistry::with_builtin_languages();
    for (const exthost::ContributedLanguage& contributed : bridge_.contributions().languages) {
        if (const auto problem = languages_.add(contributed.language)) {
            logger.log(core::LogLevel::warning, "extensions",
                       contributed.extension + ": " + *problem);
        }
    }
    languages_.set_user_associations(file_associations_);
    refresh_document_languages();
    rebuild_language_comments();
}

void MainWindow::rebuild_language_comments() {
    core::NullLogger discarded;
    core::Logger& logger = options_.logger ? *options_.logger : discarded;
    language_comments_.clear();
    for (const std::string& id : languages_.language_ids()) {
        if (const core::LanguageComments* builtin = core::builtin_language_comments(id)) {
            language_comments_.emplace(id, *builtin);
        }
    }
    const auto report = [&](const std::string& extension, const std::filesystem::path& file,
                            const std::string& problem) {
        const std::string message = extension + ": " + file.string() + ": " + problem;
        if (reported_language_configurations_.insert(message).second) {
            logger.log(core::LogLevel::warning, "extensions", message);
        }
    };
    for (const exthost::ContributedLanguage& contributed : bridge_.contributions().languages) {
        if (!contributed.configuration) {
            continue;
        }
        const auto text = platform::read_file(*contributed.configuration);
        if (!text) {
            report(contributed.extension, *contributed.configuration, text.error().message);
            continue;
        }
        const core::ReadComments read = core::read_language_comments(*text);
        for (const std::string& problem : read.problems) {
            report(contributed.extension, *contributed.configuration, problem);
        }
        if (!read.comments.empty()) {
            core::LanguageComments& tokens = language_comments_[contributed.language.id];
            tokens = tokens.overlaid_with(read.comments);
        }
    }
}

std::optional<core::LanguageComments>
MainWindow::language_comments(std::string_view language_id) const {
    const auto found = language_comments_.find(language_id);
    if (found == language_comments_.end()) {
        return std::nullopt;
    }
    return found->second;
}

void MainWindow::apply_file_associations() {
    core::FileAssociations read = core::read_file_associations(settings_->settings());
    if (read.associations == file_associations_ && read.problems == file_association_problems_) {
        return;
    }
    core::NullLogger discarded;
    core::Logger& logger = options_.logger ? *options_.logger : discarded;
    for (const std::string& problem : read.problems) {
        logger.log(core::LogLevel::warning, "settings", problem);
    }
    const bool associations_changed = read.associations != file_associations_;
    file_associations_ = std::move(read.associations);
    file_association_problems_ = std::move(read.problems);
    if (associations_changed) {
        languages_.set_user_associations(file_associations_);
        refresh_document_languages();
    }
}

void MainWindow::apply_search_settings() {
    core::FileSearchOptions read = core::FileSearchOptions::from_settings(settings_->settings());
    if (read.problems != search_options_.problems && options_.logger) {
        for (const std::string& problem : read.problems) {
            // Those of `files.exclude` are logged with the explorer's.
            if (std::ranges::find(file_excludes_.problems, problem) ==
                file_excludes_.problems.end()) {
                options_.logger->log(core::LogLevel::warning, "settings", problem);
            }
        }
    }
    search_options_ = std::move(read);
}

void MainWindow::remember_shown(const core::Uri& uri) {
    constexpr std::size_t remembered = 200;
    std::erase(recent_documents_, uri);
    recent_documents_.insert(recent_documents_.begin(), uri);
    if (recent_documents_.size() > remembered) {
        recent_documents_.erase(recent_documents_.begin() + remembered, recent_documents_.end());
    }
}

QuickOpenEntry MainWindow::quick_open_entry(const core::Uri& uri) {
    QuickOpenEntry entry{.uri = uri};
    const auto path = uri.file_path();
    if (!path) {
        entry.label = document_name(uri).toStdString();
        entry.path = entry.label;
        if (content_title(uri)) {
            entry.description = display_name(uri).toStdString();
        }
        return entry;
    }
    entry.label = path->filename().string();
    if (options_.workspace && exthost::is_path_within(*path, options_.workspace->folder)) {
        const std::filesystem::path relative = path->lexically_relative(options_.workspace->folder);
        entry.path = relative.generic_string();
        entry.description = relative.parent_path().generic_string();
    } else {
        entry.path = path->string();
        entry.description = path->parent_path().string();
    }
    return entry;
}

std::vector<QuickOpenEntry> MainWindow::recent_entries(bool open_only) {
    std::vector<QuickOpenEntry> entries;
    std::set<core::Uri> listed;
    for (const core::Uri& uri : recent_documents_) {
        bool offered = documents_.contains(uri);
        if (!open_only) {
            // A file that was closed is offered while it is there; an untitled document is not a
            // file.
            std::error_code error;
            const auto path = uri.file_path();
            offered = path && (offered || std::filesystem::exists(*path, error));
        }
        if (offered && listed.insert(uri).second) {
            entries.push_back(quick_open_entry(uri));
        }
    }
    if (open_only) {
        for (const auto& [uri, document] : documents_) {
            if (listed.insert(uri).second) {
                entries.push_back(quick_open_entry(uri));
            }
        }
    }
    return entries;
}

void MainWindow::dismiss_pick() {
    finish_go_to_line(false);
    pick_action_ = nullptr;
    pick_highlighted_ = nullptr;
    if (const auto dismissed = std::exchange(pick_dismissed_, nullptr);
        dismissed && palette_->isVisible()) {
        dismissed();
    }
}

void MainWindow::follow_quick_access_text(const QString& text) {
    // A popup that is being opened gets its text before it shows.
    if (!palette_->isVisible() || quick_access_ == QuickAccess::other) {
        return;
    }
    const QuickAccess wanted = text.startsWith(QLatin1Char('>'))   ? QuickAccess::commands
                               : text.startsWith(QLatin1Char(':')) ? QuickAccess::line
                                                                   : QuickAccess::files;
    if (wanted == quick_access_) {
        if (wanted == QuickAccess::line) {
            preview_go_to_line();
        }
    } else if (wanted == QuickAccess::commands) {
        show_command_palette(text);
    } else if (wanted == QuickAccess::line) {
        show_go_to_line(text);
    } else {
        show_files(text);
    }
}

void MainWindow::show_quick_open() {
    if (quick_open_->is_open() && palette_->isVisible()) {
        palette_->select_next();
        return;
    }
    show_files({});
}

void MainWindow::show_files(const QString& filter) {
    if (QApplication::activeModalWidget()) {
        return;
    }
    dismiss_pick();
    quick_access_ = QuickAccess::files;
    std::optional<QuickOpen::Files> files;
    if (options_.workspace) {
        const std::filesystem::path& folder = options_.workspace->folder;
        if (options_.list_files) {
            files = QuickOpen::Files{folder, options_.list_files(folder, search_options_)};
        } else if (options_.ripgrep) {
            files = QuickOpen::Files{
                folder, ripgrep_file_source(*options_.ripgrep, folder, search_options_)};
        } else {
            status_bar_->show_message(tr("Go to File lists only the open documents: ripgrep, which "
                                         "lists the files of the folder, was not found. ANVIL_RG "
                                         "names one."),
                                      15000);
        }
    }
    palette_->set_top_offset(has_custom_title_bar() ? title_bar_->height() : menu_bar_->height());
    std::vector<QuickOpenEntry> entries = recent_entries(!files);
    quick_open_->open(std::move(entries), std::move(files), filter);
}

void MainWindow::open_chosen(const core::Uri& uri) {
    if (const auto found = documents_.find(uri); found != documents_.end()) {
        show_document(uri, found->second);
    } else if (const auto path = uri.file_path()) {
        open_file(QString::fromStdString(path->string()));
    }
}

void MainWindow::apply_file_excludes() {
    core::FileExcludes read = core::FileExcludes::from_settings(settings_->settings());
    if (read.patterns == file_excludes_.patterns && read.problems == file_excludes_.problems) {
        return;
    }
    core::NullLogger discarded;
    core::Logger& logger = options_.logger ? *options_.logger : discarded;
    if (read.problems != file_excludes_.problems) {
        for (const std::string& problem : read.problems) {
            logger.log(core::LogLevel::warning, "settings", problem);
        }
    }
    const bool patterns_changed = read.patterns != file_excludes_.patterns;
    file_excludes_ = std::move(read);
    if (patterns_changed && explorer_) {
        explorer_->set_excludes(file_excludes_);
    }
}

std::string MainWindow::resolve_language(const core::Uri& uri, std::string_view first_line) const {
    const auto file = uri.file_path();
    return languages_.language_for(file ? file->generic_string() : uri.path, first_line);
}

std::string MainWindow::language_for_icon(const core::Uri& uri) {
    if (documents_.contains(uri)) {
        if (const std::string& id = language_of_document(uri).id;
            id != core::LanguageRegistry::plaintext_id) {
            return id;
        }
    }
    return resolve_language(uri, {});
}

SetiIcon MainWindow::file_icon_of(const core::Uri& uri) {
    const auto path = uri.file_path();
    const std::string name = path ? path->filename().string() : uri.path;
    if (const std::optional<SetiIcon> icon = seti_icon_by_name(name)) {
        return *icon;
    }
    return seti_file_icon(name, language_for_icon(uri));
}

void MainWindow::refresh_file_icons() {
    if (explorer_) {
        explorer_->refresh_icons();
    }
}

MainWindow::DocumentLanguage MainWindow::describe_language(const std::string& id) const {
    const core::Language* language = languages_.language(id);
    const std::string& name = language && language->name ? *language->name : id;
    return {id, QString::fromStdString(name)};
}

const MainWindow::DocumentLanguage& MainWindow::language_of_document(const core::Uri& uri) {
    if (const auto found = document_languages_.find(uri); found != document_languages_.end()) {
        return found->second;
    }
    std::string id;
    if (const auto known = bridge_.language_of(uri)) {
        id = *known;
    } else {
        id = resolve_language(uri, documents_.at(uri)->text().line(0));
    }
    return document_languages_.emplace(uri, describe_language(id)).first->second;
}

void MainWindow::refresh_document_languages() {
    document_languages_.clear();
    for (const auto& [uri, document] : documents_) {
        if (const auto chosen = chosen_languages_.find(uri); chosen != chosen_languages_.end()) {
            document_languages_.emplace(uri, describe_language(chosen->second));
            continue;
        }
        if (bridge_.language_is_fixed(uri)) {
            continue;
        }
        const std::string id = resolve_language(uri, document->text().line(0));
        bridge_.language_changed(uri, id);
        document_languages_.emplace(uri, describe_language(id));
    }
    for (const auto& [uri, document] : documents_) {
        update_document(uri);
    }
    area_->refresh_highlighters();
    update_status();
    refresh_file_icons();
}

std::shared_ptr<const core::Highlighter> MainWindow::highlighter_of(const core::Uri& uri) {
    const auto found = documents_.find(uri);
    if (found == documents_.end()) {
        return nullptr;
    }
    return highlighting_->highlighter_for(uri, found->second, language_of_document(uri).id);
}

void MainWindow::document_edited(const core::Uri& uri, const core::DocumentChange& change) {
    const auto found = documents_.find(uri);
    if (found == documents_.end()) {
        return;
    }
    if (highlighting_->edited(uri, *found->second, change)) {
        area_->refresh_highlighters();
    }
    // Only when the title changes, which a keystroke past the first line never does.
    if (retitle(uri, *found->second)) {
        update_document(uri);
    }
}

bool MainWindow::retitle(const core::Uri& uri, const core::Document& document) {
    if (uri.scheme != "untitled") {
        return false;
    }
    const std::optional<std::string> title = core::untitled_title(document.text());
    const auto known = untitled_titles_.find(uri);
    if (!title) {
        return untitled_titles_.erase(uri) > 0;
    }
    if (known != untitled_titles_.end() && known->second == *title) {
        return false;
    }
    untitled_titles_.insert_or_assign(uri, *title);
    return true;
}

std::optional<QString> MainWindow::content_title(const core::Uri& uri) const {
    const auto found = untitled_titles_.find(uri);
    if (found == untitled_titles_.end() ||
        settings_->editor().untitled_label != core::UntitledLabel::content) {
        return std::nullopt;
    }
    return QString::fromStdString(found->second);
}

QString MainWindow::document_name(const core::Uri& uri) const {
    return content_title(uri).value_or(display_name(uri));
}

void MainWindow::add_untitled_tab(workbench::GroupId group) {
    add_untitled_document(group);
    const workbench::EditorGroup* state = layout_.group(group);
    if (state && state->active) {
        layout_.move_tab(group, *state->active, group, state->tabs.size() - 1);
    }
}

void MainWindow::forget_document_language(const core::Uri& uri) {
    highlighting_->forget(uri);
    document_languages_.erase(uri);
    chosen_languages_.erase(uri);
}

void MainWindow::pick_from(const QString& placeholder, std::vector<QuickPickItem> items,
                           std::function<void(std::size_t)> chosen, PickHooks hooks) {
    if (QApplication::activeModalWidget()) {
        return;
    }
    quick_open_->end();
    dismiss_pick();
    quick_access_ = QuickAccess::other;
    pick_action_ = std::move(chosen);
    pick_dismissed_ = std::move(hooks.dismissed);
    palette_->set_filtering(true);
    palette_->set_ignored_prefix({});
    palette_->set_placeholder(placeholder);
    palette_->set_empty_message(tr("No matching results"));
    palette_->set_items(std::move(items));
    palette_->set_top_offset(has_custom_title_bar() ? title_bar_->height() : menu_bar_->height());
    palette_->open();
    if (hooks.active) {
        palette_->set_active_item(*hooks.active);
    }
    pick_highlighted_ = std::move(hooks.highlighted);
}

void MainWindow::select_color_theme() {
    const auto groups = themes_->picker_groups();
    if (groups.empty()) {
        status_bar_->show_message(tr("There are no color themes to choose from"), 5000);
        return;
    }
    const std::string previous = themes_->current();
    std::vector<QuickPickItem> items;
    auto ids = std::make_shared<std::vector<std::string>>();
    std::optional<std::size_t> active;
    for (const auto& group : groups) {
        QString heading;
        switch (group.front()->kind) {
        case core::ThemeKind::dark:
            heading = tr("dark themes");
            break;
        case core::ThemeKind::light:
            heading = tr("light themes");
            break;
        default:
            heading = tr("high contrast themes");
            break;
        }
        items.push_back({heading, {}, {}, {}, false, true});
        ids->emplace_back();
        for (const ThemeStore::Entry* entry : group) {
            const std::string& name = entry->id.empty() ? entry->label : entry->id;
            const QString label = QString::fromStdString(entry->label);
            const QString id = QString::fromStdString(name);
            if (name == previous) {
                active = items.size();
            }
            items.push_back({label, label == id ? QString() : id, {}, {}, !entry->available});
            ids->push_back(name);
        }
    }
    const auto usable = [this, ids](std::size_t item) {
        if (item >= ids->size() || ids->at(item).empty()) {
            return false;
        }
        const auto& all = themes_->themes();
        const auto found = std::ranges::find_if(all, [&](const ThemeStore::Entry& entry) {
            return (entry.id.empty() ? entry.label : entry.id) == ids->at(item);
        });
        return found != all.end() && found->available;
    };
    pick_from(tr("Select Color Theme (Up/Down Keys to Preview)"), std::move(items),
              [this, ids, previous, usable](std::size_t item) {
                  if (!usable(item)) {
                      return;
                  }
                  const auto result =
                      change_setting("workbench.colorTheme", ids->at(item), tr("Color Theme"));
                  if (result.outcome == SettingsStore::ChangeResult::Outcome::rejected) {
                      themes_->select(previous);
                  }
              },
              {.active = active,
               .highlighted =
                   [this, ids, usable](std::size_t item) {
                       if (usable(item)) {
                           themes_->select(ids->at(item));
                       }
                   },
               .dismissed = [this, previous] { themes_->select(previous); }});
}

std::vector<QuickPickItem> MainWindow::language_picks(const std::string& current,
                                                      std::vector<std::string>& ids) const {
    struct Named {
        std::string id;
        QString name;
    };
    std::vector<Named> languages;
    for (const std::string& id : languages_.language_ids()) {
        if (const core::Language* language = languages_.language(id); language && language->name) {
            languages.push_back({id, QString::fromStdString(*language->name)});
        }
    }
    std::ranges::stable_sort(languages, [](const Named& a, const Named& b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    std::vector<QuickPickItem> items;
    ids.clear();
    for (Named& language : languages) {
        const QString id = QString::fromStdString(language.id);
        items.push_back({language.name,
                         language.id == current ? tr("(%1) - Configured Language").arg(id)
                                                : QString("(%1)").arg(id),
                         {},
                         {},
                         false});
        items.back().icon = quick_pick_icon(seti_icon(language.id));
        ids.push_back(std::move(language.id));
    }
    return items;
}

void MainWindow::choose_language(const core::Uri& uri, const std::string& id) {
    if (!documents_.contains(uri)) {
        return;
    }
    chosen_languages_[uri] = id;
    document_languages_.insert_or_assign(uri, describe_language(id));
    bridge_.language_changed(uri, id);
    area_->refresh_highlighters();
    update_document(uri);
    refresh_file_icons();
    update_status();
}

void MainWindow::auto_detect_language(const core::Uri& uri) {
    const auto document = documents_.find(uri);
    if (document == documents_.end()) {
        return;
    }
    chosen_languages_.erase(uri);
    const std::string id = resolve_language(uri, document->second->text().line(0));
    document_languages_.insert_or_assign(uri, describe_language(id));
    bridge_.language_changed(uri, id);
    area_->refresh_highlighters();
    update_document(uri);
    refresh_file_icons();
    update_status();
}

void MainWindow::change_language_mode() {
    const core::Uri* shown = current_uri_ptr();
    if (!shown) {
        return;
    }
    // The pick is not modal: the document can be closed or renamed before the user chooses.
    const core::Uri uri = *shown;
    std::vector<QuickPickItem> items;
    auto actions = std::make_shared<std::vector<std::function<void()>>>();
    const auto add = [&](QuickPickItem item, std::function<void()> action) {
        items.push_back(std::move(item));
        actions->push_back(std::move(action));
    };

    if (documents_.at(uri)->text().size() > 0) {
        add({tr("Auto Detect"), {}, {}, {}, false}, [this, uri] {
            auto_detect_language(uri);
            focus_current_editor();
        });
    }
    const auto file = uri.file_path();
    const std::string extension = file ? file->extension().string() : std::string();
    const std::string base = file ? file->filename().string() : std::string();
    if (!extension.empty()) {
        add({tr("Configure File Association for '%1'…").arg(QString::fromStdString(extension)),
             {},
             {},
             {},
             false},
            [this, extension, base] {
                // The quick pick is still closing, and opening it again at once loses the focus.
                QTimer::singleShot(0, this,
                                   [this, extension, base] { pick_association(extension, base); });
            });
    }
    add({tr("languages (identifier)"), {}, {}, {}, false, true}, nullptr);
    std::vector<std::string> ids;
    std::vector<QuickPickItem> languages = language_picks(language_of_document(uri).id, ids);
    for (std::size_t i = 0; i < languages.size(); ++i) {
        add(std::move(languages[i]), [this, uri, id = std::move(ids[i])] {
            choose_language(uri, id);
            focus_current_editor();
        });
    }
    pick_from(tr("Select Language Mode"), std::move(items), [actions](std::size_t item) {
        if (item < actions->size() && (*actions)[item]) {
            (*actions)[item]();
        }
    });
}

void MainWindow::pick_association(const std::string& extension, const std::string& base) {
    std::vector<std::string> ids;
    std::vector<QuickPickItem> picks = language_picks({}, ids);
    const std::string key = base.starts_with('.') ? base : "*" + extension;
    pick_from(
        tr("Select Language Mode to Associate with '%1'").arg(QString::fromStdString(extension)),
        std::move(picks), [this, key, ids = std::move(ids)](std::size_t pick) {
            if (pick >= ids.size()) {
                return;
            }
            const nlohmann::json* known = settings_->settings().find("files.associations");
            nlohmann::json associations =
                known && known->is_object() ? *known : nlohmann::json::object();
            associations[key] = ids[pick];
            change_setting("files.associations", std::move(associations), tr("File Associations"));
        });
}

void MainWindow::apply_extension_host_options() {
    const core::EditorSettings& editor = settings_->editor();
    bridge_.set_editor_options({.tab_size = static_cast<std::size_t>(editor.tab_size),
                                .insert_spaces = editor.insert_spaces});
    bridge_.set_overwrite_changed_files(editor.save_conflict_resolution ==
                                        core::SaveConflictResolution::overwrite_file_on_disk);
}

void MainWindow::register_commands() {
    const auto add = [this](std::string id, const QString& title, std::function<void()> action) {
        commands_.add(std::move(id), title.toStdString(),
                      [action = std::move(action)](const nlohmann::json&) { action(); });
    };
    using core::Motion;

    add("undo", tr("Undo"), [this] { view()->undo(); });
    add("redo", tr("Redo"), [this] { view()->redo(); });
    add("editor.action.copyLinesUpAction", tr("Copy Line Up"),
        [this] { view()->copy_lines(false); });
    add("editor.action.copyLinesDownAction", tr("Copy Line Down"),
        [this] { view()->copy_lines(true); });
    add("editor.action.moveLinesUpAction", tr("Move Line Up"),
        [this] { view()->move_lines(false); });
    add("editor.action.moveLinesDownAction", tr("Move Line Down"),
        [this] { view()->move_lines(true); });
    add("editor.action.duplicateSelection", tr("Duplicate Selection"),
        [this] { view()->duplicate_selection(); });
    add("cursorLeft", tr("Move Cursor Left"), [this] { view()->move(Motion::left); });
    add("cursorRight", tr("Move Cursor Right"), [this] { view()->move(Motion::right); });
    add("cursorUp", tr("Move Cursor Up"), [this] { view()->move(Motion::up); });
    add("cursorDown", tr("Move Cursor Down"), [this] { view()->move(Motion::down); });
    add("cursorHome", tr("Move Cursor to Line Start"), [this] { view()->move(Motion::home); });
    add("cursorEnd", tr("Move Cursor to Line End"), [this] { view()->move(Motion::line_end); });
    add("deleteLeft", tr("Delete Left"), [this] { view()->delete_backward(); });
    add("deleteRight", tr("Delete Right"), [this] { view()->delete_forward(); });
    add("deleteWordLeft", tr("Delete Word Left"), [this] { view()->delete_word_left(); });
    add("deleteWordRight", tr("Delete Word Right"), [this] { view()->delete_word_right(); });
    add("tab", tr("Tab"), [this] { view()->tab(); });
    add("outdent", tr("Outdent"), [this] { view()->outdent_lines(); });
    add("editor.action.indentLines", tr("Indent Line"), [this] { view()->indent_lines(); });
    add("editor.action.outdentLines", tr("Outdent Line"), [this] { view()->outdent_lines(); });
    add("editor.action.selectAll", tr("Select All"), [this] { view()->select_all(); });
    add("editor.action.clipboardCutAction", tr("Cut"), [this] { view()->cut(); });
    add("editor.action.clipboardCopyAction", tr("Copy"), [this] { view()->copy(); });
    add("editor.action.clipboardPasteAction", tr("Paste"), [this] { view()->paste(); });
    add("editor.action.addSelectionToNextFindMatch", tr("Add Selection to Next Find Match"),
        [this] { view()->add_next_occurrence(occurrence_source()); });
    add("editor.action.addSelectionToPreviousFindMatch", tr("Add Selection to Previous Find Match"),
        [this] { view()->add_previous_occurrence(occurrence_source()); });
    add("editor.action.insertCursorAbove", tr("Add Cursor Above"),
        [this] { view()->add_cursors_vertically(false); });
    add("editor.action.insertCursorBelow", tr("Add Cursor Below"),
        [this] { view()->add_cursors_vertically(true); });
    add("editor.action.insertCursorAtEndOfEachLineSelected", tr("Add Cursors to Line Ends"),
        [this] { view()->add_cursors_to_line_ends(); });
    add("editor.action.selectHighlights", tr("Select All Occurrences of Find Match"), [this] {
        report_truncated_selection(
            view()->select_all_occurrences(occurrence_source(), FindBar::match_limit));
    });
    add("editor.action.selectAllMatches", tr("Select All Matches of the Find Bar"), [this] {
        FindBar* bar = find_bar();
        if (!bar->isVisible() || !bar->search()) {
            return;
        }
        report_truncated_selection(
            view()->select_all_matches(*bar->search(), FindBar::match_limit));
        view()->setFocus();
    });
    add("actions.find", tr("Find"), [this] { open_find_bar(false); });
    add("editor.action.startFindReplaceAction", tr("Replace"), [this] { open_find_bar(true); });
    add("editor.action.nextMatchFindAction", tr("Find Next"), [this] { find_bar()->find_next(); });
    add("editor.action.previousMatchFindAction", tr("Find Previous"),
        [this] { find_bar()->find_previous(); });
    add("workbench.action.files.openFile", tr("Open File…"), [this] { open_dialog(); });
    add("workbench.action.newWindow", tr("New Window"), [this] { open_new_window(); });
    add("workbench.action.zoomIn", tr("Zoom In"), [this] { zoom_by(1); });
    add("workbench.action.zoomOut", tr("Zoom Out"), [this] { zoom_by(-1); });
    add("workbench.action.zoomReset", tr("Reset Zoom"),
        [this] { change_setting("window.zoomLevel", 0, tr("Zoom Level")); });
    add("workbench.action.showAboutDialog", tr("About"), [this] { show_about(); });
    add("workbench.action.openDocumentationUrl", tr("Documentation"),
        [this] { open_project_page(documentation_url); });
    add("workbench.action.openIssueReporter", tr("Report Issue"),
        [this] { open_project_page(new_issue_url); });
    add("workbench.action.openLicenseUrl", tr("View License"),
        [this] { open_project_page(license_url); });
    // There is no settings or shortcuts editor: both the interface and the JSON commands open the
    // file.
    const auto open_settings = [this] { open_user_file(options_.settings_file, "{}\n"); };
    const auto open_keybindings = [this] { open_user_file(options_.keybindings_file, "[]\n"); };
    add("workbench.action.openSettings", tr("Open Settings"), open_settings);
    add("workbench.action.openSettingsJson", tr("Open User Settings (JSON)"), open_settings);
    add("workbench.action.openGlobalKeybindings", tr("Open Keyboard Shortcuts"), open_keybindings);
    add("workbench.action.openGlobalKeybindingsFile", tr("Open Keyboard Shortcuts (JSON)"),
        open_keybindings);
    add("workbench.action.files.openFolder", tr("Open Folder…"),
        [this] { QTimer::singleShot(0, this, [this] { open_folder_dialog(); }); });
    add("workbench.action.closeFolder", tr("Close Folder"),
        [this] { QTimer::singleShot(0, this, [this] { close_folder(); }); });
    add("workbench.view.explorer", tr("View: Show Explorer"), [this] {
        // As VS Code does for a view container: a second use goes back to the editor.
        const QWidget* focus = focusWidget();
        if (explorer_ && explorer_->isVisible() && focus && explorer_->isAncestorOf(focus)) {
            focus_current_editor();
            return;
        }
        if (ExplorerView* explorer = show_explorer()) {
            QWidget* target = options_.workspace
                                  ? static_cast<QWidget*>(explorer->tree())
                                  : static_cast<QWidget*>(explorer->open_folder_button());
            target->setFocus(Qt::ShortcutFocusReason);
        }
    });
    add("explorer.newFile", tr("New File..."), [this] {
        if (ExplorerView* explorer = show_explorer()) {
            explorer->new_file();
        }
    });
    add("explorer.newFolder", tr("New Folder..."), [this] {
        if (ExplorerView* explorer = show_explorer()) {
            explorer->new_folder();
        }
    });
    add("renameFile", tr("Rename..."), [this] {
        if (explorer_) {
            explorer_->rename_selected();
        }
    });
    add("moveFileToTrash", tr("Delete"), [this] {
        if (explorer_) {
            explorer_->trash_selected();
        }
    });
    add("deleteFile", tr("Delete Permanently"), [this] {
        if (explorer_) {
            explorer_->delete_selected();
        }
    });
    add("revealFileInOS", reveal_title(), [this] { reveal_in_file_manager(); });
    add("workbench.action.files.save", tr("Save"), [this] { save(); });
    add("saveAll", tr("Save All"), [this] { save_all(); });
    add("workbench.action.files.revert", tr("Revert File"), [this] { revert(); });
    add("workbench.action.files.saveAs", tr("Save As…"), [this] { save_as_dialog(); });
    add("workbench.action.editor.changeLanguageMode", tr("Change Language Mode"),
        [this] { change_language_mode(); });
    add("workbench.action.closeActiveEditor", tr("Close Editor"), [this] {
        const workbench::EditorGroup* group = layout_.group(layout_.focused());
        if (group->active) {
            close_tab_at(group->id, *group->active);
        }
    });
    add("workbench.action.keybindingsReference", tr("Keyboard Shortcuts Reference"),
        [this] { show_keybindings_reference(); });
    add("workbench.action.files.newUntitledFile", tr("New Untitled Text File"),
        [this] { add_untitled_document(); });
    add("workbench.action.quit", tr("Quit"), [this] { close(); });
    add("workbench.action.closeWindow", tr("Close Window"), [this] { close(); });
    add("workbench.action.toggleFullScreen", tr("Toggle Full Screen"),
        [this] { setWindowState(windowState() ^ Qt::WindowFullScreen); });
    add("workbench.action.toggleZenMode", tr("Toggle Zen Mode"), [this] { toggle_zen_mode(); });
    add("workbench.action.exitZenMode", tr("Exit Zen Mode"), [this] {
        if (zen_.active) {
            toggle_zen_mode();
        }
    });
    const auto add_tabs_mode = [this, &add](const char* id, const QString& title, const char* key,
                                            const char* value) {
        add(id, title, [this, key, value] { change_setting(key, value, tr("Tab Bar")); });
    };
    add_tabs_mode("workbench.action.showMultipleEditorTabs", tr("Show Multiple Editor Tabs"),
                  "workbench.editor.showTabs", "multiple");
    add_tabs_mode("workbench.action.showEditorTab", tr("Show Single Editor Tab"),
                  "workbench.editor.showTabs", "single");
    add_tabs_mode("workbench.action.hideEditorTabs", tr("Hide Editor Tabs"),
                  "workbench.editor.showTabs", "none");
    add_tabs_mode("workbench.action.zenShowMultipleEditorTabs",
                  tr("Show Multiple Editor Tabs in Zen Mode"), "zenMode.showTabs", "multiple");
    add_tabs_mode("workbench.action.zenShowEditorTab", tr("Show Single Editor Tab in Zen Mode"),
                  "zenMode.showTabs", "single");
    add_tabs_mode("workbench.action.zenHideEditorTabs", tr("Hide Editor Tabs in Zen Mode"),
                  "zenMode.showTabs", "none");
    add("workbench.action.toggleCenteredLayout", tr("Toggle Centered Layout"), [this] {
        area_->set_centered(!area_->centered());
        view()->setFocus();
    });
    commands_.add(
        "setContext", tr("Set Context").toStdString(), [this](const nlohmann::json& args) {
            if (args.is_array() && args.size() >= 1 && args[0].is_string()) {
                keys_->set_extension_context(args[0].get<std::string>(),
                                             args.size() >= 2 ? args[1] : nlohmann::json());
            }
        });
    add("workbench.action.splitEditor", tr("Split Editor"),
        [this] { layout_.split(layout_.focused(), workbench::Direction::right); });
    add("workbench.action.splitEditorRight", tr("Split Editor Right"),
        [this] { layout_.split(layout_.focused(), workbench::Direction::right); });
    add("workbench.action.splitEditorDown", tr("Split Editor Down"),
        [this] { layout_.split(layout_.focused(), workbench::Direction::down); });
    add("workbench.action.splitEditorLeft", tr("Split Editor Left"),
        [this] { layout_.split(layout_.focused(), workbench::Direction::left); });
    add("workbench.action.splitEditorUp", tr("Split Editor Up"),
        [this] { layout_.split(layout_.focused(), workbench::Direction::up); });
    add("workbench.action.focusNextGroup", tr("Focus Next Editor Group"),
        [this] { layout_.focus_next(); });
    add("workbench.action.focusPreviousGroup", tr("Focus Previous Editor Group"),
        [this] { layout_.focus_previous(); });
    const auto show_current = [this](bool changed) { show_current_after(changed); };
    add("workbench.action.nextEditor", tr("Open Next Editor"),
        [this, show_current] { show_current(layout_.next_editor(true)); });
    add("workbench.action.previousEditor", tr("Open Previous Editor"),
        [this, show_current] { show_current(layout_.next_editor(false)); });
    add("workbench.action.nextEditorInGroup", tr("Open Next Editor in Group"),
        [this, show_current] { show_current(layout_.next_editor_in_group(true)); });
    add("workbench.action.previousEditorInGroup", tr("Open Previous Editor in Group"),
        [this, show_current] { show_current(layout_.next_editor_in_group(false)); });
    add("workbench.action.openNextRecentlyUsedEditor", tr("Open Next Recently Used Editor"),
        [this, show_current] { show_current(layout_.navigate_recent(false, false)); });
    add("workbench.action.openPreviousRecentlyUsedEditor", tr("Open Previously Used Editor"),
        [this, show_current] { show_current(layout_.navigate_recent(true, false)); });
    add("workbench.action.openNextRecentlyUsedEditorInGroup",
        tr("Open Next Recently Used Editor in Group"),
        [this, show_current] { show_current(layout_.navigate_recent(false, true)); });
    add("workbench.action.openPreviousRecentlyUsedEditorInGroup",
        tr("Open Previously Used Editor in Group"),
        [this, show_current] { show_current(layout_.navigate_recent(true, true)); });
    struct Ordinal {
        const char* id;
        const char* title;
    };
    static const Ordinal ordinals[] = {
        {"workbench.action.focusFirstEditorGroup", QT_TR_NOOP("Focus First Editor Group")},
        {"workbench.action.focusSecondEditorGroup", QT_TR_NOOP("Focus Second Editor Group")},
        {"workbench.action.focusThirdEditorGroup", QT_TR_NOOP("Focus Third Editor Group")},
        {"workbench.action.focusFourthEditorGroup", QT_TR_NOOP("Focus Fourth Editor Group")},
        {"workbench.action.focusFifthEditorGroup", QT_TR_NOOP("Focus Fifth Editor Group")},
        {"workbench.action.focusSixthEditorGroup", QT_TR_NOOP("Focus Sixth Editor Group")},
        {"workbench.action.focusSeventhEditorGroup", QT_TR_NOOP("Focus Seventh Editor Group")},
        {"workbench.action.focusEighthEditorGroup", QT_TR_NOOP("Focus Eighth Editor Group")},
    };
    for (std::size_t index = 0; index < std::size(ordinals); ++index) {
        add(ordinals[index].id, tr(ordinals[index].title),
            [this, index] { focus_group_number(index); });
    }
    const auto add_direction = [&](const char* id, const QString& title,
                                   workbench::Direction direction) {
        add(id, title,
            [this, direction] { show_current_after(layout_.focus_neighbor(direction)); });
    };
    add_direction("workbench.action.focusLeftGroup", tr("Focus Left Editor Group"),
                  workbench::Direction::left);
    add_direction("workbench.action.focusRightGroup", tr("Focus Right Editor Group"),
                  workbench::Direction::right);
    add_direction("workbench.action.focusAboveGroup", tr("Focus Editor Group Above"),
                  workbench::Direction::up);
    add_direction("workbench.action.focusBelowGroup", tr("Focus Editor Group Below"),
                  workbench::Direction::down);
    struct Arrangement {
        const char* id;
        const char* title;
        workbench::Orientation orientation;
        std::vector<workbench::LayoutShape> shapes;
    };
    using workbench::LayoutShape;
    using workbench::Orientation;
    const LayoutShape leaf;
    const LayoutShape pair{{leaf, leaf}};
    const std::vector<Arrangement> arrangements = {
        {"workbench.action.editorLayoutSingle",
         QT_TR_NOOP("Single Column Editor Layout"),
         Orientation::horizontal,
         {leaf}},
        {"workbench.action.editorLayoutTwoColumns",
         QT_TR_NOOP("Two Columns Editor Layout"),
         Orientation::horizontal,
         {leaf, leaf}},
        {"workbench.action.editorLayoutThreeColumns",
         QT_TR_NOOP("Three Columns Editor Layout"),
         Orientation::horizontal,
         {leaf, leaf, leaf}},
        {"workbench.action.editorLayoutTwoRows",
         QT_TR_NOOP("Two Rows Editor Layout"),
         Orientation::vertical,
         {leaf, leaf}},
        {"workbench.action.editorLayoutThreeRows",
         QT_TR_NOOP("Three Rows Editor Layout"),
         Orientation::vertical,
         {leaf, leaf, leaf}},
        {"workbench.action.editorLayoutTwoByTwoGrid",
         QT_TR_NOOP("Grid Editor Layout (2x2)"),
         Orientation::horizontal,
         {pair, pair}},
        {"workbench.action.editorLayoutTwoColumnsBottom",
         QT_TR_NOOP("Two Columns Bottom Editor Layout"),
         Orientation::vertical,
         {leaf, pair}},
        {"workbench.action.editorLayoutTwoRowsRight",
         QT_TR_NOOP("Two Rows Right Editor Layout"),
         Orientation::horizontal,
         {leaf, pair}},
    };
    for (const Arrangement& arrangement : arrangements) {
        add(arrangement.id, tr(arrangement.title), [this, arrangement] {
            layout_.apply_layout(arrangement.orientation, arrangement.shapes);
            focus_current_editor();
        });
    }
    add("workbench.action.toggleEditorGroupLayout", tr("Toggle Vertical/Horizontal Editor Layout"),
        [this] {
            if (layout_.flip()) {
                focus_current_editor();
            }
        });
    add("workbench.action.closeGroup", tr("Close Editor Group"),
        [this] { close_group(layout_.focused()); });
    const auto set_render_whitespace = [this](const char* value) {
        return [this, value] {
            change_setting("editor.renderWhitespace", value, tr("Render Whitespace"));
        };
    };
    add("editor.action.renderWhitespace.none", tr("Set Render Whitespace to None"),
        set_render_whitespace("none"));
    add("editor.action.renderWhitespace.boundary", tr("Set Render Whitespace to Boundary"),
        set_render_whitespace("boundary"));
    add("editor.action.renderWhitespace.selection", tr("Set Render Whitespace to Selection"),
        set_render_whitespace("selection"));
    add("editor.action.renderWhitespace.trailing", tr("Set Render Whitespace to Trailing"),
        set_render_whitespace("trailing"));
    add("editor.action.renderWhitespace.all", tr("Set Render Whitespace to All"),
        set_render_whitespace("all"));
    add("editor.action.toggleWordWrap", tr("View: Toggle Word Wrap"),
        [this] { toggle_word_wrap(); });
    add("editor.action.toggleRenderControlCharacter", tr("Toggle Control Characters"), [this] {
        change_setting("editor.renderControlCharacters",
                       !settings_->editor().render_control_characters,
                       tr("Render Control Characters"));
    });
    add("workbench.action.toggleMultiCursorModifier", tr("Toggle Multi-Cursor Modifier"), [this] {
        const bool ctrl_cmd =
            settings_->editor().multi_cursor_modifier == core::MultiCursorModifier::ctrl_cmd;
        change_setting("editor.multiCursorModifier", ctrl_cmd ? "alt" : "ctrlCmd",
                       tr("Multi-Cursor Modifier"));
    });
    add("editor.action.toggleColumnSelection", tr("Toggle Column Selection Mode"), [this] {
        change_setting("editor.columnSelection", !settings_->editor().column_selection,
                       tr("Column Selection Mode"));
    });
    add("workbench.action.toggleAutoSave", tr("Toggle Auto Save"), [this] {
        change_setting("files.autoSave", auto_save_ == core::AutoSave::off ? "afterDelay" : "off",
                       tr("Auto Save"));
    });
    add("workbench.action.navigateBack", tr("Go Back"),
        [this] { navigate(history_.go_back(), false); });
    add("workbench.action.navigateForward", tr("Go Forward"),
        [this] { navigate(history_.go_forward(), false); });
    add("workbench.action.navigateToLastEditLocation", tr("Go to Last Edit Location"),
        [this] { navigate(history_.go_to_last_edit(), true); });
    add("workbench.action.gotoLine", tr("Go to Line/Column..."), [this] { show_go_to_line(); });
    add("workbench.action.quickOpen", tr("Go to File..."), [this] { show_quick_open(); });
    add("editor.action.jumpToBracket", tr("Go to Bracket"), [this] { view()->jump_to_bracket(); });
    add("editor.action.commentLine", tr("Toggle Line Comment"),
        [this] { run_comment_command(false, core::LineCommentType::toggle); });
    add("editor.action.addCommentLine", tr("Add Line Comment"),
        [this] { run_comment_command(false, core::LineCommentType::add); });
    add("editor.action.removeCommentLine", tr("Remove Line Comment"),
        [this] { run_comment_command(false, core::LineCommentType::remove); });
    add("editor.action.blockComment", tr("Toggle Block Comment"),
        [this] { run_comment_command(true, core::LineCommentType::toggle); });
    add("editor.action.smartSelect.expand", tr("Expand Selection"),
        [this] { view()->expand_selection(); });
    add("editor.action.smartSelect.shrink", tr("Shrink Selection"),
        [this] { view()->shrink_selection(); });
    add("workbench.action.selectTheme", tr("Preferences: Color Theme"),
        [this] { select_color_theme(); });
    add("workbench.action.reopenClosedEditor", tr("Reopen Closed Editor"),
        [this] { reopen_closed_editor(); });
    add("workbench.action.showCommands", tr("Show All Commands"),
        [this] { show_command_palette(); });
    add("workbench.action.toggleSidebarVisibility", tr("Toggle Primary Side Bar Visibility"),
        [this] { workbench_->toggle(Part::sidebar); });
    add("workbench.action.togglePanel", tr("Toggle Panel Visibility"),
        [this] { workbench_->toggle(Part::panel); });
    add("workbench.action.toggleActivityBarVisibility", tr("Toggle Activity Bar Visibility"),
        [this] {
            const bool hidden =
                settings_->editor().activity_bar_location == core::ActivityBarLocation::hidden;
            change_setting("workbench.activityBar.location", hidden ? "default" : "hidden",
                           tr("Activity Bar Position"));
        });
    add("workbench.action.activityBarLocation.default", tr("Move Activity Bar to Side"), [this] {
        change_setting("workbench.activityBar.location", "default", tr("Activity Bar Position"));
    });
    add("workbench.action.activityBarLocation.hide", tr("Hide Activity Bar"), [this] {
        change_setting("workbench.activityBar.location", "hidden", tr("Activity Bar Position"));
    });
    add("workbench.action.toggleStatusbarVisibility", tr("Toggle Status Bar Visibility"), [this] {
        change_setting("workbench.statusBar.visible", !settings_->editor().status_bar_visible,
                       tr("Status Bar Visibility"));
    });
    add("workbench.action.toggleSidebarPosition", tr("Toggle Primary Side Bar Position"), [this] {
        const bool right = settings_->editor().side_bar_location == core::SideBarLocation::right;
        change_setting("workbench.sideBar.location", right ? "left" : "right",
                       tr("Primary Side Bar Position"));
    });
    const struct {
        const char* id;
        const char* title;
        PanelPosition position;
    } panel_positions[] = {
        {"workbench.action.positionPanelTop", QT_TR_NOOP("Move Panel To Top"), PanelPosition::top},
        {"workbench.action.positionPanelLeft", QT_TR_NOOP("Move Panel Left"), PanelPosition::left},
        {"workbench.action.positionPanelRight", QT_TR_NOOP("Move Panel Right"),
         PanelPosition::right},
        {"workbench.action.positionPanelBottom", QT_TR_NOOP("Move Panel To Bottom"),
         PanelPosition::bottom},
    };
    for (const auto& entry : panel_positions) {
        add(entry.id, tr(entry.title),
            [this, position = entry.position] { workbench_->set_panel_position(position); });
    }
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

namespace {

// Qt marks the mnemonic with a single `&` before it and writes a literal ampersand as `&&`.
QString menu_text(const workbench::MenuLabel& label) {
    const auto escaped = [](const std::string& text) {
        return QString::fromStdString(text).replace(QLatin1Char('&'), QStringLiteral("&&"));
    };
    if (label.mnemonic == workbench::MenuLabel::npos) {
        return escaped(label.text);
    }
    return escaped(label.text.substr(0, label.mnemonic)) + QLatin1Char('&') +
           escaped(label.text.substr(label.mnemonic));
}

} // namespace

// The system draws the global menu bar of macOS, and its menus with it.
void MainWindow::style_menu(QMenu* menu) {
#if !defined(Q_OS_MACOS)
    menu->setStyle(menu_style_.get());
    menu->setFont(menu_font());
#else
    Q_UNUSED(menu);
#endif
}

QFont MainWindow::menu_font() const {
    QFont font = QApplication::font("QMenu");
    if (zoom_.level() != 0) {
        font.setPixelSize(zoom_.scaled(QFontInfo(font).pixelSize()));
    }
    return font;
}

EditorArea::Wrapping
MainWindow::wrapping_of(const std::shared_ptr<core::Document>& document) const {
    const core::EditorSettings& settings = settings_->editor();
    const auto column = static_cast<std::size_t>(settings.word_wrap_column);
    if (const auto found = wrap_overrides_.find(document); found != wrap_overrides_.end()) {
        return {found->second ? EditorView::Wrapping::view : EditorView::Wrapping::off, column};
    }
    switch (settings.word_wrap) {
    case core::WordWrap::on:
        return {EditorView::Wrapping::view, column};
    case core::WordWrap::word_wrap_column:
        return {EditorView::Wrapping::column, column};
    case core::WordWrap::bounded:
        return {EditorView::Wrapping::bounded, column};
    case core::WordWrap::off:
        break;
    }
    return {EditorView::Wrapping::off, column};
}

void MainWindow::toggle_word_wrap() {
    const std::shared_ptr<core::Document> document = view()->document();
    if (!document) {
        return;
    }
    if (wrap_overrides_.erase(document) == 0) {
        wrap_overrides_.emplace(document, view()->wrapping() == EditorView::Wrapping::off);
    }
    area_->refresh_wrapping();
}

core::ContextKeys MainWindow::menu_context() const {
    core::ContextKeys keys;
    workbench::set_menu_context_defaults(keys);
    workbench::set_menu_config_context(keys, settings_->settings());
    fill_context(keys);
    // Only a menu reads these, and finding the dirty documents grows with the number of tabs, which
    // `fill_context` must not do because it runs on every key press.
    keys.set("activeEditor", current_uri_ptr() != nullptr);
    keys.set("activeEditorCanRevert", current_uri_ptr() && current_uri_ptr()->file_path());
    bool editor_open = false;
    for (const workbench::GroupId id : layout_.group_ids()) {
        editor_open = editor_open || !layout_.group(id)->tabs.empty();
    }
    keys.set("editorIsOpen", editor_open);
    keys.set("dirtyWorkingCopies", std::ranges::any_of(documents_, [](const auto& entry) {
                 return entry.second->is_dirty();
             }));
    keys.set("isFullscreen", isFullScreen());
    keys.set("sideBarVisible", workbench_->is_visible(Part::sidebar));
    keys.set("panelVisible", workbench_->is_visible(Part::panel));
    keys.set("panelPosition", std::string(panel_position_name(workbench_->panel_position())));
    keys.set("canReopenClosedEditor", !closed_editors_.empty());
    const EditorView* shown = area_->focused_group()->view();
    keys.set("canToggleWordWrap", shown->document() != nullptr);
    keys.set("editorWordWrap", shown->wrapping() != EditorView::Wrapping::off);
    keys.set("canNavigateBack", history_.can_go_back());
    keys.set("canNavigateForward", history_.can_go_forward());
    keys.set("canNavigateToLastEditLocation", history_.can_go_to_last_edit());
    return keys;
}

void MainWindow::populate_menu(QMenu* menu, const std::vector<workbench::MenuItem>& items) {
    for (const workbench::MenuItem& item : items) {
        switch (item.kind) {
        case workbench::MenuEntryKind::separator:
            menu->addSeparator();
            break;
        case workbench::MenuEntryKind::submenu: {
            QMenu* submenu = menu->addMenu(menu_text(item.label));
            style_menu(submenu);
            populate_menu(submenu, item.items);
            break;
        }
        case workbench::MenuEntryKind::item: {
            const QString title = menu_text(item.label);
            const QString label = keys_->shortcut_label(item.id);
            QAction* action =
                menu->addAction(label.isEmpty() ? title : title + QLatin1Char('\t') + label);
            action->setData(QString::fromStdString(item.id));
            action->setProperty("anvil.title", title);
            if (item.checkable) {
                action->setCheckable(true);
                action->setChecked(item.checked);
            }
            if (commands_.find(item.id)) {
                action->setEnabled(item.enabled);
                connect(action, &QAction::triggered, this,
                        [this, id = item.id] { commands_.execute(id); });
            } else {
                action->setEnabled(false);
                action->setToolTip(
                    tr("%1 is not available yet").arg(QString::fromStdString(item.label.text)));
            }
            break;
        }
        }
    }
}

void MainWindow::build_menus() {
    menu_style_ = std::make_unique<MenuStyle>(colors_);
    menu_style_->set_zoom(&zoom_);
    const auto apply_menu_fonts = [this] {
        for (QMenu* menu : findChildren<QMenu*>()) {
            menu->setFont(menu_font());
        }
        // The menu bar of a window with the system's frame; the title bar sizes its own.
        if (!has_custom_title_bar()) {
            menu_bar_->setFont(menu_font());
        }
    };
    connect(&zoom_, &Zoom::changed, this, apply_menu_fonts);
    apply_menu_fonts();
    const core::ContextKeys context = menu_context();
    for (workbench::MenuItem top : workbench::top_level_menus(context)) {
        // Alt+C, Alt+W and Alt+R are the find bar's option shortcuts, and a menu that shares one
        // makes Qt trigger neither, so VS Code's Alt+R for Run is left out.
        if (top.label.mnemonic != workbench::MenuLabel::npos &&
            QStringLiteral("cwr").contains(QChar(top.label.text[top.label.mnemonic]).toLower())) {
            top.label.mnemonic = workbench::MenuLabel::npos;
        }
        QMenu* menu = menu_bar_->addMenu(menu_text(top.label));
        style_menu(menu);
        menu->setToolTipsVisible(true);
        // The entries depend on the context and on the commands that exist, and the key bindings
        // can change while the window runs, so a menu is filled when it opens.
        connect(menu, &QMenu::aboutToShow, this, [this, menu, id = top.id] {
            for (QMenu* submenu : menu->findChildren<QMenu*>(Qt::FindDirectChildrenOnly)) {
                submenu->deleteLater();
            }
            menu->clear();
            populate_menu(menu, workbench::menu_items(id, menu_context(),
                                                      workbench::unprovided_menu_items()));
        });
    }
}

void MainWindow::fill_context(core::ContextKeys& keys) const {
#if defined(Q_OS_MACOS)
    keys.set("isMac", true);
    keys.set("isMacNative", true);
#elif defined(Q_OS_WIN)
    keys.set("isWindows", true);
#else
    keys.set("isLinux", true);
#endif
    QWidget* focus = QApplication::focusWidget();
    const auto* view = qobject_cast<const EditorView*>(focus);
    const bool input = qobject_cast<const QLineEdit*>(focus) != nullptr;
    bool editor_focus = view != nullptr;
    for (EditorGroupWidget* group : area_->groups()) {
        editor_focus = editor_focus || (focus && group->find_bar()->isAncestorOf(focus));
    }
    const bool find_bar_visible = area_->focused_group()->find_bar()->isVisible();
    keys.set("workbenchState", std::string(options_.workspace ? "folder" : "empty"));
    keys.set("inZenMode", zen_.active);
    keys.set("isCenteredLayout", area_->centered());
    keys.set("editorFocus", editor_focus);
    keys.set("editorTextFocus", view != nullptr);
    keys.set("inputFocus", input);
    const bool explorer_focus = explorer_ && focus && explorer_->isAncestorOf(focus);
    keys.set("explorerViewletVisible", explorer_ && explorer_->isVisible());
    keys.set("filesExplorerFocus", explorer_focus);
    keys.set("explorerViewletFocus", explorer_focus);
    if (explorer_focus) {
        const bool selected = !explorer_->selected_paths().empty();
        keys.set("explorerResourceIsRoot", !selected);
        keys.set("explorerResourceIsFolder", explorer_->selection_is_directory());
        keys.set("explorerResourceMoveableToTrash", selected && explorer_->trash_enabled());
    }
    keys.set("textInputFocus", view != nullptr || input);
    if (view && view->document()) {
        const core::SelectionSet& selections = view->document()->selections();
        keys.set("editorHasSelection", !selections.primary().empty());
        keys.set("editorHasMultipleSelections", selections.all().size() > 1);
    }

    if (const core::Uri* uri = current_uri_ptr()) {
        const std::string path = uri->path;
        const std::size_t slash = path.find_last_of('/');
        const std::string name = path.substr(slash == std::string::npos ? 0 : slash + 1);
        const std::size_t dot = name.find_last_of('.');
        keys.set("resourceScheme", uri->scheme);
        keys.set("resourceFilename", name);
        keys.set("resourceExtname",
                 dot == std::string::npos || dot == 0 ? std::string() : name.substr(dot));
        keys.set("resourceLangId", std::string("plaintext"));
    }
    const workbench::EditorGroup* group = layout_.group(layout_.focused());
    keys.set("findWidgetVisible", find_bar_visible);
    keys.set("activeEditorGroupEmpty", group && group->tabs.empty());
    keys.set("multipleEditorGroups", layout_.group_ids().size() > 1);
    keys.set("multiCursorModifier", std::string(settings_->editor().multi_cursor_modifier ==
                                                        core::MultiCursorModifier::ctrl_cmd
                                                    ? "ctrlCmd"
                                                    : "altKey"));
}

void MainWindow::run_bound_command(const std::string& command, const nlohmann::json* args) {
    if (commands_.find(command)) {
        commands_.execute(command, args ? nlohmann::json::array({*args}) : nlohmann::json::array());
    } else {
        execute_command(command);
    }
}

const core::Uri* MainWindow::current_uri_ptr() const {
    const workbench::EditorGroup* group = layout_.group(layout_.focused());
    return group && group->active ? &group->tabs[*group->active] : nullptr;
}

std::optional<core::Uri> MainWindow::uri_of(const std::shared_ptr<core::Document>& document) const {
    for (const auto& [uri, candidate] : documents_) {
        if (candidate == document) {
            return uri;
        }
    }
    return std::nullopt;
}

SetiIcon MainWindow::tab_icon(const core::Uri& uri) {
    if (!documents_.contains(uri)) {
        return file_icon_of(uri);
    }
    language_of_document(uri);
    DocumentLanguage& language = document_languages_.find(uri)->second;
    if (!language.icon) {
        language.icon = file_icon_of(uri);
    }
    return *language.icon;
}

TabStrip::Tab MainWindow::tab_info(const core::Uri& uri) {
    const auto path = uri.file_path();
    const auto found = documents_.find(uri);
    const std::optional<QString> titled = content_title(uri);
    QString tooltip = QString::fromStdString(path ? path->string() : uri.to_string());
    if (deleted_files_.contains(uri)) {
        tooltip += tr(" • Deleted");
    }
    return {.title = titled.value_or(display_name(uri)),
            .tooltip = tooltip,
            .dirty = found != documents_.end() && found->second->is_dirty(),
            .has_path = path.has_value(),
            .icon = tab_icon(uri),
            .description = titled ? display_name(uri) : QString(),
            .deleted = deleted_files_.contains(uri)};
}

void MainWindow::add_untitled_document(std::optional<workbench::GroupId> group, core::Text text) {
    const core::Uri uri{"untitled", "", "Untitled-" + std::to_string(next_untitled_++), "", ""};
    auto document =
        std::make_shared<core::Document>(std::move(text), settings_->editor().line_ending);
    bridge_.add_document(uri, document);
    static_cast<void>(retitle(uri, *document));
    if (!group) {
        show_document(uri, std::move(document));
        return;
    }
    documents_.emplace(uri, std::move(document));
    layout_.open(*group, uri);
    layout_.focus(*group);
    focus_current_editor();
}

void MainWindow::show_keybindings_reference() {
    CommandTitles titles;
    for (const CommandRegistry::Command& command : commands_.commands()) {
        titles.emplace(command.id, command.title);
    }
    const std::string reference = keybindings_reference(titles, keys_->table(), keys_->platform());
    add_untitled_document(std::nullopt, core::Text::from_utf8(reference).value_or(core::Text{}));
}

void MainWindow::show_document(const core::Uri& uri, std::shared_ptr<core::Document> document) {
    documents_.try_emplace(uri, std::move(document));
    refresh_file_icons();
    follow_files();
    layout_.show(uri);
    focus_current_editor();
}

void MainWindow::record_closed(workbench::GroupId group, std::size_t index, const core::Uri& uri) {
    const auto found = documents_.find(uri);
    if (found == documents_.end()) {
        return;
    }
    const core::Document& document = *found->second;
    const bool is_file = uri.file_path().has_value();
    if (!is_file && document.text().size() == 0) {
        return;
    }
    workbench::ClosedEditor closed;
    closed.group = group;
    closed.index = index;
    closed.uri = uri;
    for (const core::Selection& selection : document.selections().all()) {
        closed.selections.push_back({document.text().position_at(selection.anchor),
                                     document.text().position_at(selection.active)});
    }
    closed.primary = document.selections().primary_index();
    if (!is_file) {
        closed.text = document.text();
    }
    closed_editors_.push(std::move(closed));
}

void MainWindow::reopen_closed_editor() {
    while (!closed_editors_.empty()) {
        bool reopened = false;
        for (const workbench::ClosedEditor& closed : closed_editors_.take_last_batch()) {
            reopened = reopen(closed) || reopened;
        }
        if (reopened) {
            return;
        }
    }
}

bool MainWindow::reopen(const workbench::ClosedEditor& closed) {
    const workbench::GroupId group = layout_.group(closed.group) ? closed.group : layout_.focused();
    core::Uri uri = closed.uri;
    if (const auto path = uri.file_path()) {
        const workbench::EditorGroup* state = layout_.group(group);
        if (std::ranges::find(state->tabs, uri) != state->tabs.end()) {
            return false;
        }
        if (!documents_.contains(uri) && !std::filesystem::exists(*path)) {
            status_bar_->show_message(tr("%1 no longer exists").arg(display_name(uri)), 5000);
            return false;
        }
        const auto document = file_document(*path, uri);
        if (!document) {
            return false;
        }
        documents_.try_emplace(uri, document);
        refresh_file_icons();
        follow_files();
        layout_.open(group, uri);
    } else {
        add_untitled_document(group);
        const workbench::EditorGroup* state = layout_.group(group);
        uri = state->tabs[*state->active];
        if (closed.text) {
            const std::vector<core::Edit> insert = {{0, 0, closed.text->to_string()}};
            if (auto change = documents_.at(uri)->apply_edits(insert)) {
                bridge_.text_changed(uri, *change);
                document_edited(uri, *change);
            }
        }
    }

    const auto locations = layout_.locations(uri);
    const auto here = std::ranges::find(locations, group, &workbench::TabLocation::group);
    std::size_t index = here->index;
    const std::size_t wanted = std::min(closed.index, layout_.group(group)->tabs.size() - 1);
    if (wanted != index && layout_.move_tab(group, index, group, wanted)) {
        index = wanted;
    }
    layout_.activate(group, index);
    layout_.focus(group);
    focus_current_editor();

    const core::Text& text = documents_.at(uri)->text();
    std::vector<core::Selection> selections;
    for (const workbench::NavigationSelection& saved : closed.selections) {
        selections.push_back({text.offset_at(saved.anchor), text.offset_at(saved.active)});
    }
    if (!selections.empty()) {
        view()->select_ranges(std::move(selections),
                              std::min(closed.primary, closed.selections.size() - 1));
    }
    return true;
}

bool MainWindow::move_tab(workbench::GroupId from, std::size_t index, workbench::GroupId to,
                          std::size_t to_index) {
    const workbench::EditorGroup* source = layout_.group(from);
    if (!source || index >= source->tabs.size() || !layout_.group(to)) {
        return false;
    }
    // The move can close the source group, which drops the places of the group.
    const core::Uri uri = source->tabs[index];
    history_.move_editor(from, to, uri);
    return layout_.move_tab(from, index, to, to_index);
}

void MainWindow::activate_tab(workbench::GroupId group, std::size_t index) {
    layout_.activate(group, index);
    layout_.focus(group);
    focus_current_editor();
}

void MainWindow::focus_current_editor() {
    EditorGroupWidget* group = area_->focused_group();
    if (group && !group->is_empty()) {
        group->view()->setFocus();
    }
}

EditorView::OccurrenceSource MainWindow::occurrence_source() {
    FindBar* bar = find_bar();
    EditorView::OccurrenceSource source;
    source.match_case = bar->match_case_button()->isChecked();
    source.whole_word = bar->whole_word_button()->isChecked();
    // The find bar owns the search while it is shown, has a pattern and the editor has no focus.
    if (!view()->hasFocus() && bar->isVisible() && !bar->find_field()->text().isEmpty()) {
        source.find_text = bar->find_field()->text().toStdString();
    }
    return source;
}

void MainWindow::report_truncated_selection(bool truncated) {
    if (truncated) {
        status_bar_->show_message(
            tr("Only the first %1 occurrences were selected").arg(FindBar::match_limit), 8000);
    }
}

void MainWindow::open_find_bar(bool with_replace) {
    if (!area_->focused_group()->is_empty()) {
        find_bar()->open(with_replace);
    }
}

void MainWindow::release_document(const core::Uri& uri) {
    forget_document_language(uri);
    untitled_titles_.erase(uri);
    deleted_files_.erase(uri);
    wrap_overrides_.erase(documents_.at(uri));
    documents_.erase(uri);
    refresh_file_icons();
    follow_files();
}

bool MainWindow::close_document(std::size_t index) {
    return close_tab_at(layout_.focused(), index);
}

bool MainWindow::close_tab_at(workbench::GroupId group, std::size_t index) {
    const workbench::EditorGroup* state = layout_.group(group);
    if (!state || index >= state->tabs.size()) {
        return false;
    }
    return close_document_in(group, documents_.at(state->tabs[index]));
}

bool MainWindow::close_document_in(workbench::GroupId group,
                                   const std::shared_ptr<core::Document>& document) {
    const ClosingScope closing(*this);
    if (!confirm_close(document)) {
        return false;
    }
    const auto uri = uri_of(document);
    if (!uri) {
        return true;
    }
    auto locations = layout_.locations(*uri);
    if (locations.empty()) {
        return true;
    }
    // The replacement goes in first so the window never has nothing to show.
    const auto here = std::ranges::find(locations, group, &workbench::TabLocation::group);
    workbench::TabLocation target = here != locations.end() ? *here : locations.front();
    if (documents_.size() == 1 && locations.size() == 1) {
        add_untitled_document(target.group);
        locations = layout_.locations(*uri);
        target = locations.front();
    }
    record_closed(target.group, target.index, *uri);
    layout_.close_tab(target.group, target.index);
    if (layout_.locations(*uri).empty()) {
        bridge_.remove_document(*uri);
        release_document(*uri);
    }
    return true;
}

void MainWindow::close_tabs_where(workbench::GroupId group,
                                  const std::function<bool(std::size_t)>& wanted) {
    const ClosingScope closing(*this);
    // Closing asks about unsaved changes, and extensions can open and close documents meanwhile, so
    // the documents are remembered by identity rather than by position.
    const workbench::EditorGroup* state = layout_.group(group);
    if (!state) {
        return;
    }
    std::vector<std::shared_ptr<core::Document>> targets;
    for (std::size_t i = 0; i < state->tabs.size(); ++i) {
        if (wanted(i)) {
            targets.push_back(documents_.at(state->tabs[i]));
        }
    }
    for (const auto& document : targets) {
        if (!close_document_in(group, document)) {
            return;
        }
    }
}

bool MainWindow::close_group(workbench::GroupId group) {
    const ClosingScope closing(*this);
    const workbench::EditorGroup* state = layout_.group(group);
    if (!state || layout_.group_ids().size() < 2) {
        return false;
    }
    std::vector<std::shared_ptr<core::Document>> documents;
    for (const core::Uri& uri : state->tabs) {
        documents.push_back(documents_.at(uri));
    }
    for (const auto& document : documents) {
        if (!confirm_close(document)) {
            return false;
        }
    }
    // Questions can be asked while extensions close the group, or other groups.
    if (const workbench::EditorGroup* closing_group = layout_.group(group)) {
        for (std::size_t i = 0; i < closing_group->tabs.size(); ++i) {
            record_closed(group, i, closing_group->tabs[i]);
        }
    }
    if (!layout_.close_group(group)) {
        return false;
    }
    for (const auto& document : documents) {
        if (const auto uri = uri_of(document); uri && layout_.locations(*uri).empty()) {
            bridge_.remove_document(*uri);
            release_document(*uri);
        }
    }
    if (documents_.empty()) {
        add_untitled_document();
    }
    return true;
}

bool MainWindow::confirm_close(const std::shared_ptr<core::Document>& document) {
    if (!document->is_dirty() || !options_.confirm_unsaved_changes) {
        return true;
    }
    const auto uri = uri_of(document);
    if (!uri) {
        return true;
    }
    show_document(*uri, document);
    const auto answer = QMessageBox::question(
        this, tr("Unsaved changes"), tr("Save changes to %1?").arg(document_name(*uri)),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (answer == QMessageBox::Discard) {
        return true;
    }
    const auto current = uri_of(document);
    return answer == QMessageBox::Save && current && save_document(*current);
}

void MainWindow::update_document(const core::Uri& uri) {
    for (const workbench::TabLocation& location : layout_.locations(uri)) {
        area_->refresh_tab(location.group, location.index);
    }
    const core::Uri* current = current_uri_ptr();
    if (current && *current == uri) {
        update_title();
    }
}

bool MainWindow::open_file(const QString& path) {
    const std::filesystem::path file = QFileInfo(path).absoluteFilePath().toStdString();
    const core::Uri uri = core::Uri::file(file);
    if (const auto found = documents_.find(uri); found != documents_.end()) {
        show_document(uri, found->second);
        return true;
    }
    std::shared_ptr<core::Document> document = file_document(file, uri);
    if (!document) {
        return false;
    }
    show_document(uri, std::move(document));
    return true;
}

std::expected<std::shared_ptr<core::Document>, QString>
MainWindow::load_document(const std::filesystem::path& file, const core::Uri& uri) {
    if (const auto found = documents_.find(uri); found != documents_.end()) {
        return found->second;
    }
    // An extension may have opened the document already; share it so both see the same text.
    if (auto shared = bridge_.find(uri)) {
        // Its file is not watched until the window has the document.
        if (const auto bytes = platform::read_file(file);
            bytes && !shared->is_dirty() && !shared->matches_file(*bytes)) {
            if (const auto current = core::Document::from_file_bytes(*bytes)) {
                static_cast<void>(take_file_content(uri, shared, *current, *bytes));
            }
        }
        return shared;
    }
    auto bytes = platform::read_file(file);
    if (!bytes) {
        return std::unexpected(QString::fromStdString(bytes.error().message));
    }
    auto decoded = core::Document::from_file_bytes(*bytes);
    if (!decoded) {
        return std::unexpected(
            tr("%1 is not a UTF-8 text file.").arg(QString::fromStdString(file.string())));
    }
    auto document = std::make_shared<core::Document>(std::move(*decoded));
    bridge_.add_document(uri, document);
    return document;
}

std::shared_ptr<core::Document> MainWindow::file_document(const std::filesystem::path& file,
                                                          const core::Uri& uri) {
    auto document = load_document(file, uri);
    if (!document) {
        QMessageBox::warning(this, tr("Open"), document.error());
        return nullptr;
    }
    return *document;
}

workbench::EditorLayout MainWindow::saved_editors() const {
    return layout_.retained([](const core::Uri& uri) { return uri.file_path().has_value(); });
}

bool MainWindow::restore_editors() {
    if (!settings_->editor().restore_editors) {
        return false;
    }
    const auto saved = state_->value(workbench::StateScope::workspace, editor_layout_key);
    if (!saved) {
        return false;
    }
    core::NullLogger discarded;
    core::Logger& logger = options_.logger ? *options_.logger : discarded;
    const auto layout = workbench::EditorLayout::from_json(*saved);
    if (!layout) {
        logger.log(core::LogLevel::warning, "state", layout.error().message);
        return false;
    }
    // A file that cannot be read is left out, without a dialog for each at every start.
    const workbench::EditorLayout readable = layout->retained([&](const core::Uri& uri) {
        const auto path = uri.file_path();
        if (!path) {
            return false;
        }
        auto document = load_document(*path, uri);
        if (!document) {
            logger.log(core::LogLevel::warning, "state", document.error().toStdString());
            return false;
        }
        documents_.try_emplace(uri, std::move(*document));
        return true;
    });
    if (documents_.empty()) {
        return false;
    }
    layout_.restore(readable);
    refresh_file_icons();
    follow_files();
    return true;
}

void MainWindow::switch_editors() {
    // The tabs go first, so that nothing asks for a document that is released.
    layout_.restore(workbench::EditorLayout());
    std::vector<core::Uri> open;
    for (const auto& [uri, document] : documents_) {
        open.push_back(uri);
    }
    for (const core::Uri& uri : open) {
        bridge_.remove_document(uri);
        release_document(uri);
    }
    // What was done in the workspace left does not lead back into it. The navigation history drops
    // its places with the groups they were in.
    closed_editors_ = {};
    recent_documents_.clear();
    shown_uri_.reset();
    next_untitled_ = 1;
    if (!restore_editors()) {
        add_untitled_document();
    }
}

MainWindow::SaveOutcome MainWindow::write_document(const core::Uri& uri,
                                                   const std::filesystem::path& path,
                                                   bool automatic) {
    const std::shared_ptr<core::Document> document = documents_.at(uri);
    // Another file than the document's own is one that the user chose to replace.
    if (uri.file_path() == path &&
        settings_->editor().save_conflict_resolution == core::SaveConflictResolution::ask_user) {
        const auto changed = exthost::file_changed_under(*document, path);
        if (!changed) {
            report_save_failure(QString::fromStdString(changed.error().message), automatic);
            return SaveOutcome::failed;
        }
        if (*changed) {
            return resolve_save_conflict(uri, document, automatic);
        }
    }
    return write_over(uri, path, automatic);
}

MainWindow::SaveOutcome MainWindow::write_over(const core::Uri& uri,
                                               const std::filesystem::path& path, bool automatic) {
    const std::shared_ptr<core::Document> document = documents_.at(uri);
    const std::string bytes = document->to_file_bytes();
    // The directory is made again for a document whose file was deleted together with it.
    auto written = path.has_parent_path() ? platform::make_directory(path.parent_path())
                                          : std::expected<void, Error>();
    if (written) {
        written = platform::write_file(path, bytes);
    }
    if (!written) {
        report_save_failure(QString::fromStdString(written.error().message), automatic);
        return SaveOutcome::failed;
    }
    document->record_file_content(bytes);
    document->mark_saved();
    deleted_files_.erase(uri);
    bridge_.saved(uri);
    update_document(uri);
    return SaveOutcome::saved;
}

void MainWindow::report_save_failure(const QString& reason, bool automatic) {
    if (automatic) {
        status_bar_->show_message(tr("Auto save failed: %1").arg(reason), 5000);
    } else {
        QMessageBox::critical(this, tr("Save"), reason);
    }
}

MainWindow::SaveOutcome
MainWindow::resolve_save_conflict(const core::Uri& uri,
                                  const std::shared_ptr<core::Document>& document, bool automatic) {
    if (automatic) {
        report_save_failure(tr("%1 has changed on disk").arg(document_name(uri)), true);
        return SaveOutcome::failed;
    }
    // A file that cannot be taken, such as one that is no longer text, is asked about.
    if (!document->is_dirty() && reload_document(uri, document, *uri.file_path())) {
        return SaveOutcome::saved;
    }
    QMessageBox question(
        QMessageBox::Warning, tr("Save"),
        tr("Failed to save '%1': the content of the file is newer.").arg(document_name(uri)),
        QMessageBox::NoButton, this);
    question.setInformativeText(
        tr("Overwrite the file with your changes, or revert to the file and lose them."));
    const QAbstractButton* overwrite = question.addButton(tr("Overwrite"), QMessageBox::AcceptRole);
    const QAbstractButton* revert_to_file =
        question.addButton(tr("Revert"), QMessageBox::DestructiveRole);
    question.setDefaultButton(question.addButton(QMessageBox::Cancel));
    question.exec();
    // The question runs a loop of its own, in which the document can be renamed or closed.
    const auto current = uri_of(document);
    const auto path = current ? current->file_path() : std::nullopt;
    if (path && question.clickedButton() == overwrite) {
        return write_over(*current, *path, false);
    }
    if (path && question.clickedButton() == revert_to_file) {
        if (const auto reloaded = reload_document(*current, document, *path); !reloaded) {
            report_save_failure(QString::fromStdString(reloaded.error().message), false);
            return SaveOutcome::failed;
        }
        return SaveOutcome::saved;
    }
    return SaveOutcome::cancelled;
}

void MainWindow::follow_files() {
    std::set<std::filesystem::path> files;
    for (const auto& [uri, document] : documents_) {
        if (const auto path = uri.file_path()) {
            files.insert(*path);
        }
    }
    open_files_.set_files(files);
}

void MainWindow::files_changed(const std::vector<std::filesystem::path>& files) {
    const std::set<std::filesystem::path> changed(files.begin(), files.end());
    std::vector<core::Uri> open;
    for (const auto& [uri, document] : documents_) {
        if (const auto path = uri.file_path(); path && changed.contains(*path)) {
            open.push_back(uri);
        }
    }
    for (const core::Uri& uri : open) {
        check_file(uri);
    }
}

void MainWindow::check_file(const core::Uri& uri) {
    const auto found = documents_.find(uri);
    const auto path = uri.file_path();
    if (found == documents_.end() || !path) {
        return;
    }
    const auto bytes = platform::read_file(*path);
    if (!bytes) {
        const ErrorCode code = bytes.error().code;
        if (code == ErrorCode::not_found || code == ErrorCode::not_a_directory) {
            set_file_deleted(uri, true);
        }
        return;
    }
    set_file_deleted(uri, false);
    const std::shared_ptr<core::Document> document = found->second;
    if (document->is_dirty() || document->matches_file(*bytes)) {
        return;
    }
    // A file that is not text any more leaves the document as it is; saving it asks.
    if (const auto current = core::Document::from_file_bytes(*bytes)) {
        static_cast<void>(take_file_content(uri, document, *current, *bytes));
    }
}

std::expected<void, Error>
MainWindow::take_file_content(const core::Uri& uri, const std::shared_ptr<core::Document>& document,
                              const core::Document& file, std::string_view bytes) {
    const bool was_dirty = document->is_dirty();
    const bool format_changed =
        file.line_ending() != document->line_ending() || file.has_bom() != document->has_bom();
    // Before the edit, so that extensions read its offsets with the new line ending.
    document->set_file_format(file.line_ending(), file.has_bom());
    core::DocumentChange change{document->version(), core::ChangeCause::edit, {}};
    const core::Text& before = document->text();
    const core::Text& after = file.text();
    if (before.to_string() != after.to_string()) {
        const auto moved = [&](std::size_t offset) {
            return after.offset_at(before.position_at(offset));
        };
        std::vector<core::Selection> selections;
        for (const core::Selection& selection : document->selections().all()) {
            selections.push_back({moved(selection.anchor), moved(selection.active)});
        }
        const core::Edit replacement{0, before.size(), after.to_string()};
        auto applied = document->apply_edits(
            std::span(&replacement, 1),
            {.selections_after = core::SelectionSet::from(std::move(selections),
                                                          document->selections().primary_index())});
        if (!applied) {
            return std::unexpected(std::move(applied.error()));
        }
        change = std::move(*applied);
    }
    document->record_file_content(bytes);
    document->mark_saved();
    if (!change.changes.empty() || format_changed || was_dirty) {
        bridge_.text_changed(uri, change);
    }
    if (!documents_.contains(uri)) {
        return {};
    }
    if (!change.changes.empty()) {
        for (EditorGroupWidget* group : area_->groups()) {
            if (group->view()->document() == document) {
                group->view()->document_changed_elsewhere(change);
            }
        }
        document_edited(uri, change);
    }
    update_document(uri);
    update_status();
    return {};
}

std::expected<void, Error>
MainWindow::reload_document(const core::Uri& uri, const std::shared_ptr<core::Document>& document,
                            const std::filesystem::path& path) {
    const auto bytes = platform::read_file(path);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    const auto current = core::Document::from_file_bytes(*bytes);
    if (!current) {
        return std::unexpected(current.error());
    }
    return take_file_content(uri, document, *current, *bytes);
}

void MainWindow::set_file_deleted(const core::Uri& uri, bool deleted) {
    const bool changed =
        deleted ? deleted_files_.insert(uri).second : deleted_files_.erase(uri) > 0;
    if (changed) {
        update_document(uri);
    }
}

void MainWindow::schedule_auto_save(const core::Uri& uri) {
    if (!uri.file_path()) {
        return;
    }
    const auto found = documents_.find(uri);
    if (found == documents_.end()) {
        return;
    }
    ScheduledAutoSave& scheduled = scheduled_auto_saves_[found->second.get()];
    if (!scheduled.timer) {
        scheduled.document = found->second;
        scheduled.timer = std::make_unique<QTimer>();
        scheduled.timer->setSingleShot(true);
        connect(scheduled.timer.get(), &QTimer::timeout, this, [this, key = found->second.get()] {
            auto node = scheduled_auto_saves_.extract(key);
            if (node.empty()) {
                return;
            }
            // The timer is the sender of this slot and cannot be deleted inside it.
            node.mapped().timer.release()->deleteLater();
            if (const auto document = node.mapped().document.lock()) {
                auto_save(document);
            }
        });
    }
    scheduled.timer->start(auto_save_delay_);
}

void MainWindow::auto_save(const std::shared_ptr<core::Document>& document) {
    if (!document->is_dirty()) {
        return;
    }
    const auto uri = uri_of(document);
    const auto path = uri ? uri->file_path() : std::nullopt;
    if (path) {
        write_document(*uri, *path, true);
    }
}

void MainWindow::auto_save_all() {
    std::vector<std::shared_ptr<core::Document>> dirty;
    for (const auto& [uri, document] : documents_) {
        if (document->is_dirty() && uri.file_path()) {
            dirty.push_back(document);
        }
    }
    for (const auto& document : dirty) {
        auto_save(document);
    }
}

void MainWindow::cancel_scheduled_auto_saves() {
    for (auto& [key, scheduled] : scheduled_auto_saves_) {
        scheduled.timer.release()->deleteLater();
    }
    scheduled_auto_saves_.clear();
}

void MainWindow::on_focus_changed(QWidget* old_widget, QWidget* now) {
    const auto* view = qobject_cast<const EditorView*>(old_widget);
    if (auto_save_ != core::AutoSave::on_focus_change || !view || !view->document() ||
        !isAncestorOf(old_widget)) {
        return;
    }
    for (EditorGroupWidget* group : area_->groups()) {
        if (group->isAncestorOf(old_widget) && now && group->isAncestorOf(now)) {
            return;
        }
    }
    auto_save(view->document());
}

bool MainWindow::save() {
    const core::Uri* current = current_uri_ptr();
    return current && save_document(*current);
}

bool MainWindow::save_as(const QString& path) {
    const core::Uri* current = current_uri_ptr();
    return current && save_document_as(*current, path);
}

void MainWindow::focus_group_number(std::size_t index) {
    const std::vector<workbench::GroupId> groups = layout_.group_ids();
    if (index < groups.size()) {
        layout_.focus(groups[index]);
    } else if (index == groups.size()) {
        // The group after the last one is made by splitting the last, as VS Code does.
        layout_.split(groups.back(), workbench::Direction::right);
    } else {
        return;
    }
    focus_current_editor();
}

void MainWindow::show_current_after(bool changed) {
    if (changed) {
        focus_current_editor();
    }
}

bool MainWindow::save_all() {
    std::vector<std::shared_ptr<core::Document>> dirty;
    std::set<const core::Document*> seen;
    for (const workbench::GroupId id : layout_.group_ids()) {
        for (const core::Uri& uri : layout_.group(id)->tabs) {
            const auto found = documents_.find(uri);
            if (found != documents_.end() && found->second->is_dirty() &&
                seen.insert(found->second.get()).second) {
                dirty.push_back(found->second);
            }
        }
    }
    bool all_saved = true;
    for (const std::shared_ptr<core::Document>& document : dirty) {
        const auto uri = uri_of(document);
        if (!uri || !document->is_dirty()) {
            continue;
        }
        if (const auto path = uri->file_path()) {
            const SaveOutcome outcome = write_document(*uri, *path);
            // Cancelling a question stops the rest.
            if (outcome == SaveOutcome::cancelled) {
                return false;
            }
            all_saved = outcome == SaveOutcome::saved && all_saved;
        } else if (!save_document(*uri)) {
            return false;
        }
    }
    return all_saved;
}

bool MainWindow::revert() {
    const core::Uri* current = current_uri_ptr();
    const auto path = current ? current->file_path() : std::nullopt;
    if (!path) {
        return false;
    }
    const core::Uri uri = *current;
    if (const auto reloaded = reload_document(uri, documents_.at(uri), *path); !reloaded) {
        status_bar_->show_message(
            tr("Revert failed: %1").arg(QString::fromStdString(reloaded.error().message)), 5000);
        return false;
    }
    return true;
}

bool MainWindow::save_document(const core::Uri& uri) {
    if (const auto path = uri.file_path()) {
        return write_document(uri, *path) == SaveOutcome::saved;
    }
    // The dialog can run while an extension renames or closes the document.
    const auto document = documents_.at(uri);
    const QString chosen = QFileDialog::getSaveFileName(this, tr("Save As"));
    const auto current = uri_of(document);
    return !chosen.isEmpty() && current && save_document_as(*current, chosen);
}

bool MainWindow::save_document_as(const core::Uri& uri, const QString& path) {
    const std::filesystem::path file = QFileInfo(path).absoluteFilePath().toStdString();
    const core::Uri new_uri = core::Uri::file(file);
    if (!(new_uri == uri) && documents_.contains(new_uri)) {
        QMessageBox::warning(this, tr("Save As"),
                             tr("%1 is already open in the editor.").arg(path));
        return false;
    }
    if (write_document(uri, file) != SaveOutcome::saved) {
        return false;
    }
    if (!(uri == new_uri)) {
        rekey_document(uri, new_uri, false);
    }
    update_document(new_uri);
    area_->refresh_highlighters();
    return true;
}

void MainWindow::rekey_document(const core::Uri& uri, const core::Uri& new_uri,
                                bool extensions_know) {
    // The document keeps its text and history; extensions see it under its new name.
    std::optional<std::string> chosen;
    if (const auto found = chosen_languages_.find(uri); found != chosen_languages_.end()) {
        chosen = found->second;
        chosen_languages_.erase(found);
        chosen_languages_.emplace(new_uri, *chosen);
    }
    document_languages_.erase(uri);
    auto node = documents_.extract(uri);
    const std::shared_ptr<core::Document> document = node.mapped();
    node.key() = new_uri;
    documents_.insert(std::move(node));
    highlighting_->rename(uri, new_uri);
    if (!extensions_know) {
        bridge_.remove_document(uri);
        bridge_.add_document(new_uri, document, chosen);
    } else if (chosen) {
        bridge_.language_changed(new_uri, *chosen);
    }
    refresh_file_icons();
    follow_files();
    history_.replace_uri(uri, new_uri);
    std::erase(recent_documents_, new_uri);
    std::ranges::replace(recent_documents_, uri, new_uri);
    layout_.replace_document(uri, new_uri);
}

void MainWindow::document_renamed(const core::Uri& uri, const core::Uri& new_uri) {
    if (!documents_.contains(uri) || documents_.contains(new_uri)) {
        return;
    }
    rekey_document(uri, new_uri, true);
    update_document(new_uri);
    area_->refresh_highlighters();
}

void MainWindow::document_deleted(const core::Uri& uri) {
    std::erase(recent_documents_, uri);
    if (!documents_.contains(uri)) {
        return;
    }
    auto locations = layout_.locations(uri);
    if (!locations.empty() && documents_.size() == 1) {
        add_untitled_document(locations.front().group);
    }
    for (locations = layout_.locations(uri); !locations.empty();
         locations = layout_.locations(uri)) {
        layout_.close_tab(locations.front().group, locations.front().index);
    }
    release_document(uri);
}

void MainWindow::save_as_dialog() {
    const QString chosen = QFileDialog::getSaveFileName(this, tr("Save As"));
    if (!chosen.isEmpty()) {
        save_as(chosen);
    }
}

bool MainWindow::open_user_file(const std::filesystem::path& file, std::string_view initial) {
    if (file.empty()) {
        status_bar_->show_message(tr("This window has no configuration directory"), 5000);
        return false;
    }
    std::error_code error;
    if (!std::filesystem::exists(file, error)) {
        std::filesystem::create_directories(file.parent_path(), error);
        if (const auto written = platform::write_file(file, initial); !written) {
            status_bar_->show_message(tr("%1 cannot be created: %2")
                                          .arg(QString::fromStdString(file.string()),
                                               QString::fromStdString(written.error().message)),
                                      8000);
            return false;
        }
    }
    return open_file(QString::fromStdString(file.string()));
}

void MainWindow::open_new_window() {
    platform::ProcessSpec spec;
    spec.executable =
        options_.executable.empty()
            ? std::filesystem::path(QCoreApplication::applicationFilePath().toStdString())
            : options_.executable;
    spec.arguments = {"--new-window"};
    spec.environment = platform::current_environment();
    std::error_code error;
    std::filesystem::path directory = std::filesystem::current_path(error);
    if (error || !std::filesystem::is_directory(directory, error)) {
        directory = std::filesystem::temp_directory_path(error);
    }
    spec.working_directory = directory;
    const auto started =
        options_.start_process ? options_.start_process(spec) : platform::spawn_detached(spec);
    if (!started) {
        status_bar_->show_message(tr("The new window cannot be started: %1")
                                      .arg(QString::fromStdString(started.error().message)),
                                  8000);
    }
}

void MainWindow::reopen_last_folder(core::Logger& logger) {
    const core::RestoreWindows restore = settings_->editor().restore_windows;
    if (options_.workspace || !options_.reopen_folder || restore == core::RestoreWindows::none ||
        (options_.opens_files && restore != core::RestoreWindows::preserve)) {
        return;
    }
    const auto saved = state_->string(workbench::StateScope::global, last_folder_key);
    if (!saved || saved->empty()) {
        return;
    }
    auto workspace =
        core::Workspace::open(std::filesystem::path(std::u8string(saved->begin(), saved->end())));
    if (!workspace) {
        logger.log(core::LogLevel::warning, "state", workspace.error().message);
        return;
    }
    options_.workspace = std::move(*workspace);
    state_->set_workspace(options_.workspace->id);
}

void MainWindow::zoom_by(int step) {
    // Whole levels, rounded as JavaScript's Math.round does, which VS Code uses.
    const double level = std::floor(settings_->editor().zoom_level + 0.5) + step;
    if (level < core::min_zoom_level || level > core::max_zoom_level) {
        return;
    }
    change_setting("window.zoomLevel", static_cast<int>(level), tr("Zoom Level"));
}

void MainWindow::show_about() {
    if (about_dialog_) {
        about_dialog_->raise();
        about_dialog_->activateWindow();
        return;
    }
    auto* dialog = new QMessageBox(QMessageBox::Information, tr("About"),
                                   about_text(about_versions_), QMessageBox::Ok, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    about_dialog_ = dialog;
    dialog->open();
}

void MainWindow::open_project_page(std::string_view address) {
    const QUrl url(QString::fromUtf8(address.data(), static_cast<qsizetype>(address.size())));
    const bool opened = options_.open_url ? options_.open_url(url) : QDesktopServices::openUrl(url);
    if (!opened) {
        status_bar_->show_message(
            tr("The page cannot be opened. Its address is %1").arg(url.toString()), 15000);
    }
}

void MainWindow::open_dialog() {
    const QString chosen = QFileDialog::getOpenFileName(this, tr("Open"));
    if (!chosen.isEmpty()) {
        open_file(chosen);
    }
}

void MainWindow::show_command_palette(const QString& text) {
    if (QApplication::activeModalWidget()) {
        return;
    }
    quick_open_->end();
    dismiss_pick();
    quick_access_ = QuickAccess::commands;
    struct Entry {
        std::string id;
        QString title;
        bool disabled = false;
    };
    std::vector<Entry> entries;
    for (const CommandRegistry::Command& command : commands_.commands()) {
        if (command.id != "setContext" && command.id != "editor.action.selectAllMatches") {
            entries.push_back({command.id, QString::fromStdString(command.title)});
        }
    }
    if (bridge_.running()) {
        const core::ContextKeys context = keys_->context();
        const exthost::Contributions& contributions = bridge_.contributions();
        std::set<std::string> listed;
        for (const exthost::ContributedCommand& command : contributions.commands) {
            if (commands_.find(command.command) || !listed.insert(command.command).second ||
                !shown_in_palette(contributions, command.command, context)) {
                continue;
            }
            entries.push_back({command.command, QString::fromStdString(command.label()),
                               !holds(command.enablement, context)});
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

    palette_->set_filtering(true);
    palette_->set_ignored_prefix(">");
    palette_->set_placeholder(tr("Type the name of a command to run"));
    palette_->set_empty_message(tr("No matching commands"));
    palette_ids_.clear();
    std::vector<QuickPickItem> items;
    for (Entry& entry : entries) {
        palette_ids_.push_back(std::move(entry.id));
        items.push_back({entry.title, {}, {}, {}, entry.disabled});
    }
    palette_->set_items(std::move(items));
    palette_->set_top_offset(has_custom_title_bar() ? title_bar_->height() : menu_bar_->height());
    palette_->open(text);
}

void MainWindow::show_go_to_line(const QString& text) {
    // Run again while it is in use, it leaves the popup and what was typed as they are.
    if (QApplication::activeModalWidget() ||
        (quick_access_ == QuickAccess::line && palette_->isVisible())) {
        return;
    }
    quick_open_->end();
    dismiss_pick();
    quick_access_ = QuickAccess::line;
    if (const core::Uri* current = current_uri_ptr()) {
        go_to_line_state_ = {documents_.at(*current), view()->document()->selections(),
                             view()->scroll_target()};
    }
    palette_->set_filtering(false);
    palette_->set_ignored_prefix(":");
    palette_->set_placeholder(tr("Type a line number, then : and a column"));
    palette_->set_top_offset(has_custom_title_bar() ? title_bar_->height() : menu_bar_->height());
    palette_->open(text);
    preview_go_to_line();
}

void MainWindow::preview_go_to_line() {
    const auto& document = go_to_line_state_.document;
    if (!document) {
        palette_->set_items({{tr("Open a text editor first to go to a line."), {}, {}, {}, true}});
        return;
    }
    if (view()->document() != document) {
        return;
    }
    QString input = palette_->filter_field()->text();
    if (input.startsWith(QLatin1Char(':'))) {
        input.remove(0, 1);
    }
    const core::GoToLine parsed = core::parse_go_to_line(
        document->text(), static_cast<std::size_t>(settings_->editor().tab_size),
        input.toStdString());

    QString label;
    if (!parsed.has_line) {
        label = tr("Type a line number to go to (from 1 to %1).").arg(parsed.line_count);
    } else if (!parsed.has_separator) {
        label = tr("Press 'Enter' to go to line %1 or enter colon : to add a column number.")
                    .arg(parsed.line + 1);
    } else if (!parsed.has_column) {
        label = tr("Press 'Enter' to go to line %1 or enter a column number (from 1 to %2).")
                    .arg(parsed.line + 1)
                    .arg(parsed.max_column);
    } else {
        label = tr("Press 'Enter' to go to line %1 at column %2.")
                    .arg(parsed.line + 1)
                    .arg(parsed.column);
    }
    palette_->set_items({{label, {}, {}, {}, !parsed.has_line}});

    if (parsed.has_line) {
        view()->select_range(core::Selection::caret(document->text().offset_at(parsed.target)));
    } else {
        view()->select_ranges(go_to_line_state_.selections.all(),
                              go_to_line_state_.selections.primary_index());
    }
}

void MainWindow::finish_go_to_line(bool keep) {
    GoToLineState state = std::exchange(go_to_line_state_, {});
    if (!state.document || view()->document() != state.document) {
        return;
    }
    if (keep) {
        record_selection(layout_.focused(), workbench::SelectionReason::navigation);
        return;
    }
    view()->select_ranges(state.selections.all(), state.selections.primary_index());
    view()->scroll_to(state.scroll);
}

bool MainWindow::command_enabled(const std::string& id) const {
    if (!bridge_.running() || commands_.find(id)) {
        return true;
    }
    for (const exthost::ContributedCommand& command : bridge_.contributions().commands) {
        if (command.command == id) {
            return holds(command.enablement, keys_->context());
        }
    }
    return true;
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
    execute_command(id);
}

void MainWindow::execute_command(const std::string& id) {
    if (!command_enabled(id)) {
        return;
    }
    if (bridge_.running()) {
        bridge_.host()->commands().execute(id, "[]", [this, id](exthost::CallResult result) {
            if (!result) {
                status_bar_->show_message(
                    tr("%1 failed: %2")
                        .arg(QString::fromStdString(id), QString::fromStdString(result.error())),
                    8000);
            }
        });
    } else if (!commands_.execute(id)) {
        status_bar_->show_message(tr("Command %1 not found").arg(QString::fromStdString(id)), 5000);
    }
}

void MainWindow::on_text_changed(workbench::GroupId group, const core::DocumentChange& change) {
    const workbench::EditorGroup* state = layout_.group(group);
    if (!state || !state->active) {
        return;
    }
    bridge_.text_changed(state->tabs[*state->active], change);
    // Other groups can show the same document. Their views follow before anything can ask them
    // to paint.
    const auto found = documents_.find(state->tabs[*state->active]);
    for (EditorGroupWidget* other : area_->groups()) {
        if (other->id() != group && found != documents_.end() &&
            other->view()->document() == found->second) {
            other->view()->document_changed_elsewhere(change);
        }
    }
    document_edited(state->tabs[*state->active], change);
    record_selection(group, workbench::SelectionReason::edit);
    if (auto_save_ == core::AutoSave::after_delay) {
        schedule_auto_save(state->tabs[*state->active]);
    }
    area_->refresh_tab(group, *state->active);
    update_title();
    update_status();
}

void MainWindow::on_selections_changed(workbench::GroupId group) {
    const workbench::EditorGroup* state = layout_.group(group);
    if (!state || !state->active) {
        return;
    }
    bridge_.selections_changed(state->tabs[*state->active]);
    record_selection(group, workbench::SelectionReason::user);
    update_status();
}

std::optional<workbench::NavigationSelection> MainWindow::selection_of(const core::Uri& uri) const {
    const auto found = documents_.find(uri);
    if (found == documents_.end()) {
        return std::nullopt;
    }
    const core::Document& document = *found->second;
    const core::Selection& primary = document.selections().primary();
    return workbench::NavigationSelection{document.text().position_at(primary.anchor),
                                          document.text().position_at(primary.active)};
}

void MainWindow::record_activation() {
    const core::Uri* uri = current_uri_ptr();
    if (walking_history_ || go_to_line_state_.document || !uri) {
        return;
    }
    if (const auto selection = selection_of(*uri)) {
        history_.editor_activated(layout_.focused(), *uri, *selection);
    }
}

void MainWindow::record_selection(workbench::GroupId group, workbench::SelectionReason reason) {
    const workbench::EditorGroup* state = layout_.group(group);
    if (walking_history_ || go_to_line_state_.document || !state || !state->active) {
        return;
    }
    const core::Uri& uri = state->tabs[*state->active];
    if (const auto selection = selection_of(uri)) {
        history_.selection_changed(group, uri, *selection, reason);
    }
}

void MainWindow::prune_navigation_history() {
    const std::vector<workbench::GroupId> groups = layout_.group_ids();
    history_.remove_if([&](const workbench::NavigationEntry& entry) {
        if (std::ranges::find(groups, entry.group) == groups.end()) {
            return true;
        }
        return !entry.uri.file_path() && layout_.locations(entry.uri).empty();
    });
}

bool MainWindow::show_place(const workbench::NavigationEntry& place) {
    const auto path = place.uri.file_path();
    if (!documents_.contains(place.uri) && (!path || !std::filesystem::exists(*path))) {
        history_.remove_uri(place.uri);
        status_bar_->show_message(tr("%1 cannot be opened again").arg(display_name(place.uri)),
                                  5000);
        return false;
    }
    const struct Walking {
        explicit Walking(bool& flag) : flag(flag) {
            flag = true;
        }
        ~Walking() {
            flag = false;
        }
        bool& flag;
    } walking(walking_history_);
    if (layout_.group(place.group)) {
        layout_.focus(place.group);
    }
    if (documents_.contains(place.uri)) {
        layout_.open(layout_.focused(), place.uri);
        focus_current_editor();
    } else if (!open_file(QString::fromStdString(path->string()))) {
        history_.remove_uri(place.uri);
        return false;
    }
    const core::Text& text = documents_.at(place.uri)->text();
    view()->select_range(
        {text.offset_at(place.selection.anchor), text.offset_at(place.selection.active)});
    return true;
}

void MainWindow::navigate(std::optional<workbench::NavigationEntry> place, bool to_edit) {
    while (place) {
        if (show_place(*place)) {
            const auto selection = selection_of(place->uri);
            if (selection) {
                if (to_edit) {
                    history_.arrived_at_edit(layout_.focused(), place->uri, *selection);
                } else {
                    history_.arrived(layout_.focused(), place->uri, *selection);
                }
            }
            return;
        }
        place = to_edit ? history_.go_to_last_edit() : std::nullopt;
    }
}

void MainWindow::refresh_current() {
    const core::Uri* current = current_uri_ptr();
    if (current && (!shown_uri_ || !(*shown_uri_ == *current))) {
        if (shown_uri_ && auto_save_ == core::AutoSave::on_focus_change) {
            if (const auto previous = documents_.find(*shown_uri_); previous != documents_.end()) {
                auto_save(previous->second);
            }
        }
        shown_uri_ = *current;
        remember_shown(*current);
        bridge_.show(*current);
    } else if (!current) {
        shown_uri_.reset();
    }
    record_activation();
    update_title();
    update_status();
}

void MainWindow::update_title() {
    const core::Uri* current = current_uri_ptr();
    const QString folder =
        options_.workspace ? QString::fromStdString(options_.workspace->name) : QString();
    if (!current) {
        setWindowTitle(folder.isEmpty() ? tr("Anvil") : tr("%1 — Anvil").arg(folder));
        setWindowModified(false);
        return;
    }
    const QString name = document_name(*current);
    setWindowTitle(folder.isEmpty() ? tr("%1 — Anvil[*]").arg(name)
                                    : tr("%1 — %2 — Anvil[*]").arg(name, folder));
    setWindowModified(documents_.at(*current)->is_dirty());
}

void MainWindow::update_status() {
    status_bar_->set_item(
        host_item_id, StatusBarSide::right,
        {.text = bridge_.running() ? tr("Extension host: running") : tr("Extension host: off")});
    const core::Uri* current = current_uri_ptr();
    if (!current) {
        status_bar_->remove_item(position_item_id);
        status_bar_->remove_item(language_item_id);
        return;
    }
    status_bar_->set_item(language_item_id, StatusBarSide::right,
                          {.text = language_of_document(*current).name,
                           .tooltip = tr("Select Language Mode"),
                           .command = "workbench.action.editor.changeLanguageMode",
                           .priority = language_priority});
    const core::Document& document = *documents_.at(*current);
    const core::Position position =
        document.text().position_at(document.selections().primary().active);
    QString text = tr("Ln %1, Col %2").arg(position.line + 1).arg(position.column + 1);
    if (document.selections().all().size() > 1) {
        text += tr("  (%1 cursors)").arg(document.selections().all().size());
    }
    status_bar_->set_item(position_item_id, StatusBarSide::right,
                          {.text = text, .priority = position_priority});
}

bool MainWindow::resolve_unsaved_changes() {
    std::vector<std::shared_ptr<core::Document>> documents;
    for (const auto& [uri, document] : documents_) {
        documents.push_back(document);
    }
    for (const auto& document : documents) {
        if (!confirm_close(document)) {
            return false;
        }
    }
    return true;
}

void MainWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    switch (event->type()) {
    case QEvent::PaletteChange:
        colors_.set_palette(palette());
        break;
    case QEvent::WindowTitleChange:
    case QEvent::ModifiedChange:
        if (has_custom_title_bar()) {
            title_bar_->set_title(title_text());
        }
        break;
    case QEvent::ActivationChange:
        if (has_custom_title_bar()) {
            title_bar_->set_active(isActiveWindow());
        }
        if (!isActiveWindow() && (auto_save_ == core::AutoSave::on_focus_change ||
                                  auto_save_ == core::AutoSave::on_window_change)) {
            auto_save_all();
        }
        break;
    case QEvent::WindowStateChange:
        update_chrome();
        if (zen_.active && zen_.entered_full_screen && !isFullScreen()) {
            toggle_zen_mode();
        }
        break;
    default:
        break;
    }
}

QString MainWindow::title_text() const {
    QString title = windowTitle();
    title.replace(QStringLiteral("[*]"), isWindowModified() ? QStringLiteral("*") : QString());
    return title;
}

void MainWindow::set_up_title_bar() {
    setWindowFlag(Qt::FramelessWindowHint, true);
    title_bar_ = new TitleBar(colors_, this);
    title_bar_->set_zoom(&zoom_);
    workbench_->set_title_bar(title_bar_);
    menu_bar_ = new QMenuBar;
    title_bar_->set_menu_bar(menu_bar_);
    window_frame_ = new WindowFrame(this);
    frame_border_ = new FrameBorder(colors_, this);

    connect(title_bar_, &TitleBar::move_requested, this, [this] {
        if (!windowHandle() || !windowHandle()->startSystemMove()) {
            use_native_frame();
        }
    });
    connect(title_bar_, &TitleBar::toggle_maximized_requested, this, [this] {
        if (isMaximized()) {
            showNormal();
        } else {
            showMaximized();
        }
    });
    connect(title_bar_, &TitleBar::minimize_requested, this, &QWidget::showMinimized);
    connect(title_bar_, &TitleBar::close_requested, this, &QWidget::close);
    connect(window_frame_, &WindowFrame::resize_failed, this, &MainWindow::use_native_frame);
    // In fullscreen the title bar is out of the way until the pointer reaches the top edge.
    connect(window_frame_, &WindowFrame::fullscreen_pointer_moved, this, [this](int y) {
        if (!isFullScreen()) {
            return;
        }
        if (y <= 1) {
            workbench_->set_visible(Part::title_bar, true);
        } else if (y > title_bar_->height()) {
            workbench_->set_visible(Part::title_bar, false);
        }
    });
    connect(title_bar_, &TitleBar::command_center_requested, this,
            [this] { commands_.execute("workbench.action.showCommands"); });
    connect(title_bar_, &TitleBar::toggle_sidebar_requested, this,
            [this] { commands_.execute("workbench.action.toggleSidebarVisibility"); });
    connect(title_bar_, &TitleBar::toggle_panel_requested, this,
            [this] { commands_.execute("workbench.action.togglePanel"); });
    // The buttons show what the window shows, however the parts were shown or hidden.
    connect(workbench_, &Workbench::visibility_changed, this, [this](Part part, bool shown) {
        if (part == Part::sidebar) {
            title_bar_->set_sidebar_shown(shown);
        } else if (part == Part::panel) {
            title_bar_->set_panel_shown(shown);
        }
    });
    connect(workbench_, &Workbench::sidebar_side_changed, title_bar_, &TitleBar::set_sidebar_side);
    title_bar_->set_sidebar_side(workbench_->sidebar_side());
    title_bar_->set_sidebar_shown(workbench_->is_visible(Part::sidebar));
    title_bar_->set_panel_shown(workbench_->is_visible(Part::panel));
    title_bar_->set_command_center_text(workspace_label());
    title_bar_->set_title(title_text());
    update_chrome();
}

void MainWindow::update_title_bar_tooltips() {
    if (!title_bar_) {
        return;
    }
    const auto with_shortcut = [this](const QString& name, const std::string& command) {
        const QString shortcut = keys_->shortcut_label(command);
        return shortcut.isEmpty() ? name : tr("%1 (%2)").arg(name, shortcut);
    };
    title_bar_->sidebar_button()->setToolTip(
        with_shortcut(tr("Toggle Primary Side Bar"), "workbench.action.toggleSidebarVisibility"));
    title_bar_->panel_button()->setToolTip(
        with_shortcut(tr("Toggle Panel"), "workbench.action.togglePanel"));
    title_bar_->command_center()->setToolTip(
        with_shortcut(tr("Show All Commands"), "workbench.action.showCommands"));
}

void MainWindow::apply_part_visibility() {
    const core::EditorSettings& editor = settings_->editor();
    workbench_->set_visible(Part::status_bar, editor.status_bar_visible &&
                                                  !(zen_.active && editor.zen_hide_status_bar));
    sidebar_->set_activity_bar_allowed(editor.activity_bar_location !=
                                           core::ActivityBarLocation::hidden &&
                                       !(zen_.active && editor.zen_hide_activity_bar));
    area_->set_tabs_mode(zen_.active ? editor.zen_show_tabs : editor.show_tabs);
}

void MainWindow::run_comment_command(bool block, core::LineCommentType type) {
    const core::Uri* uri = current_uri_ptr();
    if (!uri) {
        return;
    }
    const DocumentLanguage& language = language_of_document(*uri);
    const core::EditorSettings& editor = settings_->editor();
    core::CommentOptions options;
    options.tokens = language_comments(language.id).value_or(core::LanguageComments{});
    options.indent_size = static_cast<std::size_t>(editor.tab_size);
    options.insert_space = editor.comments_insert_space;
    options.ignore_empty_lines = editor.comments_ignore_empty_lines;
    if (block) {
        if (!options.tokens.block) {
            status_bar_->show_message(tr("%1 has no block comment.").arg(language.name), 5000);
            return;
        }
        view()->toggle_block_comment(options);
        return;
    }
    if (!options.tokens.line && !options.tokens.block) {
        status_bar_->show_message(tr("%1 has no comment tokens.").arg(language.name), 5000);
        return;
    }
    view()->comment_lines(type, options);
}

void MainWindow::toggle_zen_mode() {
    const core::EditorSettings& editor = settings_->editor();
    const QPointer<QWidget> focused = QApplication::focusWidget();
    if (!zen_.active) {
        zen_ = {};
        zen_.active = true;
        zen_.sidebar_was_visible = workbench_->is_visible(Part::sidebar);
        zen_.panel_was_visible = workbench_->is_visible(Part::panel);
        workbench_->set_visible(Part::sidebar, false);
        workbench_->set_visible(Part::panel, false);
        apply_part_visibility();
        if (editor.zen_center_layout && !area_->centered()) {
            area_->set_centered(true);
            zen_.entered_centered_layout = true;
        }
        if (editor.zen_full_screen && !isFullScreen()) {
            zen_.entered_full_screen = true;
            setWindowState(windowState() | Qt::WindowFullScreen);
        }
    } else {
        const ZenMode before = std::exchange(zen_, {});
        if (before.panel_was_visible) {
            workbench_->set_visible(Part::panel, true);
        }
        if (before.sidebar_was_visible) {
            workbench_->set_visible(Part::sidebar, true);
        }
        apply_part_visibility();
        if (before.entered_centered_layout) {
            area_->set_centered(false);
        }
        if (before.entered_full_screen && isFullScreen()) {
            setWindowState(windowState() & ~Qt::WindowFullScreen);
        }
    }
    if (!focused || !focused->isVisible()) {
        view()->setFocus();
    }
}

void MainWindow::update_chrome() {
    if (!has_custom_title_bar()) {
        return;
    }
    const bool fullscreen = isFullScreen();
    title_bar_->set_maximized(isMaximized());
    workbench_->set_visible(Part::title_bar, !fullscreen);
    frame_border_->setVisible(!isMaximized() && !fullscreen);
}

void MainWindow::use_native_frame() {
    if (!custom_title_bar_) {
        return;
    }
    custom_title_bar_ = false;
    window_frame_->set_enabled(false);
    frame_border_->hide();
    workbench_->set_visible(Part::title_bar, false);
    menu_bar_->setParent(nullptr);
    setMenuBar(menu_bar_);
    const bool shown = isVisible();
    setWindowFlag(Qt::FramelessWindowHint, false);
    if (shown) {
        show();
    }
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (resolve_unsaved_changes()) {
        const std::u8string folder =
            options_.workspace ? options_.workspace->folder.u8string() : std::u8string();
        state_->overwrite(workbench::StateScope::global, last_folder_key,
                          std::string(folder.begin(), folder.end()));
        // The window closed last is the one whose editors are kept.
        state_->overwrite(workbench::StateScope::workspace, editor_layout_key,
                          saved_editors().to_json());
        state_keeper_->save();
        event->accept();
    } else {
        event->ignore();
    }
}

} // namespace anvil::ui
