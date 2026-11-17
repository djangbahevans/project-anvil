#include "anvil/exthost/logging_service.hpp"

#include "anvil/exthost/vscode_types.hpp"
#include "anvil/platform/file.hpp"

#include <filesystem>
#include <regex>

namespace anvil::exthost {
namespace {

using core::LogLevel;
using nlohmann::json;

constexpr const char* host_source = "extension host";

LogLevel level_from_console_severity(const std::string& severity) {
    if (severity == "error") {
        return LogLevel::error;
    }
    if (severity == "warn") {
        return LogLevel::warning;
    }
    if (severity == "debug") {
        return LogLevel::debug;
    }
    if (severity == "trace") {
        return LogLevel::trace;
    }
    return LogLevel::info;
}

// The extension host sends the arguments of a console call as one JSON string. Stack traces it
// attaches are objects with a `__$stack` member.
std::string render_console_message(const json& message) {
    const std::string text = message.value("arguments", "[]");
    const json arguments = json::parse(text, nullptr, false);
    if (!arguments.is_array()) {
        return text;
    }
    std::string result;
    std::string stack;
    for (const json& argument : arguments) {
        if (argument.is_object() && argument.contains("__$stack")) {
            stack = argument["__$stack"].get<std::string>();
            continue;
        }
        if (!result.empty()) {
            result += ' ';
        }
        result += argument.is_string() ? argument.get<std::string>() : argument.dump();
    }
    if (!stack.empty()) {
        result += '\n' + stack;
    }
    return result;
}

// A serialized error carries the name, message and stack; the stack already starts with the
// name and message.
std::string render_error(const json& error) {
    if (!error.is_object()) {
        return error.is_string() ? error.get<std::string>() : error.dump();
    }
    const std::string stack = error.value("stack", "");
    if (!stack.empty()) {
        return stack;
    }
    return error.value("name", "Error") + ": " + error.value("message", "");
}

std::string extension_id(const json& identifier) {
    return identifier.is_object() ? identifier.value("value", "unknown extension")
                                  : "unknown extension";
}

} // namespace

LoggingService::LoggingService(std::shared_ptr<core::Logger> logger, RequestRouter& router)
    : logger_(std::move(logger)) {
    using Args = const json&;
    const auto ok = [](const Request&, Args) -> std::optional<HandlerResult> { return Ok{}; };

    router.on(ProxyId::MainThreadConsole, "$logExtensionHostMessage",
              [this](const Request&, Args args) -> std::optional<HandlerResult> {
                  const json& message = args.at(0);
                  log(level_from_console_severity(message.value("severity", "log")), host_source,
                      render_console_message(message));
                  return Ok{};
              });
    router.on(ProxyId::MainThreadErrors, "$onUnexpectedError",
              [this](const Request&, Args args) -> std::optional<HandlerResult> {
                  log(LogLevel::error, host_source, render_error(args.at(0)));
                  return Ok{};
              });

    router.on(ProxyId::MainThreadExtensionService, "$onExtensionRuntimeError",
              [this](const Request&, Args args) -> std::optional<HandlerResult> {
                  log(LogLevel::error, extension_id(args.at(0)), render_error(args.at(1)));
                  return Ok{};
              });
    router.on(ProxyId::MainThreadExtensionService, "$onExtensionActivationError",
              [this](const Request&, Args args) -> std::optional<HandlerResult> {
                  log(LogLevel::error, extension_id(args.at(0)),
                      "activation failed: " + render_error(args.at(1)));
                  return Ok{};
              });
    router.on(ProxyId::MainThreadExtensionService, "$onDidActivateExtension",
              [this](const Request&, Args args) -> std::optional<HandlerResult> {
                  log(LogLevel::debug, extension_id(args.at(0)),
                      "activated (code load " + std::to_string(args.at(1).get<double>()) +
                          " ms, activate call " + std::to_string(args.at(2).get<double>()) +
                          " ms, resolved " + std::to_string(args.at(3).get<double>()) + " ms)");
                  return Ok{};
              });

    // A visible logger is an output channel of its own, such as the extension host's log or a
    // log output channel. A hidden one only backs a plain output channel, which registers
    // itself by label next.
    router.on(ProxyId::MainThreadLogger, "$registerLogger",
              [this](const Request&, Args args) -> std::optional<HandlerResult> {
                  const json& logger = args.at(0);
                  const auto file = uri_from_json(logger.at("resource"));
                  if (file && file->file_path() && !logger.value("hidden", false)) {
                      track(file->to_string(), *file->file_path(),
                            logger.value("name", logger.value("id", "log")));
                  }
                  return Ok{};
              });
    router.on(ProxyId::MainThreadLogger, "$deregisterLogger",
              [this](const Request&, Args args) -> std::optional<HandlerResult> {
                  if (const auto file = uri_from_json(args.at(0))) {
                      channels_.erase(file->to_string());
                  }
                  return Ok{};
              });
    router.on(ProxyId::MainThreadOutputService, "$register",
              [this](const Request&, Args args) -> std::optional<HandlerResult> {
                  const auto file = uri_from_json(args.at(1));
                  if (!file || !file->file_path()) {
                      return Failed{error_json("an output channel needs a file")};
                  }
                  const std::string id = "anvil-channel-" + std::to_string(next_channel_++);
                  track(file->to_string(), *file->file_path(), args.at(0).get<std::string>());
                  channel_keys_[id] = file->to_string();
                  return OkJson{json(id).dump()};
              });
    router.on(ProxyId::MainThreadOutputService, "$update",
              [this](const Request&, Args args) -> std::optional<HandlerResult> {
                  const auto key = channel_keys_.find(args.at(0).get<std::string>());
                  const auto channel =
                      key == channel_keys_.end() ? channels_.end() : channels_.find(key->second);
                  if (channel == channels_.end()) {
                      return Ok{};
                  }
                  constexpr int append = 1;
                  if (args.at(1).get<int>() == append) {
                      read_channel(channel->second);
                  } else if (args.size() > 2 && args[2].is_number_unsigned()) {
                      // The channel was replaced or cleared up to this point of the file.
                      channel->second.offset = args[2].get<std::uint64_t>();
                      channel->second.pending.clear();
                  }
                  return Ok{};
              });
    router.on(ProxyId::MainThreadOutputService, "$dispose",
              [this](const Request&, Args args) -> std::optional<HandlerResult> {
                  const auto key = channel_keys_.find(args.at(0).get<std::string>());
                  if (key != channel_keys_.end()) {
                      channels_.erase(key->second);
                      channel_keys_.erase(key);
                  }
                  return Ok{};
              });

    for (const char* method : {"$flush", "$setVisibility"}) {
        router.on(ProxyId::MainThreadLogger, method, ok);
    }
    for (const char* method : {"$reveal", "$close"}) {
        router.on(ProxyId::MainThreadOutputService, method, ok);
    }
    for (const char* method : {"$publicLog", "$publicLog2"}) {
        router.on(ProxyId::MainThreadTelemetry, method, ok);
    }
    router.on(ProxyId::MainThreadExtensionService, "$onWillActivateExtension", ok);
    router.on(ProxyId::MainThreadExtensionService, "$setPerformanceMarks", ok);
}

namespace {

// "2026-11-17 10:12:13.456 [info] text" as written by log output channels.
std::optional<std::pair<LogLevel, std::string>> parse_log_line(const std::string& line) {
    static const std::regex pattern(
        R"(^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}(?:\.\d+)? \[(trace|debug|info|warning|error)\] (.*)$)");
    std::smatch match;
    if (!std::regex_match(line, match, pattern)) {
        return std::nullopt;
    }
    static const std::map<std::string, LogLevel> levels = {
        {"trace", LogLevel::trace},     {"debug", LogLevel::debug}, {"info", LogLevel::info},
        {"warning", LogLevel::warning}, {"error", LogLevel::error},
    };
    return std::pair{levels.at(match[1]), match[2].str()};
}

} // namespace

void LoggingService::track(const std::string& key, std::filesystem::path path, std::string label) {
    const auto [entry, inserted] = channels_.try_emplace(key);
    if (inserted) {
        entry->second.path = std::move(path);
    }
    entry->second.label = std::move(label);
}

void LoggingService::poll() {
    for (auto& [id, channel] : channels_) {
        read_channel(channel);
    }
}

void LoggingService::read_channel(Channel& channel) {
    constexpr std::size_t chunk = 1 << 20;
    while (true) {
        auto bytes = platform::read_file_range(channel.path, channel.offset, chunk);
        if (!bytes || bytes->empty()) {
            return;
        }
        channel.offset += bytes->size();
        channel.pending += *bytes;
        std::size_t line_start = 0;
        for (std::size_t end = channel.pending.find('\n'); end != std::string::npos;
             end = channel.pending.find('\n', line_start)) {
            std::string line = channel.pending.substr(line_start, end - line_start);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            line_start = end + 1;
            if (line.empty()) {
                continue;
            }
            if (auto parsed = parse_log_line(line)) {
                log(parsed->first, channel.label, parsed->second);
            } else {
                log(LogLevel::info, channel.label, line);
            }
        }
        channel.pending.erase(0, line_start);
    }
}

void LoggingService::log(LogLevel level, const std::string& source, const std::string& message) {
    logger_->log(level, source, message);
}

} // namespace anvil::exthost
