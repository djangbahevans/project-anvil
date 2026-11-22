#pragma once

#include "anvil/core/document.hpp"
#include "anvil/core/log.hpp"
#include "anvil/core/uri.hpp"
#include "anvil/ui/command_registry.hpp"
#include "anvil/ui/editor_view.hpp"
#include "anvil/ui/extension_host_bridge.hpp"
#include "anvil/ui/find_bar.hpp"
#include "anvil/ui/panel.hpp"
#include "anvil/ui/quick_pick.hpp"
#include "anvil/ui/settings_store.hpp"
#include "anvil/ui/sidebar.hpp"
#include "anvil/ui/status_bar.hpp"
#include "anvil/ui/theme_store.hpp"
#include "anvil/ui/view_container_registry.hpp"
#include "anvil/ui/workbench.hpp"
#include "anvil/ui/workbench_colors.hpp"

#include <QMainWindow>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

class QMenu;
class QTabBar;

namespace anvil::ui {

struct MainWindowOptions {
    // The extension host to run; none runs the editor without extension support.
    std::optional<ExtensionHostBridge::Config> extension_host;
    // Receives the extension host's messages; discarded when unset.
    std::shared_ptr<core::Logger> logger;
    // The user settings file, read at startup and whenever it changes. A missing file means the
    // defaults; empty means no settings file at all.
    std::filesystem::path settings_file;
    // The directory with the color themes; the application's own when unset.
    std::optional<std::filesystem::path> themes_directory;
    // Whether closing asks what to do with unsaved documents.
    bool confirm_unsaved_changes = true;
};

// The application window: a tab bar of the open documents above one editor showing the selected
// one, a find and replace bar between them, menus for files, editing and extension commands, and a
// status bar. With an extension host configured, extensions see the open documents and can edit and
// show them.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    using Options = MainWindowOptions;

    explicit MainWindow(Options options = {}, QWidget* parent = nullptr);
    ~MainWindow() override;

    // Opens the file in the editor, or shows it when it is already open.
    bool open_file(const QString& path);
    // Writes the shown document to its file, asking for one when it has none.
    bool save();
    bool save_as(const QString& path);
    // Closes the document at tab position `index`. With unsaved changes it asks whether to save
    // them first; false means the user cancelled. Closing the last document opens an untitled one.
    bool close_document(std::size_t index);

    EditorView* view() {
        return view_;
    }
    QTabBar* tab_bar() {
        return tabs_;
    }
    FindBar* find_bar() {
        return find_bar_;
    }
    // The parts of the window: the editor area with the tabs and the editor, and the empty
    // activity bar, sidebar and panel around it.
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
    const core::Uri& current_uri() const {
        return documents_[current_].uri;
    }
    const std::shared_ptr<core::Document>& current_document() const {
        return documents_[current_].document;
    }
    std::size_t open_document_count() const {
        return documents_.size();
    }

protected:
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    struct OpenDocument {
        core::Uri uri;
        std::shared_ptr<core::Document> document;
    };

    void register_commands();
    // Lets extensions execute the registered commands, once the extension host is running.
    void share_commands_with_extensions();
    void build_menus();
    void apply_settings();
    void add_untitled_document();
    // Makes the document the shown one, adding it to the open documents when it is new.
    void show_document(const core::Uri& uri, std::shared_ptr<core::Document> document);
    void activate(std::size_t index);
    void move_document(int from, int to);
    std::optional<std::size_t> index_of(const core::Uri& uri) const;
    void update_tab(std::size_t index);
    // Refreshes the tab of the document at `index`, and the window title when it is shown.
    void update_document(std::size_t index);
    void update_title();
    void update_status();
    void on_text_changed(const core::DocumentChange& change);
    void on_selections_changed();
    void save_as_dialog();
    void open_dialog();
    // Shows the command palette, unless a dialog is open.
    void show_command_palette();
    // Runs the command the user chose in the palette, found by the id captured when it was chosen.
    void run_palette_command(const std::string& id);
    // Runs a command by id, in the extension host when it is running, with a message when it fails.
    void execute_command(const std::string& id);
    // The document at `index` is saved whichever one is shown, because an extension can change the
    // shown document while a dialog is open.
    bool save_document(std::size_t index);
    bool save_document_as(std::size_t index, const QString& path);
    bool write_document(std::size_t index, const std::filesystem::path& path);
    // Shows the document and, when it has unsaved changes, asks whether to save them. False means
    // the user cancelled or saving failed.
    bool confirm_close(std::size_t index);
    bool resolve_unsaved_changes();

    Options options_;
    WorkbenchColors colors_;
    QTabBar* tabs_ = nullptr;
    EditorView* view_ = nullptr;
    FindBar* find_bar_ = nullptr;
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
    ThemeStore* themes_ = nullptr;
    CommandRegistry commands_;
    ExtensionHostBridge bridge_;
    // Parallel to the tabs: documents_[i] is shown by tab i.
    std::vector<OpenDocument> documents_;
    std::size_t current_ = 0;
    std::size_t next_untitled_ = 1;
    StatusBar* status_bar_ = nullptr;
    QMenu* window_menu_ = nullptr;
};

} // namespace anvil::ui
