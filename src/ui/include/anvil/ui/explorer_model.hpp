#pragma once

#include "anvil/core/file_tree.hpp"

#include <QAbstractItemModel>
#include <QTimer>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace anvil::ui {

// Lists a directory with `platform::list_directory`: a name, whether it is a directory (a symbolic
// link to one is) and whether it is a symbolic link.
core::DirectoryLister platform_directory_lister();

// What the explorer needs of a watcher of directories: the set of directories it reports on, and
// the paths of the events since the last call.
class DirectoryWatch {
public:
    virtual ~DirectoryWatch() = default;
    virtual std::expected<void, Error> add(const std::filesystem::path& directory) = 0;
    virtual void remove(const std::filesystem::path& directory) = 0;
    // The paths that were created, changed or deleted in the watched directories. An error means
    // that events were lost.
    virtual std::expected<std::vector<std::filesystem::path>, Error> poll() = 0;
};

// Starts watching `folder`, and nothing below it until directories are added.
using DirectoryWatchFactory = std::function<std::expected<std::unique_ptr<DirectoryWatch>, Error>(
    const std::filesystem::path&)>;

// Watches with `platform::FileWatcher`, not recursive: one watcher, one thread, and one inotify
// watch for each directory that the tree has read.
DirectoryWatchFactory platform_directory_watch();

// The entries of a folder as a tree for a view: the model that a `QTreeView` shows in the
// explorer. It adapts `core::FileTree` and does not use `QFileSystemModel`, so that a file system
// that is not the local one can plug in later. A directory is read when the view first expands it
// (`canFetchMore`/`fetchMore`), never before, and the folder itself is read when it is set.
//
// The directories that were read are watched, and a timer reads again the ones that events touch,
// each once per round, so that the rows follow the disk. A directory that was never read is not
// watched and is not listed.
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
                           DirectoryWatchFactory watch = platform_directory_watch(),
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

    // Takes the events of the watch and reads again each directory that has rows and that an event
    // names, once. The timer calls it; tests call it to run a round at a time.
    void process_file_events();
    // Whether the folder is watched. False without a folder, when the watch could not start, and on
    // platforms that cannot watch.
    bool watching() const {
        return watch_ != nullptr;
    }

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

signals:
    // The watch could not start or lost events, as a sentence for the status bar. After lost
    // events the open directories were read again.
    void watch_problem(const QString& message);

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
    // Reads `directory` and its open subdirectories again.
    void refresh_open_directories(Node& directory);
    void refresh(Node& directory);
    // The node of a directory that has rows, or null.
    Node* open_directory(const std::filesystem::path& path) const;
    void start_watch();
    void watch_directory(const std::filesystem::path& path);
    void unwatch_below(const std::filesystem::path& path);
    void report_once(const QString& message);

    core::DirectoryLister lister_;
    DirectoryWatchFactory watch_factory_;
    std::unique_ptr<DirectoryWatch> watch_;
    // The directories added to the watch, as generic paths.
    std::set<std::string> watched_;
    QTimer* poll_timer_ = nullptr;
    // The last problem that was reported, so that the same one is not said again and again.
    QString reported_problem_;
    core::FileExcludes excludes_ = core::FileExcludes::defaults();
    std::optional<std::filesystem::path> folder_;
    std::unique_ptr<core::FileTree> tree_;
    std::unique_ptr<Node> root_;
};

} // namespace anvil::ui
