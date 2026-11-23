#pragma once

#include "anvil/core/document.hpp"
#include "anvil/core/log.hpp"
#include "anvil/core/uri.hpp"
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
#include "anvil/ui/view_container_registry.hpp"
#include "anvil/ui/workbench.hpp"
#include "anvil/ui/workbench_colors.hpp"
#include "anvil/workbench/editor_layout.hpp"
#include "anvil/workbench/menu_model.hpp"

#include <QMainWindow>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

class QMenu;

namespace anvil::ui {

struct MainWindowOptions {
    // The extension host to run; none runs the editor without extension support.
    std::optional<ExtensionHostBridge::Config> extension_host;
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
    // Lets extensions execute the registered commands, once the extension host is running.
    void share_commands_with_extensions();
    void build_menus();
    void apply_settings();
    // Gives the key bindings the default ones followed by the user's.
    void apply_keybindings();
    // Takes the key bindings that the extensions declare in their manifests; problems go to
    // `logger`.
    void read_extension_keybindings(core::Logger& logger);
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
    bool write_document(const core::Uri& uri, const std::filesystem::path& path);
    // Shows the document and, when it has unsaved changes, asks whether to save them. False means
    // the user cancelled or saving failed.
    bool confirm_close(const std::shared_ptr<core::Document>& document);
    bool resolve_unsaved_changes();

    Options options_;
    WorkbenchColors colors_;
    Workbench* workbench_ = nullptr;
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
    std::size_t next_untitled_ = 1;
    StatusBar* status_bar_ = nullptr;
    KeybindingService* keys_ = nullptr;
    QString chord_text_;
};

} // namespace anvil::ui
