#include "anvil/exthost/file_system_service.hpp"

#include "anvil/core/utf.hpp"
#include "anvil/exthost/vscode_types.hpp"
#include "anvil/platform/file.hpp"
#include "anvil/platform/file_system.hpp"

#include <algorithm>
#include <functional>
#include <memory>

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

HandlerResult no_provider(const core::Uri& uri) {
    return Failed{error_json("Unavailable", "ENOPRO: No file system provider found for resource '" +
                                                uri.to_string() + "'")};
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
    return std::unexpected(no_provider(*uri));
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

FileSystemService::FileSystemService(Session& session, RequestRouter& router,
                                     ActivationService& activation, FileSystemProviders& providers,
                                     std::shared_ptr<core::Logger> logger) {
    using Result = std::optional<HandlerResult>;
    using Handler = std::function<Result(const Request&, const json&)>;
    const ProxyId service = ProxyId::MainThreadFileSystem;

    // VS Code's file service fires `onFileSystem:<scheme>` before every operation on a resource
    // and waits for the extensions that start (`activateProvider`), so that a provider that an
    // extension registers for the scheme is there when the operation runs. Nothing registers
    // providers yet: once the activation has finished the operation is the provider's, or fails as
    // for a scheme that has no provider. `resources` is the number of leading arguments that are
    // resources.
    const auto serve = [&router, &session, &activation, &providers,
                        logger](const char* method, Handler handler, std::size_t resources = 1) {
        router.on(
            service, method,
            [&session, &activation, &providers, logger, method = std::string(method),
             handler = std::move(handler),
             resources](const Request& request, const json& args) -> Result {
                std::vector<core::Uri> foreign;
                for (std::size_t i = 0; i < resources && i < args.size(); ++i) {
                    const auto uri = uri_from_json(args[i]);
                    if (uri && uri->scheme != "file" &&
                        std::ranges::none_of(foreign, [&](const core::Uri& other) {
                            return other.scheme == uri->scheme;
                        })) {
                        foreign.push_back(*uri);
                    }
                }
                if (foreign.empty()) {
                    return handler(request, args);
                }
                const std::uint32_t req = request.req;
                std::optional<Bytes> content;
                if (const Bytes* buffer = buffer_argument(request, 1)) {
                    content = *buffer;
                }
                auto waiting = std::make_shared<std::size_t>(foreign.size());
                auto request_args = std::make_shared<json>(args);
                auto request_content = std::make_shared<std::optional<Bytes>>(std::move(content));
                for (const core::Uri& uri : foreign) {
                    activation.fire("onFileSystem:" + uri.scheme, ActivationKind::normal,
                                    [&session, &providers, logger, req, waiting, method,
                                     request_args, request_content, first = foreign.front(),
                                     scheme = uri.scheme](CallResult result) {
                                        if (!result) {
                                            logger->log(core::LogLevel::warning, "extension host",
                                                        "Activating the file system of '" + scheme +
                                                            "' failed: " + result.error());
                                        }
                                        if (--*waiting != 0) {
                                            return;
                                        }
                                        if (!providers.find(first.scheme)) {
                                            session.reply(req, no_provider(first));
                                            return;
                                        }
                                        providers.serve(method, *request_args,
                                                        std::move(*request_content),
                                                        [&session, req](HandlerResult reply) {
                                                            session.reply(req, std::move(reply));
                                                        });
                                    });
                }
                return std::nullopt;
            });
    };

    serve("$stat", [](const Request&, const json& args) -> Result {
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

    serve("$readdir", [](const Request&, const json& args) -> Result {
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

    serve("$readFile", [](const Request&, const json& args) -> Result {
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

    serve("$writeFile", [](const Request& request, const json& args) -> Result {
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
    serve("$rename", move_or_copy(true), 2);
    serve("$copy", move_or_copy(false), 2);

    serve("$mkdir", [](const Request&, const json& args) -> Result {
        const auto path = local_path(args.at(0));
        if (!path) {
            return path.error();
        }
        if (const auto made = platform::make_directory(*path); !made) {
            return failed(made.error());
        }
        return Ok{};
    });

    serve("$delete", [](const Request&, const json& args) -> Result {
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

    router.on(service, "$ensureActivation",
              [&session, &activation, logger](const Request& request, const json& args) -> Result {
                  if (!args.is_array() || args.empty() || !args[0].is_string()) {
                      return Failed{error_json("$ensureActivation needs a scheme")};
                  }
                  const std::string scheme = args[0].get<std::string>();
                  if (scheme == "file") {
                      return Ok{};
                  }
                  const std::uint32_t req = request.req;
                  activation.fire("onFileSystem:" + scheme, ActivationKind::normal,
                                  [&session, logger, req, scheme](CallResult result) {
                                      if (!result) {
                                          logger->log(core::LogLevel::warning, "extension host",
                                                      "Activating the file system of '" + scheme +
                                                          "' failed: " + result.error());
                                      }
                                      session.reply(req, Ok{});
                                  });
                  return std::nullopt;
              });
}

} // namespace anvil::exthost
