#pragma once

#include "anvil/core/file_tree.hpp"
#include "anvil/ui/explorer_model.hpp"
#include "anvil/ui/workbench_colors.hpp"
#include "anvil/ui/zoom.hpp"

#include <QString>
#include <QWidget>
#include <filesystem>
#include <optional>

class QVBoxLayout;
class QLabel;
class QPushButton;
class QStackedLayout;
class QTreeView;

namespace anvil::ui {

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
class ExplorerView : public QWidget {
    Q_OBJECT

public:
    ExplorerView(const WorkbenchColors& colors, QWidget* parent = nullptr,
                 core::DirectoryLister lister = platform_directory_lister(),
                 DirectoryWatchFactory watch = platform_directory_watch(),
                 std::unique_ptr<ListingRunner> runner = nullptr);
    ~ExplorerView() override;

    // Shows the folder with `name` above it, or the Open Folder offer for none.
    void set_workspace(std::optional<std::filesystem::path> folder, const QString& name);
    // Applies the patterns of `files.exclude`; what is open stays open.
    void set_excludes(core::FileExcludes excludes);

    // The zoom that the sizes of the view (rows, indentation, twisties, paddings, font) are read
    // through, which must outlive the view; the view follows it when it changes. Null means level
    // 0.
    void set_zoom(const Zoom* zoom);

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
    // The watch of the folder could not start or lost events: a sentence for the status bar.
    void watch_problem(const QString& message);
    // A file was activated with Enter or a double click.
    void file_activated(const QString& path);
    // The Open Folder button was pressed.
    void open_folder_requested();

private:
    class Header;

    void apply_colors();
    void apply_zoom();
    const Zoom* zoom_ = nullptr;
    int base_font_pixels_ = 0;
    QVBoxLayout* empty_layout_ = nullptr;

    const WorkbenchColors& colors_;
    ExplorerModel* model_ = nullptr;
    QStackedLayout* pages_ = nullptr;
    Header* header_ = nullptr;
    QTreeView* tree_ = nullptr;
    QLabel* message_ = nullptr;
    QPushButton* open_folder_ = nullptr;
};

} // namespace anvil::ui
