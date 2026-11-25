#include "anvil/ui/explorer_model.hpp"

#include "anvil/platform/file_system.hpp"

#include <QString>
#include <algorithm>
#include <unordered_map>

namespace anvil::ui {

struct ExplorerModel::Node {
    Node* parent = nullptr;
    int row = 0;
    core::FileTreeEntry entry;
    std::filesystem::path path;
    bool read = false;
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

QString text_of(const std::filesystem::path& path) {
    return QString::fromStdString(path.string());
}

} // namespace

ExplorerModel::ExplorerModel(core::DirectoryLister lister, QObject* parent)
    : QAbstractItemModel(parent), lister_(std::move(lister)), root_(std::make_unique<Node>()) {}

ExplorerModel::~ExplorerModel() = default;

ExplorerModel::Node* ExplorerModel::node_of(const QModelIndex& index) const {
    return index.isValid() ? static_cast<Node*>(index.internalPointer()) : root_.get();
}

QModelIndex ExplorerModel::index_of_node(const Node* node) const {
    if (node == nullptr || node == root_.get()) {
        return {};
    }
    return createIndex(node->row, 0, const_cast<Node*>(node));
}

void ExplorerModel::fill(Node& directory) {
    const std::vector<core::FileTreeEntry>& entries = tree_->children(directory.path);
    directory.read = true;
    if (entries.empty()) {
        return;
    }
    std::vector<std::unique_ptr<Node>> children;
    children.reserve(entries.size());
    for (const core::FileTreeEntry& entry : entries) {
        auto node = std::make_unique<Node>();
        node->parent = &directory;
        node->row = static_cast<int>(children.size());
        node->entry = entry;
        node->path = directory.path / entry.name;
        children.push_back(std::move(node));
    }
    const QModelIndex parent = index_of_node(&directory);
    beginInsertRows(parent, 0, static_cast<int>(children.size()) - 1);
    directory.children = std::move(children);
    endInsertRows();
}

void ExplorerModel::set_folder(std::optional<std::filesystem::path> folder) {
    beginResetModel();
    folder_ = std::move(folder);
    root_ = std::make_unique<Node>();
    tree_.reset();
    endResetModel();
    if (folder_) {
        tree_ = std::make_unique<core::FileTree>(*folder_, lister_, excludes_);
        root_->entry = {.name = folder_->filename().string(), .is_directory = true};
        root_->path = tree_->root();
        fill(*root_);
    }
}

void ExplorerModel::set_excludes(core::FileExcludes excludes) {
    excludes_ = excludes;
    if (!tree_) {
        return;
    }
    tree_->set_excludes(std::move(excludes));
    refresh_open_directories(*root_);
}

void ExplorerModel::refresh_open_directories(Node& directory) {
    tree_->refresh(directory.path);
    synchronize(directory);
    for (const std::unique_ptr<Node>& child : directory.children) {
        if (child->read) {
            refresh_open_directories(*child);
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

    for (int row = static_cast<int>(directory.children.size()) - 1; row >= 0; --row) {
        const Node& node = *directory.children[static_cast<std::size_t>(row)];
        const auto same = by_name.find(node.entry.name);
        if (same != by_name.end() && *same->second == node.entry) {
            continue;
        }
        beginRemoveRows(parent, row, row);
        directory.children.erase(directory.children.begin() + row);
        for (std::size_t later = static_cast<std::size_t>(row); later < directory.children.size();
             ++later) {
            directory.children[later]->row = static_cast<int>(later);
        }
        endRemoveRows();
    }
    for (std::size_t i = 0; i < fresh.size(); ++i) {
        if (i < directory.children.size() && directory.children[i]->entry.name == fresh[i].name) {
            continue;
        }
        auto node = std::make_unique<Node>();
        node->parent = &directory;
        node->entry = fresh[i];
        node->path = directory.path / fresh[i].name;
        const int row = static_cast<int>(i);
        beginInsertRows(parent, row, row);
        directory.children.insert(directory.children.begin() + row, std::move(node));
        for (std::size_t later = i; later < directory.children.size(); ++later) {
            directory.children[later]->row = static_cast<int>(later);
        }
        endInsertRows();
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
    return tree_ && node->entry.is_directory && !node->read;
}

void ExplorerModel::fetchMore(const QModelIndex& parent) {
    if (canFetchMore(parent)) {
        fill(*node_of(parent));
    }
}

QVariant ExplorerModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid()) {
        return {};
    }
    const Node& node = *node_of(index);
    switch (role) {
    case Qt::DisplayRole:
        return QString::fromStdString(node.entry.name);
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
    return index.isValid() ? Qt::ItemIsEnabled | Qt::ItemIsSelectable : Qt::NoItemFlags;
}

} // namespace anvil::ui
