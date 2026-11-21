#pragma once

#include "anvil/core/error.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace anvil::exthost {

// How many sessions' logs directories are kept, the current one included.
inline constexpr std::size_t kept_log_sessions = 10;

// The name of the logs directory of one editor session: its UTC start time and process id, such as
// "20261121T143358Z-1234". Names sort in time order.
std::string session_logs_name(std::chrono::system_clock::time_point time, std::int64_t pid);

// True for a name that session_logs_name can produce, with or without the "-<n>" that
// create_session_logs adds to keep the directories of two sessions apart.
bool is_session_logs_name(std::string_view name);

// Creates `logs_root`/`name` for the extension host's log files, and removes the directories of the
// oldest sessions so that at most `keep` remain, the one just created always among them. Only
// directories whose names are session names are removed. Fails with ErrorCode::io_error when the
// directory cannot be created.
std::expected<std::filesystem::path, Error>
create_session_logs(const std::filesystem::path& logs_root, std::string_view name,
                    std::size_t keep = kept_log_sessions);

// Like the above for a session that started at `time` in process `pid`, in a directory that no
// other session uses: when one of that name exists already (a host started again by the same
// process within the second) "-1", "-2" and so on are appended until a free name is found.
std::expected<std::filesystem::path, Error>
create_session_logs(const std::filesystem::path& logs_root,
                    std::chrono::system_clock::time_point time, std::int64_t pid,
                    std::size_t keep = kept_log_sessions);

// The same for a session that starts now, in this process.
std::expected<std::filesystem::path, Error>
create_session_logs(const std::filesystem::path& logs_root);

} // namespace anvil::exthost
