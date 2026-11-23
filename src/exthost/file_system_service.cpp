#include "anvil/exthost/file_system_service.hpp"

#include "anvil/core/utf.hpp"
#include "anvil/exthost/vscode_types.hpp"
#include "anvil/platform/file.hpp"
#include "anvil/platform/file_system.hpp"

namespace anvil::exthost {
namespace {

using nlohmann::json;
namespace fs = std::filesystem;

// `FileSystemProviderErrorCode` of VS Code, by which the extension host builds its errors.
const char* provider_code(ErrorCode code) {
    switch (code) {
    case ErrorCode::not_found:
        return "EntryNotFound";
    case ErrorCode::already_exists:
        return "EntryExists";
    case ErrorCode::not_a_directory:
        return "EntryNotADirectory";
    case ErrorCode::is_a_directory:
        return "EntryIsADirectory";
    case ErrorCode::permission_denied:
        return "NoPermissions";
    default:
        return "Unknown";
    }
}

HandlerResult failed(const Error& error) {
    return Failed{error_json(provider_code(error.code), error.message)};
}

HandlerResult failed(ErrorCode code, const std::string& message) {
    return failed(Error{code, message});
}

// The path of a `file:` resource; anything else has no provider.
std::expected<fs::path, HandlerResult> local_path(const json& resource) {
    const auto uri = uri_from_json(resource);
    if (!uri) {
        return std::unexpected(Failed{error_json(uri.error().message)});
    }
    if (const auto path = uri->file_path()) {
        return *path;
    }
    return std::unexpected(
        Failed{error_json("Unavailable", "ENOPRO: No file system provider found for resource '" +
                                             uri->to_string() + "'")});
}

// The directory a file is to be put in, with its missing parents. A file where a directory has to
// be is "not a directory", not "exists".
std::expected<void, Error> make_parent(const fs::path& path) {
    auto made = platform::make_directory(path.parent_path());
    if (!made && made.error().code == ErrorCode::already_exists) {
        return std::unexpected(Error{ErrorCode::not_a_directory, made.error().message});
    }
    return made;
}

// A write goes through a symbolic link to its target and does not replace the link.
fs::path write_target(const fs::path& path) {
    std::error_code error;
    if (fs::is_symlink(fs::symlink_status(path, error))) {
        const fs::path resolved = fs::weakly_canonical(path, error);
        if (!error) {
            return resolved;
        }
    }
    return path;
}

json stat_json(const platform::FileStat& stat) {
    json result = {{"type", stat.type},
                   {"ctime", stat.created_ms},
                   {"mtime", stat.modified_ms},
                   {"size", stat.size}};
    if (stat.read_only) {
        result["permissions"] = 1;
    }
    return result;
}

bool overwrites(const json& options) {
    return options.is_object() && options.value("overwrite", false);
}

} // namespace

FileSystemService::FileSystemService(RequestRouter& router) {
    using Result = std::optional<HandlerResult>;
    const ProxyId service = ProxyId::MainThreadFileSystem;

    router.on(service, "$stat", [](const Request&, const json& args) -> Result {
        const auto path = local_path(args.at(0));
        if (!path) {
            return path.error();
        }
        const auto stat = platform::stat_path(*path);
        if (!stat) {
            return failed(stat.error());
        }
        return OkJson{stat_json(*stat).dump()};
    });

    router.on(service, "$readdir", [](const Request&, const json& args) -> Result {
        const auto path = local_path(args.at(0));
        if (!path) {
            return path.error();
        }
        const auto entries = platform::list_directory(*path);
        if (!entries) {
            return failed(entries.error());
        }
        json result = json::array();
        for (const platform::DirectoryEntry& entry : *entries) {
            if (core::find_invalid_utf8(entry.name)) {
                return failed(ErrorCode::io_error,
                              "cannot list " + path->string() + ": a name is not valid UTF-8");
            }
            result.push_back(json::array({entry.name, entry.type}));
        }
        return OkJson{result.dump()};
    });

    router.on(service, "$readFile", [](const Request&, const json& args) -> Result {
        const auto path = local_path(args.at(0));
        if (!path) {
            return path.error();
        }
        const auto stat = platform::stat_path(*path);
        if (!stat) {
            return failed(stat.error());
        }
        if (stat->type & platform::file_type::directory) {
            return failed(ErrorCode::is_a_directory,
                          "cannot read " + path->string() + ": it is a directory");
        }
        auto contents = platform::read_file(*path);
        if (!contents) {
            return failed(contents.error());
        }
        return OkBuffer{to_bytes(*contents)};
    });

    router.on(service, "$writeFile", [](const Request& request, const json& args) -> Result {
        const auto path = local_path(args.at(0));
        if (!path) {
            return path.error();
        }
        const Bytes* content = buffer_argument(request, 1);
        if (!content) {
            return Failed{error_json("the content of the file is not a buffer")};
        }
        const fs::path target = write_target(*path);
        const auto stat = platform::stat_path(target);
        if (stat && (stat->type & platform::file_type::directory)) {
            return failed(ErrorCode::is_a_directory,
                          "cannot write " + path->string() + ": it is a directory");
        }
        if (const auto made = make_parent(target); !made) {
            return failed(made.error());
        }
        if (const auto written = platform::write_file(target, as_text(*content)); !written) {
            return failed(written.error());
        }
        return Ok{};
    });

    const auto move_or_copy = [](bool move) {
        return [move](const Request&, const json& args) -> Result {
            const auto from = local_path(args.at(0));
            if (!from) {
                return from.error();
            }
            const auto to = local_path(args.at(1));
            if (!to) {
                return to.error();
            }
            if (const auto made = make_parent(*to); !made) {
                return failed(made.error());
            }
            const bool overwrite = overwrites(args.size() > 2 ? args.at(2) : json());
            const auto done = move ? platform::rename_path(*from, *to, overwrite)
                                   : platform::copy_path(*from, *to, overwrite);
            if (!done) {
                return failed(done.error());
            }
            return Ok{};
        };
    };
    router.on(service, "$rename", move_or_copy(true));
    router.on(service, "$copy", move_or_copy(false));

    router.on(service, "$mkdir", [](const Request&, const json& args) -> Result {
        const auto path = local_path(args.at(0));
        if (!path) {
            return path.error();
        }
        if (const auto made = platform::make_directory(*path); !made) {
            return failed(made.error());
        }
        return Ok{};
    });

    router.on(service, "$delete", [](const Request&, const json& args) -> Result {
        const auto path = local_path(args.at(0));
        if (!path) {
            return path.error();
        }
        const bool recursive =
            args.size() > 1 && args.at(1).is_object() && args.at(1).value("recursive", false);
        if (const auto removed = platform::remove_path(*path, recursive); !removed) {
            return failed(removed.error());
        }
        return Ok{};
    });

    // Nothing has to be started for the local file system, and other schemes are served by the
    // providers of extensions, which the extension host activates itself.
    router.on(service, "$ensureActivation",
              [](const Request&, const json&) -> Result { return Ok{}; });
}

} // namespace anvil::exthost
