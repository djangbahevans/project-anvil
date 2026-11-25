#pragma once

#include "anvil/core/error.hpp"

#include <expected>
#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

// The create, rename and delete operations of a `WorkspaceEdit` (`IWorkspaceFileEditDto` of the
// extension host protocol), run on the local file system.
namespace anvil::exthost {

struct FileOperation {
    enum class Kind { create, remove, rename };

    Kind kind = Kind::create;
    // The file or folder that is created or removed, and the source of a rename.
    std::filesystem::path path;
    // The destination of a rename.
    std::filesystem::path target;
    // The initial content of a created file.
    std::string contents;
    // Creates a directory instead of a file.
    bool folder = false;
    // Unset when the extension gave no value, which matters for `ignore_if_exists`.
    std::optional<bool> overwrite;
    bool ignore_if_exists = false;
    bool ignore_if_not_exists = false;
    bool recursive = false;

    // The place an operation puts something: the new file, or the target of a rename.
    const std::filesystem::path& destination() const {
        return kind == Kind::rename ? target : path;
    }
};

// Whether `path` is `root` or below it, by the components of the paths.
bool is_path_within(const std::filesystem::path& path, const std::filesystem::path& root);

// `path`, which is within `from`, as it is when `from` is at `to`.
std::filesystem::path path_moved(const std::filesystem::path& path,
                                 const std::filesystem::path& from,
                                 const std::filesystem::path& to);

// Reads an item of `WorkspaceEdit.edits` that is not a text edit. Fails with `unsupported` for
// what the editor does not do: a resource that is not a `file:` URI, `copy`, and contents that
// are not given inline.
std::expected<FileOperation, Error> parse_file_operation(const nlohmann::json& item);

// Checks the preconditions of operations in sequence: what must exist and what must not, and that
// a directory is only removed with `recursive` when it has content. Each call assumes that the
// operations checked before it have run, so a whole edit can be checked before anything changes.
// Only exact paths and the contents of moved directories are tracked, so a check is a forecast;
// perform() tests the preconditions again when the operation runs.
class FileOperationPlanner {
public:
    // Whether the operation runs: false when an `ignoreIf...` option skips it, an error when a
    // precondition fails.
    std::expected<bool, Error> check(const FileOperation& operation);

private:
    struct Effect {
        enum class Kind { created_file, created_directory, removed, moved };
        Kind kind;
        std::filesystem::path path;
        std::filesystem::path target;
    };

    // The type bits of what is at `path` as the operations checked so far leave it, or nothing.
    std::expected<std::optional<unsigned>, Error> type_of(std::filesystem::path path) const;
    std::expected<void, Error> check_parents(const std::filesystem::path& path) const;

    std::vector<Effect> effects_;
};

struct PerformedOperation {
    // False when an `ignoreIf...` option skipped the operation.
    bool ran = false;
    // Reverses the operation as far as that is possible: it removes what was created, restores
    // the content that a create replaced and moves a rename back. A removal is not reversed, and
    // neither is what a rename replaced. Empty when there is nothing to reverse.
    std::function<void()> undo;
};

// Runs the operation. A create makes the missing parent directories, and so does a rename for
// its target.
std::expected<PerformedOperation, Error> perform(const FileOperation& operation);

} // namespace anvil::exthost
