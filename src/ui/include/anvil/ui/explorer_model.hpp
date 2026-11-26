#pragma once

#include "anvil/core/file_tree.hpp"

#include <QAbstractItemModel>
#include <QTimer>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <stop_token>
#include <unordered_map>
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

// Runs the listing of a directory away from the UI thread.
class ListingRunner {
public:
    using Task = std::function<std::optional<core::FileTreeListing>(std::stop_token)>;
    using Done = std::function<void(std::optional<core::FileTreeListing>)>;

    virtual ~ListingRunner() = default;
    // Runs `task`, which looks at `stop` between its steps, and then calls `done` with its result
    // on the thread that called `run`, from that thread's event loop. A runner may call `done`
    // before `run` returns; the model then waits for its own event loop before it uses the result.
    virtual void run(std::stop_token stop, Task task, Done done) = 0;
};

// A thread for each listing, which gives the result back through the event loop of `context`; what
// has not been given back is dropped with the context.
std::unique_ptr<ListingRunner> thread_listing_runner(QObject* context);

// Lists on the calling thread and calls `done` before `run` returns.
std::unique_ptr<ListingRunner> immediate_listing_runner();

// The entries of a folder as a tree for a view: the model that a `QTreeView` shows in the
// explorer. It adapts `core::FileTree` and does not use `QFileSystemModel`, so that a file system
// that is not the local one can plug in later. A directory is read when the view first expands it
// (`canFetchMore`/`fetchMore`), never before, and the folder itself is read when it is set.
//
// A directory is read on a worker thread (`ListingRunner`): until the entries arrive it has one
// row, "Loading…", and then the entries are inserted `batch_size` at a time, each batch in an
// event of its own so that the window stays responsive for a directory of tens of thousands of
// entries. Collapsing the directory (`cancel_loading`), closing the folder or removing the row
// stops the listing, and what arrives later is dropped.
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
        // Whether the row is the "Loading…" row of a directory that is being read, as a bool.
        PlaceholderRole,
        // Whether the row is the row of a new file or folder that is being named, as a bool.
        PendingRole,
    };

    // The entries that are inserted in one step.
    static constexpr int batch_size = 500;

    explicit ExplorerModel(core::DirectoryLister lister = platform_directory_lister(),
                           DirectoryWatchFactory watch = platform_directory_watch(),
                           std::unique_ptr<ListingRunner> runner = nullptr,
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
    // Whether a directory is being read or its entries are being inserted.
    bool loading() const {
        return !loads_.empty();
    }
    // Whether `directory` is being read or its entries are being inserted.
    bool is_loading(const QModelIndex& directory) const;
    // Stops reading `directory` and removes the rows that it has, so that expanding it reads it
    // again. Does nothing for a directory that is read, or that was never expanded.
    void cancel_loading(const QModelIndex& directory);

    // Whether the folder is watched. False without a folder, when the watch could not start, and on
    // platforms that cannot watch.
    bool watching() const {
        return watch_ != nullptr;
    }

    // Reads `directory` again now, when it has rows, instead of when the watch reports a change;
    // for the operations that the explorer makes itself.
    void reload(const std::filesystem::path& directory);

    // Whether all the entries of `directory`, which is invalid for the folder itself, are in the
    // model, so that a new entry can be added to it.
    bool is_read(const QModelIndex& directory) const;
    // Adds the first row of `directory` for a file, or a folder, that is being named. The row has
    // an empty name and no path, is editable and is not an entry of the disk: what reads the
    // directory again leaves it where it is. The directory must be read (`is_read`). Replaces the
    // pending row that there was, and returns the index of the new one.
    QModelIndex add_pending_entry(const QModelIndex& directory, bool folder);
    void remove_pending_entry();
    // The pending row, or an invalid index.
    QModelIndex pending_entry() const;

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
    // All the entries of `directory`, which is invalid for the folder itself, have been inserted.
    void directory_read(const QModelIndex& directory);
    // The watch could not start or lost events, as a sentence for the status bar. After lost
    // events the open directories were read again.
    void watch_problem(const QString& message);

private:
    struct Node;
    // How far a directory is: `unread` has no rows, `loading` has the "Loading…" row, `filling` has
    // some of its entries and `read` all of them.
    enum class State { unread, loading, filling, read };
    struct Load {
        Node* node = nullptr;
        std::stop_source stop;
    };

    Node* node_of(const QModelIndex& index) const;
    QModelIndex index_of_node(const Node* node) const;
    // Starts reading `directory`: the watch, the "Loading…" row and the worker.
    void begin_load(Node& directory);
    // The listing of a read has arrived: the "Loading…" row goes and the first batch comes.
    void finish_load(std::uint64_t id, std::optional<core::FileTreeListing> listing);
    // Inserts the next batch of the entries of the directory that load `id` reads, and posts the
    // one after it, or ends the load.
    void insert_batch(std::uint64_t id);
    // Returns a directory that is loading to unread, without rows.
    void cancel_node(Node& directory);
    // Ends the load of `node` and the loads of the directories below it, whose rows are about to
    // go.
    void drop_loads_below(Node& node);
    void cancel_all();
    // Makes the children of an open `directory` the entries of the file tree again: rows of
    // entries that are gone or changed are removed and rows of new ones inserted.
    void synchronize(Node& directory);
    // Reads `directory` and its open subdirectories again; one that is loading is read again when
    // it has loaded.
    void refresh_open_directories(Node& directory);
    void refresh(Node& directory);
    // The node of a directory that has rows (or is loading them), or null.
    Node* find_directory(const std::filesystem::path& path) const;
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
    std::unique_ptr<ListingRunner> runner_;
    // The directories that are loading, by the number of the load; a number that is not here was
    // cancelled, and what arrives for it is dropped.
    std::unordered_map<std::uint64_t, Load> loads_;
    std::uint64_t next_load_ = 1;
    // While `begin_load` is in the call of the runner.
    bool starting_ = false;
    std::unique_ptr<Node> root_;
    // The row of a new entry that is being named, which belongs to the children of its parent.
    Node* pending_ = nullptr;
};

} // namespace anvil::ui
