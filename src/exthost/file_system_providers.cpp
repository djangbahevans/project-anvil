#include "anvil/exthost/file_system_providers.hpp"

#include "anvil/exthost/vscode_types.hpp"

#include <algorithm>
#include <array>

namespace anvil::exthost {
namespace {

using nlohmann::json;

constexpr const char* source = "file system";
constexpr std::string_view marker_suffix = " (FileSystemError)";

constexpr std::array<std::string_view, 10> known_codes = {
    "EntryNotFound", "EntryExists",      "EntryNotADirectory",       "EntryIsADirectory",
    "EntryTooLarge", "EntryWriteLocked", "EntryExceedsStorageQuota", "NoPermissions",
    "Unavailable",   "Unknown"};

ProviderError no_provider(const core::Uri& resource) {
    return {"Unavailable",
            "ENOPRO: No file system provider found for resource '" + resource.to_string() + "'"};
}

HandlerResult failed(const ProviderError& error) {
    return Failed{error_json(error.name, error.message)};
}

MixedArg json_arg(const json& value) {
    return JsonArg{value.dump()};
}

// The reply of a call that has a JSON result as a handler result.
HandlerResult to_handler_result(BinaryReply reply) {
    if (reply.buffer) {
        return OkBuffer{std::move(*reply.buffer)};
    }
    if (reply.json) {
        return OkJson{std::move(*reply.json)};
    }
    return Ok{};
}

std::expected<json, ProviderError> parsed(const BinaryReply& reply, const char* what) {
    if (!reply.json) {
        return std::unexpected(
            ProviderError{"Unknown", std::string("the provider gave no ") + what});
    }
    json value = json::parse(*reply.json, nullptr, false);
    if (value.is_discarded()) {
        return std::unexpected(
            ProviderError{"Unknown", std::string("the provider gave no valid ") + what});
    }
    return value;
}

} // namespace

ProviderError provider_error_from(const std::string& text) {
    const json error = json::parse(text, nullptr, false);
    if (!error.is_object()) {
        return {"Unavailable", text};
    }
    std::string name = error.value("name", "");
    const std::string message = error.value("message", "");
    if (name.ends_with(marker_suffix)) {
        name.resize(name.size() - marker_suffix.size());
    }
    if (name == "ENOPRO") {
        return {"Unavailable", message};
    }
    if (std::ranges::find(known_codes, name) == known_codes.end()) {
        name = "Unknown";
    }
    return {std::move(name), message};
}

FileSystemProviders::FileSystemProviders(Session& session, RequestRouter& router,
                                         std::shared_ptr<core::Logger> logger)
    : session_(session), logger_(std::move(logger)) {
    const ProxyId service = ProxyId::MainThreadFileSystem;
    router.on(service, "$registerFileSystemProvider",
              [this](const Request&, const json& args) -> std::optional<HandlerResult> {
                  if (!args.is_array() || args.size() < 3 || !args[0].is_number() ||
                      !args[1].is_string() || !args[2].is_number_integer()) {
                      return Failed{error_json("$registerFileSystemProvider needs a handle, a "
                                               "scheme and capabilities")};
                  }
                  Provider provider;
                  provider.handle = args[0].get<double>();
                  provider.scheme = args[1].get<std::string>();
                  provider.capabilities = args[2].get<int>();
                  if (args.size() > 3 && args[3].is_object()) {
                      provider.readonly_message = args[3].dump();
                  }
                  if (providers_.contains(provider.scheme)) {
                      return Failed{error_json("a provider for the scheme '" + provider.scheme +
                                               "' is registered already")};
                  }
                  std::string scheme = provider.scheme;
                  providers_.emplace(std::move(scheme), std::move(provider));
                  return Ok{};
              });
    router.on(service, "$unregisterProvider",
              [this](const Request&, const json& args) -> std::optional<HandlerResult> {
                  if (args.is_array() && !args.empty() && args[0].is_number()) {
                      const double handle = args[0].get<double>();
                      std::erase_if(providers_, [handle](const auto& entry) {
                          return entry.second.handle == handle;
                      });
                  }
                  return Ok{};
              });
    router.on(service, "$onFileSystemChange",
              [this](const Request&, const json& args) -> std::optional<HandlerResult> {
                  if (!args.is_array() || args.size() < 2 || !args[1].is_array()) {
                      return Failed{error_json("$onFileSystemChange needs a handle and changes")};
                  }
                  std::vector<ProviderChange> changes;
                  for (const json& change : args[1]) {
                      const auto resource =
                          change.is_object() && change.contains("resource")
                              ? uri_from_json(change["resource"])
                              : std::unexpected(Error{ErrorCode::invalid_argument, "no resource"});
                      if (!resource || change.value("type", -1) < 0) {
                          continue;
                      }
                      changes.push_back({*resource, change.value("type", 0)});
                  }
                  if (!changes.empty() && change_listener_) {
                      change_listener_(changes);
                  }
                  return Ok{};
              });
}

const FileSystemProviders::Provider* FileSystemProviders::find(const std::string& scheme) const {
    const auto found = providers_.find(scheme);
    return found == providers_.end() ? nullptr : &found->second;
}

void FileSystemProviders::call(const core::Uri& resource, bool modifies, const char* method,
                               MixedArgs args, Raw done) {
    const Provider* provider = find(resource.scheme);
    if (!provider) {
        done(std::unexpected(no_provider(resource)));
        return;
    }
    if (modifies && (provider->capabilities & readonly_capability)) {
        done(std::unexpected(
            ProviderError{"NoPermissions", "Unable to modify '" + resource.to_string() +
                                               "': the file system of the scheme '" +
                                               resource.scheme + "' is read-only"}));
        return;
    }
    args.insert(args.begin(), JsonArg{json(provider->handle).dump()});
    session_.call_binary(ProxyId::ExtHostFileSystem, method, std::move(args),
                         [done = std::move(done)](BinaryResult result) {
                             if (!result) {
                                 done(std::unexpected(provider_error_from(result.error())));
                             } else {
                                 done(std::move(*result));
                             }
                         });
}

void FileSystemProviders::stat(const core::Uri& resource, Callback<ProviderStat> done) {
    call(resource, false, "$stat", {json_arg(uri_to_json(resource))},
         [done = std::move(done)](std::expected<BinaryReply, ProviderError> reply) {
             if (!reply) {
                 return done(std::unexpected(std::move(reply.error())));
             }
             const auto value = parsed(*reply, "stat");
             if (!value) {
                 return done(std::unexpected(value.error()));
             }
             ProviderStat stat;
             stat.type = value->value("type", 0);
             stat.ctime = value->value("ctime", 0.0);
             stat.mtime = value->value("mtime", 0.0);
             stat.size = value->value("size", 0.0);
             if (value->contains("permissions") && (*value)["permissions"].is_number_integer()) {
                 stat.permissions = (*value)["permissions"].get<int>();
             }
             done(stat);
         });
}

void FileSystemProviders::read_directory(const core::Uri& resource,
                                         Callback<std::vector<std::pair<std::string, int>>> done) {
    call(resource, false, "$readdir", {json_arg(uri_to_json(resource))},
         [done = std::move(done)](std::expected<BinaryReply, ProviderError> reply) {
             if (!reply) {
                 return done(std::unexpected(std::move(reply.error())));
             }
             const auto value = parsed(*reply, "directory listing");
             if (!value) {
                 return done(std::unexpected(value.error()));
             }
             std::vector<std::pair<std::string, int>> entries;
             for (const json& entry : *value) {
                 if (entry.is_array() && entry.size() == 2 && entry[0].is_string() &&
                     entry[1].is_number_integer()) {
                     entries.emplace_back(entry[0].get<std::string>(), entry[1].get<int>());
                 }
             }
             done(std::move(entries));
         });
}

void FileSystemProviders::read_file(const core::Uri& resource, Callback<Bytes> done) {
    call(resource, false, "$readFile", {json_arg(uri_to_json(resource))},
         [done = std::move(done)](std::expected<BinaryReply, ProviderError> reply) {
             if (!reply) {
                 return done(std::unexpected(std::move(reply.error())));
             }
             if (!reply->buffer) {
                 return done(std::unexpected(
                     ProviderError{"Unknown", "the provider did not reply with the content"}));
             }
             done(std::move(*reply->buffer));
         });
}

namespace {

// Calls back with the unit value, or the error.
auto unit(FileSystemProviders::Callback<std::monostate> done) {
    return [done = std::move(done)](std::expected<BinaryReply, ProviderError> reply) {
        if (!reply) {
            return done(std::unexpected(std::move(reply.error())));
        }
        done(std::monostate{});
    };
}

} // namespace

void FileSystemProviders::write_file(const core::Uri& resource, Bytes content,
                                     Callback<std::monostate> done) {
    call(resource, true, "$writeFile",
         {json_arg(uri_to_json(resource)), BufferArg{std::move(content)},
          json_arg({{"create", true}, {"overwrite", true}, {"unlock", false}, {"atomic", false}})},
         unit(std::move(done)));
}

void FileSystemProviders::rename(const core::Uri& resource, const core::Uri& target, bool overwrite,
                                 Callback<std::monostate> done) {
    if (resource.scheme != target.scheme) {
        return done(std::unexpected(
            ProviderError{"Unavailable", "Cannot move '" + resource.to_string() + "' to '" +
                                             target.to_string() + "': the schemes differ"}));
    }
    call(resource, true, "$rename",
         {json_arg(uri_to_json(resource)), json_arg(uri_to_json(target)),
          json_arg({{"overwrite", overwrite}})},
         unit(std::move(done)));
}

void FileSystemProviders::copy(const core::Uri& resource, const core::Uri& target, bool overwrite,
                               Callback<std::monostate> done) {
    if (resource.scheme != target.scheme) {
        return done(std::unexpected(
            ProviderError{"Unavailable", "Cannot copy '" + resource.to_string() + "' to '" +
                                             target.to_string() + "': the schemes differ"}));
    }
    const Provider* provider = find(resource.scheme);
    if (provider && !(provider->capabilities & folder_copy_capability)) {
        return done(std::unexpected(ProviderError{"Unavailable", "The file system of the scheme '" +
                                                                     resource.scheme +
                                                                     "' does not copy files"}));
    }
    call(resource, true, "$copy",
         {json_arg(uri_to_json(resource)), json_arg(uri_to_json(target)),
          json_arg({{"overwrite", overwrite}})},
         unit(std::move(done)));
}

void FileSystemProviders::create_directory(const core::Uri& resource,
                                           Callback<std::monostate> done) {
    call(resource, true, "$mkdir", {json_arg(uri_to_json(resource))}, unit(std::move(done)));
}

void FileSystemProviders::remove(const core::Uri& resource, bool recursive,
                                 Callback<std::monostate> done) {
    call(resource, true, "$delete",
         {json_arg(uri_to_json(resource)),
          json_arg({{"recursive", recursive}, {"useTrash", false}, {"atomic", false}})},
         unit(std::move(done)));
}

void FileSystemProviders::serve(const std::string& method, const json& args,
                                std::optional<Bytes> content,
                                std::function<void(HandlerResult)> reply) {
    const auto resource =
        args.is_array() && !args.empty() ? uri_from_json(args[0]) : std::unexpected(Error{});
    if (!resource) {
        return reply(Failed{error_json(method + " needs a resource")});
    }
    const bool modifies = method != "$stat" && method != "$readdir" && method != "$readFile";
    MixedArgs forwarded;
    if (method == "$writeFile") {
        forwarded = {
            json_arg(args[0]), BufferArg{content ? std::move(*content) : Bytes{}},
            json_arg(
                {{"create", true}, {"overwrite", true}, {"unlock", false}, {"atomic", false}})};
    } else if (method == "$rename" || method == "$copy") {
        const auto target = args.size() > 1 ? uri_from_json(args[1]) : std::unexpected(Error{});
        if (!target || target->scheme != resource->scheme) {
            return reply(failed(
                {"Unavailable", "Cannot " + std::string(method == "$copy" ? "copy" : "move") +
                                    " between file systems of different schemes"}));
        }
        if (method == "$copy") {
            const Provider* provider = find(resource->scheme);
            if (provider && !(provider->capabilities & folder_copy_capability)) {
                return reply(
                    failed({"Unavailable", "The file system of the scheme '" + resource->scheme +
                                               "' does not copy files"}));
            }
        }
        forwarded = {json_arg(args[0]), json_arg(args[1]),
                     json_arg({{"overwrite", args.size() > 2 && args[2].is_object() &&
                                                 args[2].value("overwrite", false)}})};
    } else if (method == "$delete") {
        const Provider* provider = find(resource->scheme);
        const json options = args.size() > 1 && args[1].is_object() ? args[1] : json::object();
        const bool trash = options.value("useTrash", false) && provider &&
                           (provider->capabilities & trash_capability);
        forwarded = {json_arg(args[0]), json_arg({{"recursive", options.value("recursive", false)},
                                                  {"useTrash", trash},
                                                  {"atomic", options.value("atomic", false)}})};
    } else {
        forwarded = {json_arg(args[0])};
    }
    call(*resource, modifies, method.c_str(), std::move(forwarded),
         [reply = std::move(reply)](std::expected<BinaryReply, ProviderError> result) {
             if (!result) {
                 return reply(failed(result.error()));
             }
             reply(to_handler_result(std::move(*result)));
         });
}

bool FileSystemProviders::watch(const core::Uri& resource, double session, const json& options) {
    const Provider* provider = find(resource.scheme);
    if (!provider) {
        return false;
    }
    json args = json::array({provider->handle, session, uri_to_json(resource), options});
    MixedArgs mixed;
    for (const json& arg : args) {
        mixed.push_back(json_arg(arg));
    }
    session_.call_binary(ProxyId::ExtHostFileSystem, "$watch", std::move(mixed),
                         [this, scheme = resource.scheme](BinaryResult result) {
                             if (!result) {
                                 logger_->log(core::LogLevel::warning, source,
                                              "The provider of '" + scheme +
                                                  "' cannot watch: " + result.error());
                             }
                         });
    return true;
}

void FileSystemProviders::unwatch(const std::string& scheme, double session) {
    const Provider* provider = find(scheme);
    if (!provider) {
        return;
    }
    session_.call_binary(ProxyId::ExtHostFileSystem, "$unwatch",
                         {json_arg(provider->handle), json_arg(session)}, [](BinaryResult) {});
}

} // namespace anvil::exthost
