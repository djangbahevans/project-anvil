#pragma once

#include "anvil/core/file_tree.hpp"

#include <QAbstractItemModel>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace anvil::ui {

// Lists a directory with `platform::list_directory`: a name, whether it is a directory (a symbolic
// link to one is) and whether it is a symbolic link.
core::DirectoryLister platform_directory_lister();

// The entries of a folder as a tree for a view: the model that a `QTreeView` shows in the
// explorer. It adapts `core::FileTree` and does not use `QFileSystemModel`, so that a file system
// that is not the local one can plug in later. A directory is read when the view first expands it
// (`canFetchMore`/`fetchMore`), never before, and the folder itself is read when it is set.
class ExplorerModel : public QAbstractItemModel {
    Q_OBJECT

public:
    enum Role {
        // Whether the entry is a directory, as a bool.
        IsDirectoryRole = Qt::UserRole + 1,
        // The path of the entry as text with the separators of the platform.
        PathRole,
        // Why a directory could not be read; a null string for any other entry.
        ErrorRole,
    };

    explicit ExplorerModel(core::DirectoryLister lister = platform_directory_lister(),
                           QObject* parent = nullptr);
    ~ExplorerModel() override;

    // Shows the entries of `folder`; nothing for no folder. The entries that the view had
    // expanded are forgotten.
    void set_folder(std::optional<std::filesystem::path> folder);
    const std::optional<std::filesystem::path>& folder() const {
        return folder_;
    }

    // Applies other `files.exclude` patterns. The directories that are open are read again and
    // only the rows that appear or disappear change, so that what the view has expanded and
    // selected stays.
    void set_excludes(core::FileExcludes excludes);

    std::filesystem::path path_of(const QModelIndex& index) const;
    bool is_directory(const QModelIndex& index) const;
    // The row of a path whose parent directories have been read; invalid for any other.
    QModelIndex index_of(const std::filesystem::path& path) const;

    QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    bool hasChildren(const QModelIndex& parent = {}) const override;
    bool canFetchMore(const QModelIndex& parent) const override;
    void fetchMore(const QModelIndex& parent) override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;

private:
    struct Node;

    Node* node_of(const QModelIndex& index) const;
    QModelIndex index_of_node(const Node* node) const;
    // Makes the children of `directory` the entries of the file tree, with the rows inserted, and
    // marks it read.
    void fill(Node& directory);
    // Makes the children of an open `directory` the entries of the file tree again: rows of
    // entries that are gone or changed are removed and rows of new ones inserted.
    void synchronize(Node& directory);
    void refresh_open_directories(Node& directory);

    core::DirectoryLister lister_;
    core::FileExcludes excludes_ = core::FileExcludes::defaults();
    std::optional<std::filesystem::path> folder_;
    std::unique_ptr<core::FileTree> tree_;
    std::unique_ptr<Node> root_;
};

} // namespace anvil::ui
