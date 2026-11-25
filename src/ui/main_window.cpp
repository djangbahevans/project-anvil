#include "anvil/ui/main_window.hpp"

#include "anvil/core/editor_settings.hpp"
#include "anvil/core/go_to_line.hpp"
#include "anvil/exthost/contributions.hpp"
#include "anvil/platform/editor_defaults.hpp"
#include "anvil/platform/file.hpp"
#include "anvil/ui/default_keybindings.hpp"
#include "anvil/ui/menu_style.hpp"
#include "anvil/ui/resources.hpp"

#include <QAbstractButton>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>
#include <algorithm>
#include <functional>
#include <set>
#include <system_error>
#include <utility>

namespace anvil::ui {
namespace {

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
    }
    void tabs_changed(workbench::GroupId) override {
        window_.prune_navigation_history();
        window_.refresh_current();
    }
    void active_tab_changed(workbench::GroupId) override {
        window_.refresh_current();
    }
    void focus_changed(workbench::GroupId) override {
        window_.refresh_current();
    }

private:
    MainWindow& window_;
};

MainWindow::MainWindow(Options options, QWidget* parent)
    : QMainWindow(parent), options_(std::move(options)), colors_(palette()), bridge_(this) {
    resize(1000, 700);
    palette_ = new QuickPick(this, colors_);
    palette_->set_ignored_prefix(">");
    palette_->set_placeholder(tr("Type the name of a command to run"));
    palette_->set_empty_message(tr("No matching commands"));
    connect(palette_, &QuickPick::accepted, this, [this](std::size_t item) {
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
        pick_action_ = nullptr;
        pick_highlighted_ = nullptr;
        if (const auto dismissed = std::exchange(pick_dismissed_, nullptr)) {
            dismissed();
        }
    });
    go_to_line_ = new QuickPick(this, colors_);
    go_to_line_->set_ignored_prefix(":");
    go_to_line_->set_placeholder(tr("Type a line number, then : and a column"));
    go_to_line_->set_filtering(false);
    connect(go_to_line_->filter_field(), &QLineEdit::textChanged, this,
            [this] { preview_go_to_line(); });
    connect(go_to_line_, &QuickPick::accepted, this, [this] { finish_go_to_line(true); });
    connect(go_to_line_, &QuickPick::cancelled, this, [this] { finish_go_to_line(false); });
    area_ = new EditorArea(
        layout_, colors_, [this](const core::Uri& uri) { return tab_info(uri); },
        [this](const core::Uri& uri) {
            const auto found = documents_.find(uri);
            return found == documents_.end() ? std::shared_ptr<core::Document>() : found->second;
        });
    highlighting_ = new Highlighting(this);
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
    workbench_->set_editor_area(area_);
    status_bar_ = new StatusBar(colors_, this);
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
            highlight_edit(uri, change);
            update_document(uri);
            for (EditorGroupWidget* group : area_->groups()) {
                if (group->view()->document() == found->second) {
                    group->view()->document_changed_elsewhere();
                }
            }
            const core::Uri* current = current_uri_ptr();
            if (current && *current == uri) {
                update_status();
            }
        };
        callbacks.saved = [this](const core::Uri& uri) { update_document(uri); };
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
    add_untitled_document();
}

MainWindow::~MainWindow() {
    // The menus are deleted with the window, after its members, and must not outlive their style.
    for (QMenu* menu : findChildren<QMenu*>()) {
        menu->setStyle(nullptr);
    }
    // The pick closes when its widget is deleted, after the members that its handlers use are gone.
    palette_->disconnect(this);
    // Deleting the widgets moves the focus, which must not reach a window that is half destroyed.
    disconnect(qApp, &QApplication::focusChanged, this, &MainWindow::on_focus_changed);
    // The widgets are deleted after the members, and the quick pick reports a cancellation when
    // it loses the focus.
    go_to_line_->disconnect(this);
    go_to_line_->filter_field()->disconnect(this);
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
    options_.workspace = std::move(*workspace);
    workspace_changed();
    return true;
}

void MainWindow::close_folder() {
    if (!options_.workspace) {
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

void MainWindow::workspace_changed() {
    if (options_.extension_host) {
        core::NullLogger discarded;
        core::Logger& logger = options_.logger ? *options_.logger : discarded;
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
    area_->apply_settings(editor);
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
    apply_extension_host_options();
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

std::string MainWindow::resolve_language(const core::Uri& uri, std::string_view first_line) const {
    const auto file = uri.file_path();
    return languages_.language_for(file ? file->generic_string() : uri.path, first_line);
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
    area_->refresh_highlighters();
    update_status();
}

std::shared_ptr<const core::Highlighter> MainWindow::highlighter_of(const core::Uri& uri) {
    const auto found = documents_.find(uri);
    if (found == documents_.end()) {
        return nullptr;
    }
    return highlighting_->highlighter_for(uri, found->second, language_of_document(uri).id);
}

void MainWindow::highlight_edit(const core::Uri& uri, const core::DocumentChange& change) {
    const auto found = documents_.find(uri);
    if (found != documents_.end() && highlighting_->edited(uri, *found->second, change)) {
        area_->refresh_highlighters();
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
    pick_action_ = std::move(chosen);
    pick_highlighted_ = nullptr;
    pick_dismissed_ = std::move(hooks.dismissed);
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
    add("editor.action.jumpToBracket", tr("Go to Bracket"), [this] { view()->jump_to_bracket(); });
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
#else
    Q_UNUSED(menu);
#endif
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
            populate_menu(menu, workbench::menu_items(id, menu_context()));
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

TabStrip::Tab MainWindow::tab_info(const core::Uri& uri) const {
    const auto path = uri.file_path();
    const auto found = documents_.find(uri);
    return {.title = display_name(uri),
            .tooltip = QString::fromStdString(path ? path->string() : uri.to_string()),
            .dirty = found != documents_.end() && found->second->is_dirty(),
            .has_path = path.has_value()};
}

void MainWindow::add_untitled_document(std::optional<workbench::GroupId> group) {
    const core::Uri uri{"untitled", "", "Untitled-" + std::to_string(next_untitled_++), "", ""};
    auto document = std::make_shared<core::Document>(core::Text{}, settings_->editor().line_ending);
    bridge_.add_document(uri, document);
    if (!group) {
        show_document(uri, std::move(document));
        return;
    }
    documents_.emplace(uri, std::move(document));
    layout_.open(*group, uri);
    layout_.focus(*group);
    focus_current_editor();
}

void MainWindow::show_document(const core::Uri& uri, std::shared_ptr<core::Document> document) {
    documents_.try_emplace(uri, std::move(document));
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
        layout_.open(group, uri);
    } else {
        add_untitled_document(group);
        const workbench::EditorGroup* state = layout_.group(group);
        uri = state->tabs[*state->active];
        if (closed.text) {
            const std::vector<core::Edit> insert = {{0, 0, closed.text->to_string()}};
            if (auto change = documents_.at(uri)->apply_edits(insert)) {
                bridge_.text_changed(uri, *change);
                highlight_edit(uri, *change);
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
        forget_document_language(*uri);
        documents_.erase(*uri);
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
            forget_document_language(*uri);
            documents_.erase(*uri);
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
        this, tr("Unsaved changes"), tr("Save changes to %1?").arg(display_name(*uri)),
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

std::shared_ptr<core::Document> MainWindow::file_document(const std::filesystem::path& file,
                                                          const core::Uri& uri) {
    if (const auto found = documents_.find(uri); found != documents_.end()) {
        return found->second;
    }
    // An extension may have opened the document already; share it so both see the same text.
    if (auto shared = bridge_.find(uri)) {
        return shared;
    }
    auto bytes = platform::read_file(file);
    if (!bytes) {
        QMessageBox::warning(this, tr("Open"), QString::fromStdString(bytes.error().message));
        return nullptr;
    }
    auto decoded = core::Document::from_file_bytes(*bytes);
    if (!decoded) {
        QMessageBox::warning(
            this, tr("Open"),
            tr("%1 is not a UTF-8 text file.").arg(QString::fromStdString(file.string())));
        return nullptr;
    }
    auto document = std::make_shared<core::Document>(std::move(*decoded));
    bridge_.add_document(uri, document);
    return document;
}

bool MainWindow::write_document(const core::Uri& uri, const std::filesystem::path& path,
                                bool automatic) {
    const std::shared_ptr<core::Document> document = documents_.at(uri);
    if (auto written = platform::write_file(path, document->to_file_bytes()); !written) {
        const QString reason = QString::fromStdString(written.error().message);
        if (automatic) {
            status_bar_->show_message(tr("Auto save failed: %1").arg(reason), 5000);
        } else {
            QMessageBox::critical(this, tr("Save"), reason);
        }
        return false;
    }
    document->mark_saved();
    bridge_.saved(uri);
    update_document(uri);
    return true;
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
            all_saved = write_document(*uri, *path) && all_saved;
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
    const std::shared_ptr<core::Document> document = documents_.at(uri);
    const auto fail = [this](const std::string& reason) {
        status_bar_->show_message(tr("Revert failed: %1").arg(QString::fromStdString(reason)),
                                  5000);
        return false;
    };
    const auto bytes = platform::read_file(*path);
    if (!bytes) {
        return fail(bytes.error().message);
    }
    const auto decoded = core::Document::from_file_bytes(*bytes);
    if (!decoded) {
        return fail(decoded.error().message);
    }
    const core::Text& before = document->text();
    const core::Text& after = decoded->text();
    if (before.to_string() != after.to_string()) {
        const auto moved = [&](std::size_t offset) {
            return after.offset_at(before.position_at(offset));
        };
        std::vector<core::Selection> selections;
        for (const core::Selection& selection : document->selections().all()) {
            selections.push_back({moved(selection.anchor), moved(selection.active)});
        }
        const core::Edit replacement{0, before.size(), after.to_string()};
        const auto change = document->apply_edits(
            std::span(&replacement, 1),
            {.selections_after = core::SelectionSet::from(std::move(selections),
                                                          document->selections().primary_index())});
        if (!change) {
            return fail(change.error().message);
        }
        bridge_.text_changed(uri, *change);
        highlight_edit(uri, *change);
        for (EditorGroupWidget* group : area_->groups()) {
            if (group->view()->document() == document) {
                group->view()->document_changed_elsewhere();
            }
        }
    }
    document->mark_saved();
    bridge_.saved(uri);
    update_document(uri);
    update_status();
    return true;
}

bool MainWindow::save_document(const core::Uri& uri) {
    if (const auto path = uri.file_path()) {
        return write_document(uri, *path);
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
    if (!write_document(uri, file)) {
        return false;
    }
    if (!(uri == new_uri)) {
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
        bridge_.remove_document(uri);
        bridge_.add_document(new_uri, document, chosen);
        history_.replace_uri(uri, new_uri);
        layout_.replace_document(uri, new_uri);
    }
    update_document(new_uri);
    area_->refresh_highlighters();
    return true;
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

    pick_action_ = nullptr;
    pick_highlighted_ = nullptr;
    pick_dismissed_ = nullptr;
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
    palette_->open(">");
}

void MainWindow::show_go_to_line() {
    const core::Uri* current = current_uri_ptr();
    if (!current || go_to_line_state_.document) {
        return;
    }
    go_to_line_state_ = {documents_.at(*current), view()->document()->selections(),
                         view()->horizontalScrollBar()->value(),
                         view()->verticalScrollBar()->value()};
    go_to_line_->set_top_offset(has_custom_title_bar() ? title_bar_->height()
                                                       : menu_bar_->height());
    go_to_line_->open(":");
    preview_go_to_line();
}

void MainWindow::preview_go_to_line() {
    const auto& document = go_to_line_state_.document;
    if (!document || view()->document() != document) {
        return;
    }
    QString input = go_to_line_->filter_field()->text();
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
    go_to_line_->set_items({{label, {}, {}, {}, !parsed.has_line}});

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
    view()->horizontalScrollBar()->setValue(state.scroll_x);
    view()->verticalScrollBar()->setValue(state.scroll_y);
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
    highlight_edit(state->tabs[*state->active], change);
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
    setWindowTitle(folder.isEmpty() ? tr("%1 — Anvil[*]").arg(display_name(*current))
                                    : tr("%1 — %2 — Anvil[*]").arg(display_name(*current), folder));
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
        } else if (y > TitleBar::bar_height) {
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
        event->accept();
    } else {
        event->ignore();
    }
}

} // namespace anvil::ui
