#include "anvil/ui/explorer_model.hpp"

#include "anvil/platform/file_system.hpp"
#include "anvil/platform/file_watcher.hpp"

#include <QMetaObject>
#include <QString>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iterator>
#include <set>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace anvil::ui {

struct ExplorerModel::Node {
    Node* parent = nullptr;
    int row = 0;
    core::FileTreeEntry entry;
    std::filesystem::path path;
    State state = State::unread;
    // The load that reads the directory, while it is loading.
    std::uint64_t load_id = 0;
    // An event named the directory while it was loading, so that it is read again afterwards.
    bool dirty = false;
    // The "Loading…" row of a directory that is loading.
    bool placeholder = false;
    // The row of a new entry that is being named, and whether it is for a folder.
    bool pending = false;
    bool pending_folder = false;
    std::vector<std::unique_ptr<Node>> children;
};

core::DirectoryLister platform_directory_lister() {
    return [](const std::filesystem::path& directory)
               -> std::expected<std::vector<core::FileTreeEntry>, Error> {
        auto listed = platform::list_directory(directory);
        if (!listed) {
            return std::unexpected(listed.error());
        }
        std::vector<core::FileTreeEntry> entries;
        entries.reserve(listed->size());
        for (platform::DirectoryEntry& entry : *listed) {
            entries.push_back(
                {.name = std::move(entry.name),
                 .is_directory = (entry.type & platform::file_type::directory) != 0,
                 .is_symbolic_link = (entry.type & platform::file_type::symbolic_link) != 0});
        }
        return entries;
    };
}

namespace {

class PlatformDirectoryWatch : public DirectoryWatch {
public:
    explicit PlatformDirectoryWatch(std::unique_ptr<platform::FileWatcher> watcher)
        : watcher_(std::move(watcher)) {}

    std::expected<void, Error> add(const std::filesystem::path& directory) override {
        return watcher_->add_directory(directory);
    }

    void remove(const std::filesystem::path& directory) override {
        watcher_->remove_directory(directory);
    }

    std::expected<std::vector<std::filesystem::path>, Error> poll() override {
        auto events = watcher_->poll(std::chrono::milliseconds(0));
        if (!events) {
            return std::unexpected(events.error());
        }
        std::vector<std::filesystem::path> paths;
        paths.reserve(events->size());
        for (platform::FileEvent& event : *events) {
            paths.push_back(std::move(event.path));
        }
        return paths;
    }

private:
    std::unique_ptr<platform::FileWatcher> watcher_;
};

} // namespace

DirectoryWatchFactory platform_directory_watch() {
    return [](const std::filesystem::path& folder)
               -> std::expected<std::unique_ptr<DirectoryWatch>, Error> {
        auto started = platform::FileWatcher::start(folder, {.recursive = false});
        if (!started) {
            return std::unexpected(started.error());
        }
        return std::make_unique<PlatformDirectoryWatch>(std::move(*started));
    };
}

namespace {

class ThreadListingRunner : public ListingRunner {
public:
    explicit ThreadListingRunner(QObject* context) : context_(context) {}

    void run(std::stop_token stop, Task task, Done done) override {
        std::erase_if(workers_, [](const Worker& worker) { return worker.finished->load(); });
        auto finished = std::make_shared<std::atomic<bool>>(false);
        auto delivered = std::make_shared<Done>(std::move(done));
        workers_.push_back({std::jthread([context = context_, stop, task = std::move(task),
                                          delivered, finished]() mutable {
                                std::optional<core::FileTreeListing> result = task(stop);
                                QMetaObject::invokeMethod(
                                    context,
                                    [delivered, result = std::move(result)]() mutable {
                                        (*delivered)(std::move(result));
                                    },
                                    Qt::QueuedConnection);
                                finished->store(true);
                            }),
                            finished});
    }

private:
    struct Worker {
        std::jthread thread;
        std::shared_ptr<std::atomic<bool>> finished;
    };

    QObject* context_;
    std::vector<Worker> workers_;
};

class ImmediateListingRunner : public ListingRunner {
public:
    void run(std::stop_token stop, Task task, Done done) override {
        done(task(stop));
    }
};

} // namespace

std::unique_ptr<ListingRunner> thread_listing_runner(QObject* context) {
    return std::make_unique<ThreadListingRunner>(context);
}

std::unique_ptr<ListingRunner> immediate_listing_runner() {
    return std::make_unique<ImmediateListingRunner>();
}

constexpr int poll_interval_ms = 100;

namespace {

QString text_of(const std::filesystem::path& path) {
    return QString::fromStdString(path.string());
}

} // namespace

ExplorerModel::ExplorerModel(core::DirectoryLister lister, DirectoryWatchFactory watch,
                             std::unique_ptr<ListingRunner> runner, QObject* parent)
    : QAbstractItemModel(parent), lister_(std::move(lister)), watch_factory_(std::move(watch)),
      poll_timer_(new QTimer(this)),
      runner_(runner ? std::move(runner) : thread_listing_runner(this)),
      root_(std::make_unique<Node>()) {
    poll_timer_->setInterval(poll_interval_ms);
    connect(poll_timer_, &QTimer::timeout, this, [this] { process_file_events(); });
}

ExplorerModel::~ExplorerModel() {
    cancel_all();
    // Ends the workers before the members that their results would reach.
    runner_.reset();
}

ExplorerModel::Node* ExplorerModel::node_of(const QModelIndex& index) const {
    return index.isValid() ? static_cast<Node*>(index.internalPointer()) : root_.get();
}

QModelIndex ExplorerModel::index_of_node(const Node* node) const {
    if (node == nullptr || node == root_.get()) {
        return {};
    }
    return createIndex(node->row, 0, const_cast<Node*>(node));
}

void ExplorerModel::begin_load(Node& directory) {
    // The watch starts before the listing so that a change in between is not missed.
    watch_directory(directory.path);
    const std::uint64_t id = next_load_++;
    directory.state = State::loading;
    directory.load_id = id;
    directory.dirty = false;

    auto placeholder = std::make_unique<Node>();
    placeholder->parent = &directory;
    placeholder->placeholder = true;
    placeholder->entry.name = tr("Loading…").toStdString();
    const QModelIndex parent = index_of_node(&directory);
    beginInsertRows(parent, 0, 0);
    directory.children.push_back(std::move(placeholder));
    endInsertRows();

    std::stop_source stop;
    loads_.emplace(id, Load{&directory, stop});
    // The view calls `fetchMore` while it lays out an expansion, and it cannot take the rows of the
    // "Loading…" row replaced from inside that call: a result that a runner gives before `run`
    // returns waits for the event loop like one that a worker gives.
    starting_ = true;
    runner_->run(
        stop.get_token(),
        [reader = tree_->reader(), path = directory.path](std::stop_token token) {
            return reader.read(path, token);
        },
        [this, id](std::optional<core::FileTreeListing> listing) {
            if (starting_) {
                QMetaObject::invokeMethod(
                    this,
                    [this, id, listing = std::move(listing)]() mutable {
                        finish_load(id, std::move(listing));
                    },
                    Qt::QueuedConnection);
            } else {
                finish_load(id, std::move(listing));
            }
        });
    starting_ = false;
}

void ExplorerModel::finish_load(std::uint64_t id, std::optional<core::FileTreeListing> listing) {
    const auto found = loads_.find(id);
    if (found == loads_.end()) {
        return;
    }
    Node& directory = *found->second.node;
    if (!listing) {
        cancel_node(directory);
        return;
    }
    const QModelIndex parent = index_of_node(&directory);
    beginRemoveRows(parent, 0, 0);
    directory.children.clear();
    endRemoveRows();
    tree_->adopt(directory.path, std::move(*listing));
    directory.state = State::filling;
    insert_batch(id);
}

void ExplorerModel::insert_batch(std::uint64_t id) {
    const auto found = loads_.find(id);
    if (found == loads_.end()) {
        return;
    }
    Node& directory = *found->second.node;
    const std::vector<core::FileTreeEntry>& entries = tree_->children(directory.path);
    const std::size_t from = directory.children.size();
    const std::size_t to = std::min(from + static_cast<std::size_t>(batch_size), entries.size());
    if (from < to) {
        std::vector<std::unique_ptr<Node>> added;
        added.reserve(to - from);
        for (std::size_t i = from; i < to; ++i) {
            auto node = std::make_unique<Node>();
            node->parent = &directory;
            node->row = static_cast<int>(i);
            node->entry = entries[i];
            node->path = directory.path / entries[i].name;
            added.push_back(std::move(node));
        }
        beginInsertRows(index_of_node(&directory), static_cast<int>(from),
                        static_cast<int>(to) - 1);
        directory.children.insert(directory.children.end(), std::make_move_iterator(added.begin()),
                                  std::make_move_iterator(added.end()));
        endInsertRows();
    }
    if (to < entries.size()) {
        QMetaObject::invokeMethod(this, [this, id] { insert_batch(id); }, Qt::QueuedConnection);
        return;
    }
    directory.state = State::read;
    directory.load_id = 0;
    loads_.erase(found);
    if (directory.dirty) {
        directory.dirty = false;
        refresh(directory);
    }
    emit directory_read(index_of_node(&directory));
}

void ExplorerModel::cancel_node(Node& directory) {
    if (directory.state != State::loading && directory.state != State::filling) {
        return;
    }
    if (const auto found = loads_.find(directory.load_id); found != loads_.end()) {
        found->second.stop.request_stop();
        loads_.erase(found);
    }
    drop_loads_below(directory);
    if (!directory.children.empty()) {
        const QModelIndex parent = index_of_node(&directory);
        beginRemoveRows(parent, 0, static_cast<int>(directory.children.size()) - 1);
        directory.children.clear();
        endRemoveRows();
    }
    if (directory.state == State::filling && tree_) {
        tree_->forget(directory.path);
    }
    unwatch_below(directory.path);
    directory.state = State::unread;
    directory.load_id = 0;
    directory.dirty = false;
}

void ExplorerModel::drop_loads_below(Node& node) {
    if (node.state == State::loading || node.state == State::filling) {
        if (const auto found = loads_.find(node.load_id); found != loads_.end()) {
            found->second.stop.request_stop();
            loads_.erase(found);
        }
    }
    for (const std::unique_ptr<Node>& child : node.children) {
        drop_loads_below(*child);
    }
}

void ExplorerModel::cancel_all() {
    for (auto& [id, load] : loads_) {
        load.stop.request_stop();
    }
    loads_.clear();
}

bool ExplorerModel::is_loading(const QModelIndex& directory) const {
    const Node* node = node_of(directory);
    return node->state == State::loading || node->state == State::filling;
}

void ExplorerModel::cancel_loading(const QModelIndex& directory) {
    if (directory.isValid()) {
        cancel_node(*node_of(directory));
    }
}

void ExplorerModel::set_folder(std::optional<std::filesystem::path> folder) {
    cancel_all();
    beginResetModel();
    pending_ = nullptr;
    folder_ = std::move(folder);
    root_ = std::make_unique<Node>();
    tree_.reset();
    watch_.reset();
    watched_.clear();
    poll_timer_->stop();
    reported_problem_.clear();
    endResetModel();
    if (folder_) {
        tree_ = std::make_unique<core::FileTree>(*folder_, lister_, excludes_);
        root_->entry = {.name = folder_->filename().string(), .is_directory = true};
        root_->path = tree_->root();
        start_watch();
        begin_load(*root_);
    }
}

void ExplorerModel::start_watch() {
    if (!watch_factory_) {
        return;
    }
    auto started = watch_factory_(*folder_);
    if (started) {
        watch_ = std::move(*started);
        poll_timer_->start();
    } else if (started.error().code != ErrorCode::unsupported) {
        report_once(tr("The Explorer cannot follow changes on disk: %1")
                        .arg(QString::fromStdString(started.error().message)));
    }
}

void ExplorerModel::watch_directory(const std::filesystem::path& path) {
    if (!watch_) {
        return;
    }
    const std::string key = path.generic_string();
    if (watched_.contains(key)) {
        return;
    }
    if (auto added = watch_->add(path)) {
        watched_.insert(key);
    } else {
        report_once(
            tr("The Explorer cannot follow changes in %1: %2")
                .arg(QString::fromStdString(key), QString::fromStdString(added.error().message)));
    }
}

void ExplorerModel::unwatch_below(const std::filesystem::path& path) {
    if (!watch_) {
        return;
    }
    const std::string key = path.generic_string();
    const std::string prefix = key + "/";
    std::erase_if(watched_, [&](const std::string& watched) {
        if (watched != key && !watched.starts_with(prefix)) {
            return false;
        }
        watch_->remove(watched);
        return true;
    });
}

void ExplorerModel::report_once(const QString& message) {
    if (message == reported_problem_) {
        return;
    }
    reported_problem_ = message;
    emit watch_problem(message);
}

void ExplorerModel::set_excludes(core::FileExcludes excludes) {
    excludes_ = excludes;
    if (!tree_) {
        return;
    }
    tree_->set_excludes(std::move(excludes));
    refresh_open_directories(*root_);
}

void ExplorerModel::refresh(Node& directory) {
    tree_->refresh(directory.path);
    synchronize(directory);
}

void ExplorerModel::refresh_open_directories(Node& directory) {
    if (directory.state != State::read) {
        if (directory.state != State::unread) {
            directory.dirty = true;
        }
        return;
    }
    refresh(directory);
    for (const std::unique_ptr<Node>& child : directory.children) {
        if (child->entry.is_directory) {
            refresh_open_directories(*child);
        }
    }
}

ExplorerModel::Node* ExplorerModel::find_directory(const std::filesystem::path& path) const {
    if (!folder_) {
        return nullptr;
    }
    const std::filesystem::path relative = path.lexically_normal().lexically_relative(*folder_);
    if (relative.empty()) {
        return nullptr;
    }
    Node* node = root_.get();
    for (const std::filesystem::path& part : relative) {
        if (part.empty() || part == ".") {
            continue;
        }
        if (part == "..") {
            return nullptr;
        }
        const std::string name = part.string();
        const auto found = std::ranges::find_if(node->children, [&](const auto& child) {
            return !child->placeholder && child->entry.name == name;
        });
        if (found == node->children.end()) {
            return nullptr;
        }
        node = found->get();
    }
    return node->state != State::unread && node->entry.is_directory ? node : nullptr;
}

void ExplorerModel::reload(const std::filesystem::path& directory) {
    if (Node* node = find_directory(directory)) {
        if (node->state == State::read) {
            refresh(*node);
        } else {
            node->dirty = true;
        }
    }
}

bool ExplorerModel::is_read(const QModelIndex& directory) const {
    const Node* node = node_of(directory);
    return tree_ && node->entry.is_directory && node->state == State::read;
}

QModelIndex ExplorerModel::add_pending_entry(const QModelIndex& directory, bool folder) {
    remove_pending_entry();
    Node& parent = *node_of(directory);
    auto node = std::make_unique<Node>();
    node->parent = &parent;
    node->pending = true;
    node->pending_folder = folder;
    pending_ = node.get();
    beginInsertRows(directory, 0, 0);
    parent.children.insert(parent.children.begin(), std::move(node));
    for (std::size_t row = 0; row < parent.children.size(); ++row) {
        parent.children[row]->row = static_cast<int>(row);
    }
    endInsertRows();
    return index_of_node(pending_);
}

void ExplorerModel::remove_pending_entry() {
    if (pending_ == nullptr) {
        return;
    }
    Node& parent = *pending_->parent;
    const int row = pending_->row;
    pending_ = nullptr;
    beginRemoveRows(index_of_node(&parent), row, row);
    parent.children.erase(parent.children.begin() + row);
    for (std::size_t i = static_cast<std::size_t>(row); i < parent.children.size(); ++i) {
        parent.children[i]->row = static_cast<int>(i);
    }
    endRemoveRows();
}

QModelIndex ExplorerModel::pending_entry() const {
    return index_of_node(pending_);
}

void ExplorerModel::process_file_events() {
    if (!watch_) {
        return;
    }
    const auto events = watch_->poll();
    if (!events) {
        report_once(tr("The Explorer lost some file events and read the open folders again: %1")
                        .arg(QString::fromStdString(events.error().message)));
        refresh_open_directories(*root_);
        return;
    }
    if (events->empty()) {
        return;
    }
    // Parents sort before their children, so a directory that an event removes is gone before
    // its own events are looked at.
    std::set<std::filesystem::path> directories;
    for (const std::filesystem::path& path : *events) {
        directories.insert(path.parent_path());
    }
    for (const std::filesystem::path& directory : directories) {
        if (Node* node = find_directory(directory)) {
            if (node->state == State::read) {
                refresh(*node);
            } else {
                node->dirty = true;
            }
        }
    }
}

void ExplorerModel::synchronize(Node& directory) {
    const std::vector<core::FileTreeEntry>& fresh = tree_->children(directory.path);
    std::unordered_map<std::string_view, const core::FileTreeEntry*> by_name;
    by_name.reserve(fresh.size());
    for (const core::FileTreeEntry& entry : fresh) {
        by_name.emplace(entry.name, &entry);
    }
    const QModelIndex parent = index_of_node(&directory);
    const auto renumber = [&directory](std::size_t from) {
        for (std::size_t row = from; row < directory.children.size(); ++row) {
            directory.children[row]->row = static_cast<int>(row);
        }
    };

    // Rows that are gone or changed go in runs, from the end, so that a burst is a few signals.
    const auto kept = [&](const Node& node) {
        if (node.pending) {
            return true;
        }
        const auto same = by_name.find(node.entry.name);
        return same != by_name.end() && *same->second == node.entry;
    };
    for (std::size_t end = directory.children.size(); end > 0;) {
        if (kept(*directory.children[end - 1])) {
            --end;
            continue;
        }
        std::size_t begin = end - 1;
        while (begin > 0 && !kept(*directory.children[begin - 1])) {
            --begin;
        }
        for (std::size_t row = begin; row < end; ++row) {
            drop_loads_below(*directory.children[row]);
            unwatch_below(directory.children[row]->path);
            for (const Node* up = pending_; up != nullptr; up = up->parent) {
                if (up == directory.children[row].get()) {
                    pending_ = nullptr;
                    break;
                }
            }
        }
        beginRemoveRows(parent, static_cast<int>(begin), static_cast<int>(end) - 1);
        directory.children.erase(directory.children.begin() + static_cast<std::ptrdiff_t>(begin),
                                 directory.children.begin() + static_cast<std::ptrdiff_t>(end));
        renumber(begin);
        endRemoveRows();
        end = begin;
    }

    // What is left is in the order of `fresh`, so the new entries are the gaps.
    std::size_t at = 0;
    for (std::size_t i = 0; i < fresh.size();) {
        while (at < directory.children.size() && directory.children[at]->pending) {
            ++at;
        }
        if (at < directory.children.size() && directory.children[at]->entry.name == fresh[i].name) {
            ++at;
            ++i;
            continue;
        }
        std::size_t run = 0;
        while (i + run < fresh.size() &&
               !(at < directory.children.size() &&
                 directory.children[at]->entry.name == fresh[i + run].name)) {
            ++run;
        }
        std::vector<std::unique_ptr<Node>> added;
        added.reserve(run);
        for (std::size_t k = 0; k < run; ++k) {
            auto node = std::make_unique<Node>();
            node->parent = &directory;
            node->entry = fresh[i + k];
            node->path = directory.path / fresh[i + k].name;
            added.push_back(std::move(node));
        }
        beginInsertRows(parent, static_cast<int>(at), static_cast<int>(at + run) - 1);
        directory.children.insert(directory.children.begin() + static_cast<std::ptrdiff_t>(at),
                                  std::make_move_iterator(added.begin()),
                                  std::make_move_iterator(added.end()));
        renumber(at);
        endInsertRows();
        at += run;
        i += run;
    }
}

std::filesystem::path ExplorerModel::path_of(const QModelIndex& index) const {
    return index.isValid() ? node_of(index)->path : std::filesystem::path();
}

bool ExplorerModel::is_directory(const QModelIndex& index) const {
    return index.isValid() && node_of(index)->entry.is_directory;
}

QModelIndex ExplorerModel::index_of(const std::filesystem::path& path) const {
    if (!folder_) {
        return {};
    }
    const std::filesystem::path relative = path.lexically_normal().lexically_relative(*folder_);
    const Node* node = root_.get();
    for (const std::filesystem::path& part : relative) {
        if (part.empty() || part == ".") {
            continue;
        }
        const auto found = std::ranges::find_if(
            node->children, [&](const auto& child) { return child->entry.name == part.string(); });
        if (found == node->children.end()) {
            return {};
        }
        node = found->get();
    }
    return node == root_.get() ? QModelIndex() : index_of_node(node);
}

QModelIndex ExplorerModel::index(int row, int column, const QModelIndex& parent) const {
    const Node* node = node_of(parent);
    if (column != 0 || row < 0 || static_cast<std::size_t>(row) >= node->children.size()) {
        return {};
    }
    return createIndex(row, column, node->children[static_cast<std::size_t>(row)].get());
}

QModelIndex ExplorerModel::parent(const QModelIndex& child) const {
    if (!child.isValid()) {
        return {};
    }
    return index_of_node(node_of(child)->parent);
}

int ExplorerModel::rowCount(const QModelIndex& parent) const {
    return static_cast<int>(node_of(parent)->children.size());
}

int ExplorerModel::columnCount(const QModelIndex&) const {
    return 1;
}

bool ExplorerModel::hasChildren(const QModelIndex& parent) const {
    return node_of(parent)->entry.is_directory;
}

bool ExplorerModel::canFetchMore(const QModelIndex& parent) const {
    const Node* node = node_of(parent);
    return tree_ && node->entry.is_directory && node->state == State::unread;
}

void ExplorerModel::fetchMore(const QModelIndex& parent) {
    if (canFetchMore(parent)) {
        begin_load(*node_of(parent));
    }
}

QVariant ExplorerModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid()) {
        return {};
    }
    const Node& node = *node_of(index);
    if (node.pending) {
        return role == Qt::DisplayRole   ? QVariant(QString())
               : role == PendingRole     ? QVariant(true)
               : role == IsDirectoryRole ? QVariant(node.pending_folder)
               : role == PlaceholderRole ? QVariant(false)
                                         : QVariant();
    }
    if (node.placeholder) {
        return role == Qt::DisplayRole   ? QVariant(QString::fromStdString(node.entry.name))
               : role == PlaceholderRole ? QVariant(true)
               : role == IsDirectoryRole ? QVariant(false)
                                         : QVariant();
    }
    switch (role) {
    case Qt::DisplayRole:
        return QString::fromStdString(node.entry.name);
    case PlaceholderRole:
    case PendingRole:
        return false;
    case Qt::ToolTipRole: {
        const Error* error = tree_ ? tree_->error(node.path) : nullptr;
        return error ? QString::fromStdString(error->message) : text_of(node.path);
    }
    case IsDirectoryRole:
        return node.entry.is_directory;
    case PathRole:
        return text_of(node.path);
    case ErrorRole: {
        const Error* error = tree_ ? tree_->error(node.path) : nullptr;
        return error ? QString::fromStdString(error->message) : QString();
    }
    default:
        return {};
    }
}

Qt::ItemFlags ExplorerModel::flags(const QModelIndex& index) const {
    if (!index.isValid() || node_of(index)->placeholder) {
        return Qt::NoItemFlags;
    }
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsEditable;
}

} // namespace anvil::ui
