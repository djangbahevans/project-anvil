#pragma once

#include "anvil/core/log.hpp"
#include "anvil/exthost/request_router.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>

namespace anvil::exthost {

// Everything the extension host reports about itself and about extensions: console output,
// unexpected and activation errors, output channels, and activation timing. These go to a
// core::Logger; telemetry is discarded.
//
// Messages are attributed to a source: the extension host, an extension, or an output channel.
//
// The extension host writes the text of output channels and the extension host's own log to
// files and does not announce each write, so the service reads the files of the registered
// loggers and channels when poll() is called and logs each new line. A line of a log file
// carries a timestamp and a level, which become the level of the log message; other lines are
// logged at info level.
class LoggingService {
public:
    LoggingService(std::shared_ptr<core::Logger> logger, RequestRouter& router);

    // Logs the lines written to the tracked files since the last call.
    void poll();

private:
    struct Channel {
        std::string label;
        std::filesystem::path path;
        // How much of the file has been read, and the end of it that is not yet a whole line.
        std::uint64_t offset = 0;
        std::string pending;
    };

    void log(core::LogLevel level, const std::string& source, const std::string& message);
    void read_channel(Channel& channel);
    // Starts following the file of a logger or channel under `key`, labelled `label`; an entry
    // that already follows the file only takes the new label.
    void track(const std::string& key, std::filesystem::path path, std::string label);

    std::shared_ptr<core::Logger> logger_;
    // Followed files by resource key, and output channel ids by the key of their file.
    std::map<std::string, Channel> channels_;
    std::map<std::string, std::string> channel_keys_;
    std::size_t next_channel_ = 1;
};

} // namespace anvil::exthost
