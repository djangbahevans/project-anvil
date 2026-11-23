#include "anvil/ui/main_window.hpp"

#include "anvil/exthost/contributions.hpp"
#include "anvil/platform/editor_defaults.hpp"
#include "anvil/platform/file.hpp"
#include "anvil/ui/default_keybindings.hpp"
#include "anvil/ui/resources.hpp"

#include <QAbstractButton>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QVBoxLayout>
#include <QWindow>
#include <algorithm>
#include <functional>
#include <set>

namespace anvil::ui {
namespace {

constexpr const char* position_item_id = "status.editor.position";
constexpr const char* host_item_id = "status.extensionHost";
constexpr int position_priority = 100;

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

class MainWindow::LayoutWatcher : public workbench::EditorLayout::Observer {
public:
    explicit LayoutWatcher(MainWindow& window) : window_(window) {}

    void structure_changed() override {
        window_.refresh_current();
    }
    void tabs_changed(workbench::GroupId) override {
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
        if (item < palette_ids_.size()) {
            run_palette_command(palette_ids_[item]);
        }
    });
    area_ = new EditorArea(
        layout_, colors_, [this](const core::Uri& uri) { return tab_info(uri); },
        [this](const core::Uri& uri) {
            const auto found = documents_.find(uri);
            return found == documents_.end() ? std::shared_ptr<core::Document>() : found->second;
        });
    watcher_ = std::make_unique<LayoutWatcher>(*this);
    layout_.add_observer(watcher_.get());
    workbench_ = new Workbench(colors_, this);
    workbench_->set_editor_area(area_);
    status_bar_ = new StatusBar(colors_, this);
    workbench_->set_status_bar(status_bar_);
    setCentralWidget(workbench_);
    custom_title_bar_ =
        options_.custom_title_bar && !qEnvironmentVariableIsSet("ANVIL_NATIVE_FRAME");
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

    auto logger = options_.logger ? options_.logger : std::make_shared<core::NullLogger>();
    settings_ = new SettingsStore(
        {.file = options_.settings_file, .defaults = platform::editor_defaults(), .logger = logger},
        this);
    connect(settings_, &SettingsStore::changed, this, &MainWindow::apply_settings);
    connect(settings_, &SettingsStore::load_failed, this, [this](const QString& message) {
        status_bar_->show_message(
            tr("Settings not applied, keeping the previous ones: %1").arg(message), 15000);
    });
    settings_->start();

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
    connect(area_, &EditorArea::tab_moved, this,
            [this](workbench::GroupId group, int from, int to) {
                layout_.move_tab(group, static_cast<std::size_t>(from), group,
                                 static_cast<std::size_t>(to));
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
    connect(area_, &EditorArea::selections_changed, this, &MainWindow::on_selections_changed);
    connect(area_, &EditorArea::tab_dropped_on_strip, this,
            [this](workbench::GroupId from, int index, workbench::GroupId to, int position) {
                layout_.move_tab(from, static_cast<std::size_t>(index), to,
                                 static_cast<std::size_t>(position));
                focus_current_editor();
            });
    connect(area_, &EditorArea::tab_dropped_on_group, this,
            [this](workbench::GroupId from, int index, workbench::GroupId to) {
                const workbench::EditorGroup* target = layout_.group(to);
                layout_.move_tab(from, static_cast<std::size_t>(index), to,
                                 target ? target->tabs.size() : 0);
                focus_current_editor();
            });
    connect(area_, &EditorArea::tab_dropped_on_edge, this,
            [this](workbench::GroupId from, int index, workbench::GroupId to,
                   workbench::Direction direction) {
                if (const auto created = layout_.split(to, direction)) {
                    layout_.move_tab(from, static_cast<std::size_t>(index), *created, 0);
                }
                focus_current_editor();
            });
    connect(area_, &EditorArea::focus_requested, this,
            [this](workbench::GroupId group) { layout_.focus(group); });

    if (options_.extension_host) {
        ExtensionHostBridge::Callbacks callbacks;
        callbacks.edited = [this](const core::Uri& uri, const core::DocumentChange&) {
            const auto found = documents_.find(uri);
            if (found == documents_.end()) {
                return;
            }
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
        if (auto started = bridge_.start(*options_.extension_host, logger, std::move(callbacks));
            !started) {
            status_bar_->show_message(tr("The extension host did not start: %1")
                                          .arg(QString::fromStdString(started.error().message)),
                                      15000);
        } else {
            share_commands_with_extensions();
            read_extension_keybindings(*logger);
            add_extension_themes();
        }
    }

    apply_settings();
    add_untitled_document();
}

MainWindow::~MainWindow() {
    // The area observes the layout, which is destroyed before the child widgets are.
    delete area_;
}

void MainWindow::add_extension_themes() {
    std::vector<ThemeStore::Entry> entries;
    for (const exthost::ContributedTheme& theme : bridge_.contributions().themes) {
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
}

void MainWindow::apply_settings() {
    const core::EditorSettings& editor = settings_->editor();
    area_->apply_settings(editor);
    layout_.set_focus_recent_after_close(editor.focus_recent_editor_after_close);
    themes_->select(editor.color_theme, platform::editor_defaults().color_theme);
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
    add("workbench.action.files.save", tr("Save"), [this] { save(); });
    add("workbench.action.files.saveAs", tr("Save As…"), [this] { save_as_dialog(); });
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
    add("workbench.action.closeGroup", tr("Close Editor Group"),
        [this] { close_group(layout_.focused()); });
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

#if !defined(Q_OS_MACOS)
namespace {

QString menu_style_sheet(const WorkbenchColors& colors) {
    const auto css = [&](std::string_view key) { return colors.color(key).name(QColor::HexArgb); };
    QColor disabled = colors.color("menu.foreground");
    disabled.setAlphaF(0.4F);
    return QStringLiteral(
               "QMenu { background: %1; color: %2; border: 1px solid %3; padding: 4px 0; }"
               "QMenu::item { background: transparent; padding: 4px 24px 4px 26px; }"
               "QMenu::item:selected { background: %4; color: %5; }"
               "QMenu::item:disabled { color: %6; }"
               "QMenu::item:disabled:selected { background: transparent; }"
               "QMenu::separator { height: 1px; background: %7; margin: 4px 0; }")
        .arg(css("menu.background"), css("menu.foreground"), css("menu.border"),
             css("menu.selectionBackground"), css("menu.selectionForeground"),
             disabled.name(QColor::HexArgb), css("menu.separatorBackground"));
}

} // namespace
#endif

// The system draws the global menu bar of macOS, and its menus with it.
void MainWindow::style_menus() {
#if !defined(Q_OS_MACOS)
    const QString sheet = menu_style_sheet(colors_);
    for (const QAction* top : menu_bar_->actions()) {
        if (top->menu()) {
            top->menu()->setStyleSheet(sheet);
        }
    }
#endif
}

core::ContextKeys MainWindow::menu_context() const {
    core::ContextKeys keys;
    workbench::set_menu_context_defaults(keys);
    fill_context(keys);
    return keys;
}

void MainWindow::populate_menu(QMenu* menu, const std::vector<workbench::MenuItem>& items) {
    for (const workbench::MenuItem& item : items) {
        switch (item.kind) {
        case workbench::MenuEntryKind::separator:
            menu->addSeparator();
            break;
        case workbench::MenuEntryKind::submenu:
            populate_menu(menu->addMenu(menu_text(item.label)), item.items);
            break;
        case workbench::MenuEntryKind::item: {
            const QString title = menu_text(item.label);
            const QString label = keys_->shortcut_label(item.id);
            QAction* action =
                menu->addAction(label.isEmpty() ? title : title + QLatin1Char('\t') + label);
            action->setData(QString::fromStdString(item.id));
            action->setProperty("anvil.title", title);
            if (commands_.find(item.id)) {
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
    const core::ContextKeys context = menu_context();
    for (workbench::MenuItem top : workbench::top_level_menus(context)) {
        // Alt+C, Alt+W and Alt+R are the find bar's option shortcuts, and a menu that shares one
        // makes Qt trigger neither, so VS Code's Alt+R for Run is left out.
        if (top.label.mnemonic != workbench::MenuLabel::npos &&
            QStringLiteral("cwr").contains(QChar(top.label.text[top.label.mnemonic]).toLower())) {
            top.label.mnemonic = workbench::MenuLabel::npos;
        }
        QMenu* menu = menu_bar_->addMenu(menu_text(top.label));
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
    style_menus();
    connect(&colors_, &WorkbenchColors::changed, this, &MainWindow::style_menus);
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
    layout_.close_tab(target.group, target.index);
    if (layout_.locations(*uri).empty()) {
        bridge_.remove_document(*uri);
        documents_.erase(*uri);
    }
    return true;
}

void MainWindow::close_tabs_where(workbench::GroupId group,
                                  const std::function<bool(std::size_t)>& wanted) {
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
    if (!layout_.close_group(group)) {
        return false;
    }
    for (const auto& document : documents) {
        if (const auto uri = uri_of(document); uri && layout_.locations(*uri).empty()) {
            bridge_.remove_document(*uri);
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

bool MainWindow::write_document(const core::Uri& uri, const std::filesystem::path& path) {
    const std::shared_ptr<core::Document> document = documents_.at(uri);
    if (auto written = platform::write_file(path, document->to_file_bytes()); !written) {
        QMessageBox::critical(this, tr("Save"), QString::fromStdString(written.error().message));
        return false;
    }
    document->mark_saved();
    bridge_.saved(uri);
    update_document(uri);
    return true;
}

bool MainWindow::save() {
    const core::Uri* current = current_uri_ptr();
    return current && save_document(*current);
}

bool MainWindow::save_as(const QString& path) {
    const core::Uri* current = current_uri_ptr();
    return current && save_document_as(*current, path);
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
        auto node = documents_.extract(uri);
        const std::shared_ptr<core::Document> document = node.mapped();
        node.key() = new_uri;
        documents_.insert(std::move(node));
        bridge_.remove_document(uri);
        bridge_.add_document(new_uri, document);
        layout_.replace_document(uri, new_uri);
    }
    update_document(new_uri);
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
    update_status();
}

void MainWindow::refresh_current() {
    const core::Uri* current = current_uri_ptr();
    if (current && (!shown_uri_ || !(*shown_uri_ == *current))) {
        shown_uri_ = *current;
        bridge_.show(*current);
    } else if (!current) {
        shown_uri_.reset();
    }
    update_title();
    update_status();
}

void MainWindow::update_title() {
    const core::Uri* current = current_uri_ptr();
    if (!current) {
        setWindowTitle(tr("Anvil"));
        setWindowModified(false);
        return;
    }
    setWindowTitle(tr("%1 — Anvil[*]").arg(display_name(*current)));
    setWindowModified(documents_.at(*current)->is_dirty());
}

void MainWindow::update_status() {
    status_bar_->set_item(
        host_item_id, StatusBarSide::right,
        {.text = bridge_.running() ? tr("Extension host: running") : tr("Extension host: off")});
    const core::Uri* current = current_uri_ptr();
    if (!current) {
        status_bar_->remove_item(position_item_id);
        return;
    }
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
        break;
    case QEvent::WindowStateChange:
        update_chrome();
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
    title_bar_->set_title(title_text());
    update_chrome();
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
