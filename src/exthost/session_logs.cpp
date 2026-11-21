#include "anvil/exthost/session_logs.hpp"

#include "anvil/platform/process.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <vector>

namespace anvil::exthost {
namespace {

bool all_digits(std::string_view text) {
    return !text.empty() && std::ranges::all_of(text, [](char c) {
        return std::isdigit(static_cast<unsigned char>(c)) != 0;
    });
}

} // namespace

std::string session_logs_name(std::chrono::system_clock::time_point time, std::int64_t pid) {
    return std::format("{:%Y%m%dT%H%M%S}Z-{}", std::chrono::floor<std::chrono::seconds>(time), pid);
}

bool is_session_logs_name(std::string_view name) {
    // "YYYYMMDD" "T" "HHMMSS" "Z" "-" pid, and optionally "-" counter
    if (name.size() < 18 || !all_digits(name.substr(0, 8)) || name[8] != 'T' ||
        !all_digits(name.substr(9, 6)) || name[15] != 'Z' || name[16] != '-') {
        return false;
    }
    const std::string_view rest = name.substr(17);
    const std::size_t dash = rest.find('-');
    return dash == std::string_view::npos
               ? all_digits(rest)
               : all_digits(rest.substr(0, dash)) && all_digits(rest.substr(dash + 1));
}

std::expected<std::filesystem::path, Error>
create_session_logs(const std::filesystem::path& logs_root, std::string_view name,
                    std::size_t keep) {
    const std::filesystem::path directory = logs_root / name;
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error || !std::filesystem::is_directory(directory)) {
        return std::unexpected(Error{
            ErrorCode::io_error,
            "cannot create the logs directory " + directory.string() +
                (error ? ": " + error.message() : ": it exists and is not a directory"),
        });
    }

    std::vector<std::string> older;
    for (std::filesystem::directory_iterator it(logs_root, error), end; !error && it != end;
         it.increment(error)) {
        const std::string entry = it->path().filename().string();
        if (entry != name && is_session_logs_name(entry) && it->is_directory(error)) {
            older.push_back(entry);
        }
    }
    std::ranges::sort(older, std::greater<>());
    const std::size_t others_to_keep = keep > 0 ? keep - 1 : 0;
    for (std::size_t i = others_to_keep; i < older.size(); ++i) {
        std::filesystem::remove_all(logs_root / older[i], error);
    }
    return directory;
}

std::expected<std::filesystem::path, Error>
create_session_logs(const std::filesystem::path& logs_root,
                    std::chrono::system_clock::time_point time, std::int64_t pid,
                    std::size_t keep) {
    const std::string base = session_logs_name(time, pid);
    std::string name = base;
    for (int attempt = 1; std::filesystem::exists(logs_root / name) && attempt < 10000; ++attempt) {
        name = base + "-" + std::to_string(attempt);
    }
    return create_session_logs(logs_root, name, keep);
}

std::expected<std::filesystem::path, Error>
create_session_logs(const std::filesystem::path& logs_root) {
    return create_session_logs(logs_root, std::chrono::system_clock::now(),
                               platform::current_process_id());
}

} // namespace anvil::exthost
