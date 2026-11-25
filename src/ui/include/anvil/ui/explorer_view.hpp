#pragma once

#include "anvil/core/file_tree.hpp"
#include "anvil/ui/explorer_model.hpp"
#include "anvil/ui/workbench_colors.hpp"

#include <QString>
#include <QWidget>
#include <filesystem>
#include <optional>

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
class ExplorerView : public QWidget {
    Q_OBJECT

public:
    ExplorerView(const WorkbenchColors& colors, QWidget* parent = nullptr,
                 core::DirectoryLister lister = platform_directory_lister());
    ~ExplorerView() override;

    // Shows the folder with `name` above it, or the Open Folder offer for none.
    void set_workspace(std::optional<std::filesystem::path> folder, const QString& name);
    // Applies the patterns of `files.exclude`; what is open stays open.
    void set_excludes(core::FileExcludes excludes);

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
    // A file was activated with Enter or a double click.
    void file_activated(const QString& path);
    // The Open Folder button was pressed.
    void open_folder_requested();

private:
    class Header;

    void apply_colors();

    const WorkbenchColors& colors_;
    ExplorerModel* model_ = nullptr;
    QStackedLayout* pages_ = nullptr;
    Header* header_ = nullptr;
    QTreeView* tree_ = nullptr;
    QLabel* message_ = nullptr;
    QPushButton* open_folder_ = nullptr;
};

} // namespace anvil::ui
