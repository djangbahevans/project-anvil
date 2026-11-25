#pragma once

#include "anvil/core/file_tree.hpp"
#include "anvil/ui/explorer_model.hpp"
#include "anvil/ui/workbench_colors.hpp"
#include "anvil/ui/zoom.hpp"

#include <QPersistentModelIndex>
#include <QPointer>
#include <QString>
#include <QWidget>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <vector>

class QVBoxLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedLayout;
class QTreeView;

namespace anvil::ui {

// What the explorer asks of the system when it deletes: the parts that tests replace, so that they
// touch neither the user's trash nor a dialog.
struct ExplorerActions {
    // Moves the path to the trash. False when that failed, or when there is no trash.
    std::function<bool(const std::filesystem::path&)> move_to_trash;
    // Asks the user to confirm a delete; true to go on. The dialog is modal, so the answer comes
    // after a nested event loop in which anything can change.
    std::function<bool(QWidget* parent, const QString& message, const QString& detail,
                       const QString& button)>
        confirm;
};

// `QFile::moveToTrash` and a message box.
ExplorerActions default_explorer_actions();

// The explorer: the workspace folder as a tree, with the folders first and the files below, and
// folders that open with a click on their twistie, Enter, the right arrow or a double click. A
// file opens on Enter or a double click, never on a single click. With no folder open it offers an
// Open Folder button instead.
//
// The tree is a `QTreeView` over an `ExplorerModel`, drawn with the colors of the theme (`list.*`).
// A directory is read on a worker thread: while it is, it shows a "Loading…" row, and collapsing it
// stops the reading (see `ExplorerModel`).
// It follows the disk: files and folders that other programs create, delete and rename in the
// directories that are open appear and disappear, and the rest of the tree and the selection stay.
// A selected row that disappears moves the selection to the row that took its place.
//
// It creates, renames and deletes files and folders: a new file or folder is a row with a field for
// its name at the top of the folder (`new_file`, `new_folder`), a rename edits the name in its row
// (`rename_selected`), and a delete asks first, unless `set_confirm_delete(false)`, and moves the
// items to the trash (`trash_selected`) or removes them for good (`delete_selected`). Enter in the
// field accepts the name and Escape drops it. A name that is empty, starts with a slash, has a
// separator or is taken is refused with the reason under the field, and nothing changes. The
// signals tell the window what happened, so that it can keep its documents in step.
class ExplorerView : public QWidget {
    Q_OBJECT

public:
    ExplorerView(const WorkbenchColors& colors, QWidget* parent = nullptr,
                 core::DirectoryLister lister = platform_directory_lister(),
                 DirectoryWatchFactory watch = platform_directory_watch(),
                 std::unique_ptr<ListingRunner> runner = nullptr,
                 ExplorerActions actions = default_explorer_actions());
    ~ExplorerView() override;

    // Shows the folder with `name` above it, or the Open Folder offer for none.
    void set_workspace(std::optional<std::filesystem::path> folder, const QString& name);
    // Applies the patterns of `files.exclude`; what is open stays open.
    void set_excludes(core::FileExcludes excludes);

    // The zoom that the sizes of the view (rows, indentation, twisties, paddings, font) are read
    // through, which must outlive the view; the view follows it when it changes. Null means level
    // 0.
    void set_zoom(const Zoom* zoom);

    // Asks before a delete, as `explorer.confirmDelete` says; on by default.
    void set_confirm_delete(bool confirm) {
        confirm_delete_ = confirm;
    }
    // Whether `moveFileToTrash` has a trash to go to, as `files.enableTrash` says; on by default.
    // Without one it deletes for good.
    void set_trash_enabled(bool enabled) {
        trash_enabled_ = enabled;
    }
    bool trash_enabled() const {
        return trash_enabled_;
    }

    // The commands. A new file or folder goes into the selected folder, into the folder of the
    // selected file, or into the folder of the workspace when nothing is selected. A directory
    // that has not been read is expanded first.
    void new_file();
    void new_folder();
    // Edits the name of the selected item. Nothing happens without one.
    void rename_selected();
    // Delete and Shift+Delete on the selected items.
    void trash_selected();
    void delete_selected();

    // The paths of the selected rows, which are the ones that a delete acts on.
    std::vector<std::filesystem::path> selected_paths() const;
    // Whether the selected row is a directory; true for none, which stands for the folder.
    bool selection_is_directory() const;
    // Whether the field for a name is open.
    bool is_editing() const;
    // The field for a name, or null.
    QLineEdit* name_editor() const;
    // What is wrong with the name in the field, as shown under it; empty when nothing is.
    QString name_problem() const;

    QTreeView* tree() const {
        return tree_;
    }
    ExplorerModel* model() const {
        return model_;
    }
    // Whether a folder is shown, and not the offer to open one.
    bool shows_folder() const;
    // The name above the tree, as the workspace gave it.
    QString folder_name() const;
    QLabel* message() const {
        return message_;
    }
    QPushButton* open_folder_button() const {
        return open_folder_;
    }

signals:
    // A file or folder was made; the window opens a file.
    void created(const QString& path, bool directory);
    // An item was renamed, or a folder with its content.
    void renamed(const QString& from, const QString& to);
    // These items were deleted, or moved to the trash.
    void deleted(const std::vector<std::filesystem::path>& paths);
    // An operation failed: a sentence for the status bar.
    void operation_failed(const QString& message);
    // The user asked for the context menu at the position, in global coordinates. The selection is
    // the row under it, or none.
    void context_menu_requested(const QPoint& global_position);
    // The watch of the folder could not start or lost events: a sentence for the status bar.
    void watch_problem(const QString& message);
    // A file was activated with Enter or a double click.
    void file_activated(const QString& path);
    // The Open Folder button was pressed.
    void open_folder_requested();

private:
    class Header;
    class NameEditor;
    enum class Edit { create_file, create_folder, rename };
    struct Editing {
        Edit kind = Edit::create_file;
        // The folder that the item is in, and for a rename the path of the item.
        std::filesystem::path directory;
        std::filesystem::path original;
        QPersistentModelIndex row;
    };
    struct NewRequest {
        bool folder = false;
        // The folder of the workspace, which has no index of its own.
        bool root = false;
        QPersistentModelIndex directory;
    };

    void apply_colors();
    void apply_zoom();
    const Zoom* zoom_ = nullptr;
    int base_font_pixels_ = 0;
    QVBoxLayout* empty_layout_ = nullptr;

    void begin_new(bool folder);
    // Opens the field when the folder of a new item has been read.
    void continue_new();
    void begin_editing(Edit kind, const QModelIndex& row, std::filesystem::path directory,
                       std::filesystem::path original);
    QWidget* make_name_editor(QWidget* parent, const QModelIndex& row);
    QString problem_with(const QString& name) const;
    void accept_name(const QString& name);
    // Closes the field and drops the row of a new item.
    void finish_editing();
    void select_path(const std::filesystem::path& path);
    void delete_items(bool permanently);

    const WorkbenchColors& colors_;
    ExplorerModel* model_ = nullptr;
    QStackedLayout* pages_ = nullptr;
    Header* header_ = nullptr;
    QTreeView* tree_ = nullptr;
    QLabel* message_ = nullptr;
    QPushButton* open_folder_ = nullptr;
    ExplorerActions actions_;
    bool confirm_delete_ = true;
    bool trash_enabled_ = true;
    std::optional<Editing> editing_;
    std::optional<NewRequest> new_request_;
    QPointer<NameEditor> editor_;
    // Tells the fields apart, so that one that is going away does not end the next.
    std::uint64_t edit_serial_ = 0;
    // The children are deleted after the destructor body, the field among them.
    bool destroying_ = false;
};

} // namespace anvil::ui
