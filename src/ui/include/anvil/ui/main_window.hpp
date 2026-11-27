#pragma once

#include "anvil/core/document.hpp"
#include "anvil/core/error.hpp"
#include "anvil/core/language_comments.hpp"
#include "anvil/core/language_registry.hpp"
#include "anvil/core/log.hpp"
#include "anvil/core/uri.hpp"
#include "anvil/core/workspace.hpp"
#include "anvil/platform/process.hpp"
#include "anvil/ui/about.hpp"
#include "anvil/ui/command_registry.hpp"
#include "anvil/ui/editor_area.hpp"
#include "anvil/ui/editor_view.hpp"
#include "anvil/ui/explorer_view.hpp"
#include "anvil/ui/extension_host_bridge.hpp"
#include "anvil/ui/find_bar.hpp"
#include "anvil/ui/highlighting.hpp"
#include "anvil/ui/keybinding_service.hpp"
#include "anvil/ui/keybindings_store.hpp"
#include "anvil/ui/panel.hpp"
#include "anvil/ui/quick_open.hpp"
#include "anvil/ui/quick_pick.hpp"
#include "anvil/ui/settings_store.hpp"
#include "anvil/ui/sidebar.hpp"
#include "anvil/ui/status_bar.hpp"
#include "anvil/ui/tab_strip.hpp"
#include "anvil/ui/theme_store.hpp"
#include "anvil/ui/title_bar.hpp"
#include "anvil/ui/view_container_registry.hpp"
#include "anvil/ui/window_frame.hpp"
#include "anvil/ui/window_state_keeper.hpp"
#include "anvil/ui/workbench.hpp"
#include "anvil/ui/workbench_colors.hpp"
#include "anvil/ui/zoom.hpp"
#include "anvil/workbench/closed_editors.hpp"
#include "anvil/workbench/editor_layout.hpp"
#include "anvil/workbench/menu_model.hpp"
#include "anvil/workbench/navigation_history.hpp"

#include <QMainWindow>
#include <QPoint>
#include <QPointer>
#include <QTimer>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

class QMenu;
class QMessageBox;
class QUrl;
class QMenuBar;

namespace anvil::ui {

class MenuStyle;

struct MainWindowOptions {
    // The extension host to run; none runs the editor without extension support.
    std::optional<ExtensionHostBridge::Config> extension_host;
    // The folder the window works on: its name is in the window title, and the extension host
    // reports it as the workspace folder.
    std::optional<core::Workspace> workspace{};
    // Asks which folder Open Folder opens; none means the question was cancelled. The window shows
    // a folder dialog when unset.
    std::function<std::optional<std::filesystem::path>()> choose_folder;
    // Starts the watch of the Explorer's folder; the platform's watcher when unset. Tests replace
    // it to see how the window reports a watch that fails.
    std::optional<DirectoryWatchFactory> explorer_watch;
    // The trash and the question before a delete in the Explorer; the system's when unset. Tests
    // replace them so that they touch neither the user's trash nor a dialog.
    std::optional<ExplorerActions> explorer_actions;
    // Receives the extension host's messages; discarded when unset.
    std::shared_ptr<core::Logger> logger;
    // The user settings file, read at startup and whenever it changes. A missing file means the
    // defaults; empty means no settings file at all.
    std::filesystem::path settings_file;
    // The user key bindings file, read at startup and whenever it changes. A missing file means
    // the default bindings alone; empty means no such file at all.
    std::filesystem::path keybindings_file;
    // Whether the folder of the window closed last is opened where `workspace` is unset, if
    // `window.restoreWindows` says so.
    bool reopen_folder = false;
    // Whether files are opened at the start, for which only `preserve` reopens the folder and the
    // editors.
    bool opens_files = false;
    // Whether an empty window was asked for: without a folder it restores no editors.
    bool empty_window = false;
    // The directory with what a window starts with again (ADR 0021): its layout, size and
    // position. Empty keeps them for the life of the window only.
    std::filesystem::path state_directory;
    // The ripgrep that lists the files for Go to File; none when there is no ripgrep to be found.
    std::optional<std::filesystem::path> ripgrep;
    // How Go to File lists a folder; ripgrep when unset. Tests replace it to decide which files
    // there are and when they arrive.
    std::function<FileSourceFactory(const std::filesystem::path& folder,
                                    const core::FileSearchOptions& options)>
        list_files;
    // The directory with the color themes; the application's own when unset.
    std::optional<std::filesystem::path> themes_directory;
    // Whether closing asks what to do with unsaved documents.
    bool confirm_unsaved_changes = true;
    // The program that File > New Window starts, with `--new-window`; the running program when
    // empty.
    std::filesystem::path executable;
    // Starts that program; the platform's start of a process that outlives this one when unset.
    // Tests replace it to see what would be started.
    std::function<std::expected<std::int64_t, Error>(const platform::ProcessSpec&)> start_process;
    // Opens an address in the system's browser and tells whether that worked; the platform's
    // way of opening it when unset. Tests replace it to see what would be opened.
    std::function<bool(const QUrl&)> open_url;
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
    // Saves every document with unsaved changes, in the order of the tabs of the groups: one with
    // a file is written, an untitled one asks for a name. Stops when the user cancels that
    // question, and false means that it did, or that a document could not be written. The
    // documents are found again by identity after each question, because extensions can change
    // the tabs meanwhile.
    bool save_all();
    // Reads the file of the shown document again and replaces its text with it, as one edit that
    // can be undone, leaving the document without unsaved changes. Each selection keeps its line
    // and column, clamped to the new text. False for an untitled document, and when the file
    // cannot be read, which the status bar says.
    bool revert();
    // Closes the document at tab position `index` of the focused group. With unsaved changes it
    // asks whether to save them first; false means the user cancelled. Closing the last document
    // opens an untitled one.
    bool close_document(std::size_t index);

    // The folder the window works on, if any.
    const std::optional<core::Workspace>& workspace() const {
        return options_.workspace;
    }
    // The comment tokens of a language: those of VS Code's built-in languages, with the tokens that
    // the language configuration files of extensions set laid over them. None for a language that
    // has no tokens.
    std::optional<core::LanguageComments> language_comments(std::string_view language_id) const;
    // Makes the folder the workspace: the extension host restarts with it (ADR 0016), and the
    // editors are switched for those of the folder (ADR 0022), after asking about unsaved
    // documents. Fails when the user cancels there, and, with a message in the status bar, when
    // the path is not a folder. Opening the folder that is the workspace already does nothing.
    bool open_folder(const std::filesystem::path& folder);
    // Leaves the workspace as `open_folder` does; the extension host restarts without one.
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
    // Whether Zen Mode is on.
    bool zen_mode_active() const {
        return zen_.active;
    }
    EditorArea* editor_area() {
        return area_;
    }
    // The context menu of the explorer for the row that is selected, which the explorer shows when
    // the user asks for it. Its actions carry the id of their command in their data.
    std::unique_ptr<QMenu> explorer_context_menu();
    workbench::EditorLayout& editor_layout() {
        return layout_;
    }
    const workbench::NavigationHistory& navigation_history() const {
        return history_;
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
    QuickOpen* quick_open() {
        return quick_open_.get();
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
    // The zoom of the window: the level of `window.zoomLevel` as the window applies it.
    const Zoom& zoom() const {
        return zoom_;
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
    // The syntax highlighting of the documents.
    Highlighting& highlighting() {
        return *highlighting_;
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
    EditorArea::Wrapping wrapping_of(const std::shared_ptr<core::Document>& document) const;
    // View > Word Wrap: wraps the shown document when it does not wrap and the reverse, or removes
    // the override that an earlier use made.
    void toggle_word_wrap();
    // Fills `menu` with `items`. An item runs its command and shows its key binding, and is
    // disabled while no command has its id.
    void populate_menu(QMenu* menu, const std::vector<workbench::MenuItem>& items);
    // Gives a menu, or a submenu, the style that draws the menus of VS Code.
    void style_menu(QMenu* menu);
    // The font of the menus: the system's, scaled by the zoom.
    QFont menu_font() const;
    // Lets extensions execute the registered commands, once the extension host is running.
    void share_commands_with_extensions();
    // Applies `options_.workspace`: restarts the extension host with it and gives it what the
    // window holds (documents, commands, contributions), then shows the folder's name.
    void workspace_changed();
    void apply_extension_host_options();
    // The registry of the languages that files can have: VS Code's built-in ones, then those of the
    // extensions, with the user's associations. Rebuilt when the extension host starts, and the
    // documents take the languages the new registry gives.
    void rebuild_languages();
    // Reads `files.associations` and, when it changed, applies it to the registry.
    void apply_file_associations();
    // Registers the explorer as the first container of the sidebar.
    void register_explorer();
    // Gives the explorer the folder of the workspace, with the folders that were expanded in it.
    void show_workspace_in(ExplorerView& explorer);
    // Gives the explorer `explorer.confirmDelete`, `explorer.confirmDragAndDrop` and
    // `files.enableTrash`.
    void apply_explorer_settings();
    // Shows the explorer, for a command that works on it, and returns it.
    ExplorerView* show_explorer();
    // The items of the explorer's context menu, for the row that is selected. The menu runs the
    // commands of the explorer.
    std::unique_ptr<QMenu> build_explorer_menu();
    // Opens the file manager on the folder of a file or folder: the selected row of the explorer
    // when it has the focus, otherwise the file of the active editor.
    void reveal_in_file_manager();
    void reveal_path(const std::filesystem::path& path);
    // The explorer made, renamed or deleted files: the documents of the files follow, in the tabs
    // and for extensions.
    void explorer_renamed(const std::filesystem::path& from, const std::filesystem::path& to);
    void explorer_deleted(const std::vector<std::filesystem::path>& paths);
    // Whether a document with unsaved changes is open at or below the path.
    bool has_unsaved_changes_within(const std::filesystem::path& path) const;
    // Reads `files.exclude` and, when it changed, gives it to the explorer.
    void apply_file_excludes();
    // Reads the settings of file search, and logs what is wrong with those of its own.
    void apply_search_settings();
    // Go to File: opens the quick pick over the files of the workspace folder, or over the open
    // documents without one. Run again while it is open, it chooses the next row.
    void show_quick_open();
    // Quick access is one popup, as in VS Code, whose use the first character of the text says:
    // `>` the command palette, `:` Go to Line/Column and anything else a search for files.
    void show_files(const QString& filter);
    void follow_quick_access_text(const QString& text);
    // Ends what the popup shows when another use takes it over: a list of `pick_from` is
    // dismissed, and Go to Line/Column puts the caret back.
    void dismiss_pick();
    // The documents that the window has shown, in the form that quick open offers them.
    std::vector<QuickOpenEntry> recent_entries(bool open_only);
    QuickOpenEntry quick_open_entry(const core::Uri& uri);
    void open_chosen(const core::Uri& uri);
    // Puts the document first among those that were shown.
    void remember_shown(const core::Uri& uri);
    // Builds the comment tokens of the registered languages from the built-in table and from the
    // language configuration files that extensions contribute.
    void rebuild_language_comments();
    // The language id of a resource whose text starts with `first_line`.
    std::string resolve_language(const core::Uri& uri, std::string_view first_line) const;
    // The language of a file or document for its icon, as VS Code takes it: that of the open
    // document when it is not plain text, else the one that the path gives.
    std::string language_for_icon(const core::Uri& uri);
    SetiIcon file_icon_of(const core::Uri& uri);
    SetiIcon tab_icon(const core::Uri& uri);
    // The icons of files follow their languages.
    void refresh_file_icons();
    // Gives every document the language the registry decides, except those that an extension asked
    // for, and tells the extension host of the ones that changed.
    void refresh_document_languages();
    struct DocumentLanguage {
        std::string id;
        // What the status bar shows.
        QString name;
        // The icon of the document's tab, which is worked out once: the tab of the document that
        // is typed in is described on every keystroke. A new language or name drops it with the
        // rest.
        std::optional<SetiIcon> icon{};
    };
    const DocumentLanguage& language_of_document(const core::Uri& uri);
    // Lets the user pick the language of the shown document (Change Language Mode).
    void change_language_mode();
    // Asks for the language to associate with the files of an extension, and writes it to
    // `files.associations`; a name that starts with a dot is associated as a whole.
    void pick_association(const std::string& extension, const std::string& base);
    // Gives up the language that the user picked for the document and resolves it again.
    void auto_detect_language(const core::Uri& uri);
    // Gives the document the language that the user picked, until it is closed.
    void choose_language(const core::Uri& uri, const std::string& id);
    // The highlighter for the views of the document, null when its language has none.
    std::shared_ptr<const core::Highlighter> highlighter_of(const core::Uri& uri);
    // Tells the highlighting that the text of the document changed, and gives the views no
    // highlighter when the document has outgrown the limit for highlighting.
    void highlight_edit(const core::Uri& uri, const core::DocumentChange& change);
    // Forgets what the window knows of the language of a document that leaves the window, and
    // stops its highlighting.
    void forget_document_language(const core::Uri& uri);
    // What a pick can do besides choosing: start with an item as the chosen row, hear of each item
    // the chosen row moves to, and hear that the pick was closed without a choice.
    struct PickHooks {
        std::optional<std::size_t> active;
        std::function<void(std::size_t)> highlighted;
        std::function<void()> dismissed;
    };
    // Shows `items` in the quick pick; `chosen` gets the position of the one the user picks.
    void pick_from(const QString& placeholder, std::vector<QuickPickItem> items,
                   std::function<void(std::size_t)> chosen, PickHooks hooks = {});
    // Opens the quick pick of color themes, which shows each one as the chosen row moves and keeps
    // the one that is chosen in the settings.
    void select_color_theme();
    // What a pick of a language shows, as VS Code's does: the registered languages that have a
    // name, sorted by name, each with its id in parentheses after the name and `current` marked as
    // the configured language.
    std::vector<QuickPickItem> language_picks(const std::string& current,
                                              std::vector<std::string>& ids) const;
    DocumentLanguage describe_language(const std::string& id) const;
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
    // Shows the editors saved for the workspace, with their documents read from disk; false when
    // there is none to show. The window must have no editor.
    bool restore_editors();
    // The editor groups with the tabs that a restore can bring back: those of files.
    workbench::EditorLayout saved_editors() const;
    // Closes every editor without asking and shows those of the workspace that was entered.
    void switch_editors();
    // Adds an untitled document and shows it in `group`, or in the focused group.
    void add_untitled_document(std::optional<workbench::GroupId> group = std::nullopt,
                               core::Text text = {});
    // Opens a new untitled document with the key table as it is now: the default bindings with
    // the ones of extensions and of the user's file applied.
    void show_keybindings_reference();
    // Makes the document the shown one of the focused group, or of the group that has it already,
    // adding it to the open documents when it is new.
    void show_document(const core::Uri& uri, std::shared_ptr<core::Document> document);
    void activate_tab(workbench::GroupId group, std::size_t index);
    // Moves a tab, and the places of its document in the navigation history with it.
    bool move_tab(workbench::GroupId from, std::size_t index, workbench::GroupId to,
                  std::size_t to_index);
    void focus_current_editor();
    // Focuses the nth group (counting from 0) in the order they are shown; the group after the
    // last one is made by splitting it, and a further one does nothing.
    void focus_group_number(std::size_t index);
    // Gives the keyboard focus to the editor of the shown group after a command moved it.
    void show_current_after(bool changed);
    void open_find_bar(bool with_replace);
    const core::Uri* current_uri_ptr() const;
    std::optional<core::Uri> uri_of(const std::shared_ptr<core::Document>& document) const;
    TabStrip::Tab tab_info(const core::Uri& uri);
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
    // The primary selection of the document, or nothing when it is not open.
    std::optional<workbench::NavigationSelection> selection_of(const core::Uri& uri) const;
    // Tells the navigation history where the user is, unless the window is walking it.
    void record_activation();
    void record_selection(workbench::GroupId group, workbench::SelectionReason reason);
    // Drops the places of groups that are gone and of documents that cannot be opened again.
    void prune_navigation_history();
    // Shows the place, opening its document again when it was closed. Returns false, and forgets
    // the places of the document, when the document cannot be opened.
    bool show_place(const workbench::NavigationEntry& place);
    void navigate(std::optional<workbench::NavigationEntry> place, bool to_edit);
    // Remembers the editor of the tab, which is about to close, for Reopen Closed Editor. An
    // untitled document with no text is not remembered.
    void record_closed(workbench::GroupId group, std::size_t index, const core::Uri& uri);
    // Opens the editors that were closed last, together, again where they were; when none of them
    // can be opened, the ones before. A document that is open in its group already, and a file that
    // is gone, are skipped.
    void reopen_closed_editor();
    bool reopen(const workbench::ClosedEditor& closed);
    // The document of a file that is open or that an extension has, or one read from the file;
    // empty, with a message, when it cannot be read.
    // The document of the file, shared with extensions that have it open, or why it cannot be
    // read.
    std::expected<std::shared_ptr<core::Document>, QString>
    load_document(const std::filesystem::path& file, const core::Uri& uri);
    std::shared_ptr<core::Document> file_document(const std::filesystem::path& file,
                                                  const core::Uri& uri);
    void save_as_dialog();
    void open_dialog();
    // Starts another editor in a window of its own: the same program, with no arguments, in a
    // directory that exists, with the environment of this one.
    void open_new_window();
    // Makes the folder of the window closed last the workspace, when the options and
    // `window.restoreWindows` ask for it. Before anything reads the workspace.
    void reopen_last_folder(core::Logger& logger);
    // Opens a file of the user's configuration in the editor, creating it with `initial` when it
    // is missing. Says so in the status bar when the window has no such file, or it cannot be
    // created.
    bool open_user_file(const std::filesystem::path& file, std::string_view initial);
    // Shows the command palette, unless a dialog is open.
    void show_command_palette(const QString& text = QStringLiteral(">"));
    // Opens Go to Line/Column over the shown document: the caret follows the text typed, Enter
    // keeps it and Escape puts the caret and the scroll position back.
    void show_go_to_line(const QString& text = QStringLiteral(":"));
    void preview_go_to_line();
    void finish_go_to_line(bool keep);
    // Runs the command the user chose in the palette, found by the id captured when it was chosen.
    void run_palette_command(const std::string& id);
    // Runs a command by id, in the extension host when it is running, with a message when it fails.
    void execute_command(const std::string& id);
    // Documents are found by URI and not by position, because a dialog can be open while an
    // extension changes the tabs.
    bool save_document(const core::Uri& uri);
    bool save_document_as(const core::Uri& uri, const QString& path);
    // Makes the document of `uri` the document of `new_uri`, in the tabs, the languages, the
    // highlighting, the history and, unless `extensions_know`, for extensions.
    void rekey_document(const core::Uri& uri, const core::Uri& new_uri, bool extensions_know);
    // An extension renamed the file of a document, or deleted the file of one without unsaved
    // changes; the extension host has been told. A renamed document keeps its tabs, and a deleted
    // one is closed in every group without asking.
    void document_renamed(const core::Uri& uri, const core::Uri& new_uri);
    void document_deleted(const core::Uri& uri);
    // Drops a document that no tab shows any more, with what the window keeps about it.
    void release_document(const core::Uri& uri);
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
    void toggle_zen_mode();
    // Opens the About dialog, or raises the one that is open. It does not run a nested event loop,
    // so the extension host is served while it is shown.
    void show_about();
    // Writes `window.zoomLevel` one whole level above or below the current one, and does nothing
    // at the end of the range.
    void zoom_by(int step);
    // Opens a page of the project in the browser, or says in the status bar that it cannot.
    void open_project_page(std::string_view address);
    // Runs one of the comment commands on the shown editor, or says in the status bar that the
    // language of the document has no token for it.
    void run_comment_command(bool block, core::LineCommentType type);
    // Shows the status bar, the activity bar and the tabs as the settings say, and as Zen Mode
    // lets them.
    void apply_part_visibility();
    void update_title_bar_tooltips();
    QString title_text() const;

    Options options_;
    WorkbenchColors colors_;
    Zoom zoom_;
    std::unique_ptr<MenuStyle> menu_style_;
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
    std::unique_ptr<QuickOpen> quick_open_;
    // What the popup of the command palette is used for; `other` is a list of `pick_from`.
    enum class QuickAccess { other, commands, files, line };
    QuickAccess quick_access_ = QuickAccess::other;
    core::FileSearchOptions search_options_;
    // What Go to Line/Column puts back when it is cancelled. `document` is empty while it is not
    // in use, and when there is no document to go to a line of.
    struct GoToLineState {
        std::shared_ptr<core::Document> document;
        core::SelectionSet selections;
        QPoint scroll;
    } go_to_line_state_;
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
    AboutVersions about_versions_;
    QPointer<QMessageBox> about_dialog_;
    Highlighting* highlighting_ = nullptr;
    core::LanguageRegistry languages_ = core::LanguageRegistry::with_builtin_languages();
    std::vector<core::FileAssociation> file_associations_;
    std::vector<std::string> file_association_problems_;
    // Null until the explorer is first shown.
    QPointer<ExplorerView> explorer_;
    core::FileExcludes file_excludes_ = core::FileExcludes::defaults();
    std::map<std::string, core::LanguageComments, std::less<>> language_comments_;
    // The problems of language configuration files that were logged, so that a restart of the
    // extension host that reads the same files again does not log them again.
    std::set<std::string> reported_language_configurations_;
    // The language of each open document, filled when it is first asked for so that the status bar
    // does not ask the registry on every keystroke.
    std::map<core::Uri, DocumentLanguage> document_languages_;
    // The languages that the user picked for documents, which the registry does not change.
    std::map<core::Uri, std::string> chosen_languages_;
    // What the quick pick does with the item that is chosen, while it is not the command palette.
    std::function<void(std::size_t)> pick_action_;
    std::function<void(std::size_t)> pick_highlighted_;
    std::function<void()> pick_dismissed_;
    // The open documents, each shown by one or more tabs of the layout.
    std::map<core::Uri, std::shared_ptr<core::Document>> documents_;
    workbench::EditorLayout layout_;
    std::unique_ptr<LayoutWatcher> watcher_;
    EditorArea* area_ = nullptr;
    // The documents that View > Word Wrap overrides `editor.wordWrap` for, and whether each
    // wraps. Nothing of it is saved.
    std::map<std::shared_ptr<core::Document>, bool> wrap_overrides_;
    // The document the extension host was last told is the active editor.
    std::optional<core::Uri> shown_uri_;
    // The documents that the window has shown, the most recently first.
    std::vector<core::Uri> recent_documents_;
    workbench::NavigationHistory history_;
    workbench::ClosedEditors closed_editors_;
    // What Zen Mode changed, to be given back when it ends.
    struct ZenMode {
        bool active = false;
        bool sidebar_was_visible = false;
        bool panel_was_visible = false;
        bool entered_full_screen = false;
        bool entered_centered_layout = false;
    };
    ZenMode zen_;
    std::unique_ptr<workbench::StateStore> state_;
    std::unique_ptr<WindowStateKeeper> state_keeper_;
    // How many operations that close editors are running, so that the closes of a nested one are
    // one batch with the outer one.
    int closing_depth_ = 0;
    class ClosingScope;
    // Set while the window shows a place of the history, which is not a place of its own.
    bool walking_history_ = false;
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
