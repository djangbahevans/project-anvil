#pragma once

#include "anvil/core/document.hpp"
#include "anvil/core/log.hpp"
#include "anvil/core/uri.hpp"
#include "anvil/core/workspace.hpp"
#include "anvil/ui/command_registry.hpp"
#include "anvil/ui/editor_area.hpp"
#include "anvil/ui/editor_view.hpp"
#include "anvil/ui/extension_host_bridge.hpp"
#include "anvil/ui/find_bar.hpp"
#include "anvil/ui/keybinding_service.hpp"
#include "anvil/ui/keybindings_store.hpp"
#include "anvil/ui/panel.hpp"
#include "anvil/ui/quick_pick.hpp"
#include "anvil/ui/settings_store.hpp"
#include "anvil/ui/sidebar.hpp"
#include "anvil/ui/status_bar.hpp"
#include "anvil/ui/tab_strip.hpp"
#include "anvil/ui/theme_store.hpp"
#include "anvil/ui/title_bar.hpp"
#include "anvil/ui/view_container_registry.hpp"
#include "anvil/ui/window_frame.hpp"
#include "anvil/ui/workbench.hpp"
#include "anvil/ui/workbench_colors.hpp"
#include "anvil/workbench/editor_layout.hpp"
#include "anvil/workbench/menu_model.hpp"

#include <QMainWindow>
#include <QTimer>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

class QMenu;
class QMenuBar;

namespace anvil::ui {

struct MainWindowOptions {
    // The extension host to run; none runs the editor without extension support.
    std::optional<ExtensionHostBridge::Config> extension_host;
    // The folder the window works on: its name is in the window title, and the extension host
    // reports it as the workspace folder.
    std::optional<core::Workspace> workspace{};
    // Asks which folder Open Folder opens; none means the question was cancelled. The window shows
    // a folder dialog when unset.
    std::function<std::optional<std::filesystem::path>()> choose_folder;
    // Receives the extension host's messages; discarded when unset.
    std::shared_ptr<core::Logger> logger;
    // The user settings file, read at startup and whenever it changes. A missing file means the
    // defaults; empty means no settings file at all.
    std::filesystem::path settings_file;
    // The user key bindings file, read at startup and whenever it changes. A missing file means
    // the default bindings alone; empty means no such file at all.
    std::filesystem::path keybindings_file;
    // The directory with the color themes; the application's own when unset.
    std::optional<std::filesystem::path> themes_directory;
    // Whether closing asks what to do with unsaved documents.
    bool confirm_unsaved_changes = true;
    // Whether the window has no frame of its own and draws the title bar, with the menu bar and the
    // window buttons in it (ADR 0015). Without it, and always on macOS, the window has the
    // system's frame. The environment variable ANVIL_NATIVE_FRAME also turns it off.
    bool custom_title_bar = true;
};

// The application window: the editor area with one or more groups, each with a tab strip of its
// documents, a find and replace bar and an editor, menus for files, editing and extension commands,
// and a status bar. The group that has the focus is the one the commands, the menus, the status bar
// and the extension host's active editor follow. With an extension host configured, extensions see
// the open documents and can edit and show them.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    using Options = MainWindowOptions;

    explicit MainWindow(Options options = {}, QWidget* parent = nullptr);
    ~MainWindow() override;

    // Opens the file in the focused group, or shows it where it is already open.
    bool open_file(const QString& path);
    // Writes the shown document to its file, asking for one when it has none.
    bool save();
    bool save_as(const QString& path);
    // Closes the document at tab position `index` of the focused group. With unsaved changes it
    // asks whether to save them first; false means the user cancelled. Closing the last document
    // opens an untitled one.
    bool close_document(std::size_t index);

    // The folder the window works on, if any.
    const std::optional<core::Workspace>& workspace() const {
        return options_.workspace;
    }
    // Makes the folder the workspace: the extension host restarts with it (ADR 0016), and the
    // tabs, groups and documents stay as they are. Fails, with a message in the status bar, when
    // the path is not a folder. Opening the folder that is the workspace already does nothing.
    bool open_folder(const std::filesystem::path& folder);
    // Leaves the workspace; the extension host restarts without one.
    void close_folder();

    // The editor, the strip of tabs and the find bar of the focused group.
    EditorView* view() {
        return area_->focused_group()->view();
    }
    TabStrip* tab_strip() {
        return area_->focused_group()->tabs();
    }
    FindBar* find_bar() {
        return area_->focused_group()->find_bar();
    }
    // All the groups, and the model they show: a window shows more than one group after a split.
    EditorArea* editor_area() {
        return area_;
    }
    workbench::EditorLayout& editor_layout() {
        return layout_;
    }
    // The parts of the window: the editor area, and the empty activity bar, sidebar and panel
    // around it.
    Workbench* workbench() {
        return workbench_;
    }
    // The menu bar: in the title bar of a window that draws its own, otherwise the window's menu
    // bar.
    QMenuBar* menu_bar() const {
        return menu_bar_;
    }
    // Null while the window has the system's frame.
    TitleBar* title_bar() const {
        return title_bar_;
    }
    bool has_custom_title_bar() const {
        return title_bar_ != nullptr && custom_title_bar_;
    }
    // The view containers that fill the sidebar and the panel: the explorer, search, extension
    // views. Registering one for the sidebar shows the activity bar.
    ViewContainerRegistry& view_containers() {
        return view_containers_;
    }
    Sidebar* sidebar() {
        return sidebar_;
    }
    Panel* panel() {
        return panel_;
    }
    // The color themes and the one that is applied.
    ThemeStore& themes() {
        return *themes_;
    }
    // The colors of the window's parts; a color theme replaces them.
    WorkbenchColors& colors() {
        return colors_;
    }
    // The strip along the bottom; messages and the items of the window and of extensions show here.
    StatusBar& status_bar() {
        return *status_bar_;
    }
    // The key bindings, which turn key presses into commands before the widgets see them.
    KeybindingService& keybindings() {
        return *keys_;
    }
    QuickPick* command_palette() {
        return palette_;
    }
    SettingsStore& settings() {
        return *settings_;
    }
    // Changes a setting as a command does: the settings file is edited in place when it can be,
    // and otherwise the change is made for this window only, and the status bar says why. The
    // `description` names the setting in that message, which is the key when it is empty.
    SettingsStore::ChangeResult change_setting(const std::string& key, nlohmann::json value,
                                               const QString& description = {});
    // The editor's commands: menus run them, and extensions can execute them by id.
    CommandRegistry& commands() {
        return commands_;
    }
    ExtensionHostBridge& bridge() {
        return bridge_;
    }
    // The document of the active tab of the focused group; both need a focused group that has a
    // tab.
    const core::Uri& current_uri() const {
        return *current_uri_ptr();
    }
    const std::shared_ptr<core::Document>& current_document() const {
        return documents_.at(*current_uri_ptr());
    }
    std::size_t open_document_count() const {
        return documents_.size();
    }

protected:
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    class LayoutWatcher;

    void register_commands();
    // Sets the context keys that the `when` of a key binding reads.
    void fill_context(core::ContextKeys& keys) const;
    // Runs the command of a key binding: the editor's own at once, an extension's through the host.
    void run_bound_command(const std::string& command, const nlohmann::json* args);
    // The context keys that the `when` clauses of the menus read.
    core::ContextKeys menu_context() const;
    // Fills `menu` with `items`. An item runs its command and shows its key binding, and is
    // disabled while no command has its id.
    void populate_menu(QMenu* menu, const std::vector<workbench::MenuItem>& items);
    // Colors the menus from the theme; a menu's submenus follow it.
    void style_menus();
    // Lets extensions execute the registered commands, once the extension host is running.
    void share_commands_with_extensions();
    // Applies `options_.workspace`: restarts the extension host with it and gives it what the
    // window holds (documents, commands, contributions), then shows the folder's name.
    void workspace_changed();
    void apply_extension_host_options();
    // The name of the workspace folder, or "Anvil" without one.
    QString workspace_label() const;
    void open_folder_dialog();
    void build_menus();
    void apply_settings();
    // Whether a document of the window shows the file and has changes that are not saved.
    bool has_unsaved_changes_in(const std::filesystem::path& file) const;
    // Gives the key bindings the default ones followed by the user's.
    void apply_keybindings();
    // Lists the color themes that the extensions declare in their manifests after the built-in
    // ones.
    void add_extension_themes();
    // Takes the key bindings that the extensions declare in their manifests; problems go to
    // `logger`.
    void read_extension_keybindings(core::Logger& logger);
    // What the occurrence commands search for: the find bar's pattern while it owns the search,
    // and always its options.
    EditorView::OccurrenceSource occurrence_source();
    // Tells the user that a selection of all occurrences stopped at the find bar's match limit.
    void report_truncated_selection(bool truncated);
    // Whether the `enablement` of the extension command allows it to run now.
    bool command_enabled(const std::string& id) const;
    // Adds an untitled document and shows it in `group`, or in the focused group.
    void add_untitled_document(std::optional<workbench::GroupId> group = std::nullopt);
    // Makes the document the shown one of the focused group, or of the group that has it already,
    // adding it to the open documents when it is new.
    void show_document(const core::Uri& uri, std::shared_ptr<core::Document> document);
    void activate_tab(workbench::GroupId group, std::size_t index);
    void focus_current_editor();
    void open_find_bar(bool with_replace);
    const core::Uri* current_uri_ptr() const;
    std::optional<core::Uri> uri_of(const std::shared_ptr<core::Document>& document) const;
    TabStrip::Tab tab_info(const core::Uri& uri) const;
    bool close_tab_at(workbench::GroupId group, std::size_t index);
    // Closes the tab of the document in `group` when it has one, else in the group that does; the
    // document is closed with its last tab. The document is found again by identity after the
    // question about unsaved changes, because extensions can change the tabs meanwhile.
    bool close_document_in(workbench::GroupId group,
                           const std::shared_ptr<core::Document>& document);
    // Closes the documents of the tabs of `group` whose position `wanted` accepts, stopping when
    // the user cancels.
    void close_tabs_where(workbench::GroupId group, const std::function<bool(std::size_t)>& wanted);
    bool close_group(workbench::GroupId group);
    // Refreshes what follows the focused group: the title, the status bar and the extension host's
    // active editor.
    void refresh_current();
    // Refreshes the tabs of the document in every group, and the title when it is shown.
    void update_document(const core::Uri& uri);
    void update_title();
    void update_status();
    void on_text_changed(workbench::GroupId group, const core::DocumentChange& change);
    void on_selections_changed(workbench::GroupId group);
    void save_as_dialog();
    void open_dialog();
    // Shows the command palette, unless a dialog is open.
    void show_command_palette();
    // Runs the command the user chose in the palette, found by the id captured when it was chosen.
    void run_palette_command(const std::string& id);
    // Runs a command by id, in the extension host when it is running, with a message when it fails.
    void execute_command(const std::string& id);
    // Documents are found by URI and not by position, because a dialog can be open while an
    // extension changes the tabs.
    bool save_document(const core::Uri& uri);
    bool save_document_as(const core::Uri& uri, const QString& path);
    // Writes the document to `path`. A failure is shown in a dialog, or in the status bar when
    // `automatic`, which is for a save that nobody asked for.
    bool write_document(const core::Uri& uri, const std::filesystem::path& path,
                        bool automatic = false);
    // Restarts the countdown of the auto save after a delay for the document shown by `uri`.
    void schedule_auto_save(const core::Uri& uri);
    // Saves the document when it has a file and unsaved changes; an untitled one is never saved.
    void auto_save(const std::shared_ptr<core::Document>& document);
    void auto_save_all();
    void cancel_scheduled_auto_saves();
    // Saves the document of an editor that loses the focus to a widget outside its group.
    void on_focus_changed(QWidget* old_widget, QWidget* now);
    // Shows the document and, when it has unsaved changes, asks whether to save them. False means
    // the user cancelled or saving failed.
    bool confirm_close(const std::shared_ptr<core::Document>& document);
    bool resolve_unsaved_changes();

    // Gives the window the system's frame and puts the menu bar back in it, when the window system
    // cannot move or resize a window that has none.
    void use_native_frame();
    void set_up_title_bar();
    void update_chrome();
    void update_title_bar_tooltips();
    QString title_text() const;

    Options options_;
    WorkbenchColors colors_;
    Workbench* workbench_ = nullptr;
    TitleBar* title_bar_ = nullptr;
    WindowFrame* window_frame_ = nullptr;
    FrameBorder* frame_border_ = nullptr;
    QMenuBar* menu_bar_ = nullptr;
    bool custom_title_bar_ = false;
    ViewContainerRegistry view_containers_;
    Sidebar* sidebar_ = nullptr;
    Panel* panel_ = nullptr;
    QuickPick* palette_ = nullptr;
    // The commands shown in the palette, by position in its list.
    std::vector<std::string> palette_ids_;
    // The commands run from the palette, the latest first.
    std::vector<std::string> recent_commands_;
    SettingsStore* settings_ = nullptr;
    KeybindingsStore* user_keybindings_ = nullptr;
    std::vector<core::Keybinding> extension_keybindings_;
    ThemeStore* themes_ = nullptr;
    CommandRegistry commands_;
    ExtensionHostBridge bridge_;
    // The open documents, each shown by one or more tabs of the layout.
    std::map<core::Uri, std::shared_ptr<core::Document>> documents_;
    workbench::EditorLayout layout_;
    std::unique_ptr<LayoutWatcher> watcher_;
    EditorArea* area_ = nullptr;
    // The document the extension host was last told is the active editor.
    std::optional<core::Uri> shown_uri_;
    // The extension themes given to the theme store: an identifier and a file for each.
    std::set<std::string> offered_themes_;
    std::size_t next_untitled_ = 1;
    // The auto save settings in force, and the documents whose save after a delay is pending.
    core::AutoSave auto_save_ = core::AutoSave::off;
    int auto_save_delay_ = 1000;
    struct ScheduledAutoSave {
        std::weak_ptr<core::Document> document;
        std::unique_ptr<QTimer> timer;
    };
    std::map<const core::Document*, ScheduledAutoSave> scheduled_auto_saves_;
    StatusBar* status_bar_ = nullptr;
    KeybindingService* keys_ = nullptr;
    QString chord_text_;
};

} // namespace anvil::ui
