#include "anvil/platform/working_directory.hpp"

#include <cstdlib>
#include <format>
#include <optional>
#include <string>

namespace anvil::platform {
namespace {

std::optional<std::string> environment_variable(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
        return std::nullopt;
    }
    std::string result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::optional<std::string>(value) : std::nullopt;
#endif
}

std::vector<std::filesystem::path> default_fallbacks() {
    std::vector<std::filesystem::path> fallbacks;
#ifdef _WIN32
    const auto home = environment_variable("USERPROFILE");
#else
    const auto home = environment_variable("HOME");
#endif
    if (home && !home->empty()) {
        fallbacks.emplace_back(*home);
    }
    std::error_code error;
    if (auto temporary = std::filesystem::temp_directory_path(error); !error) {
        fallbacks.push_back(std::move(temporary));
    }
    return fallbacks;
}

} // namespace

std::expected<WorkingDirectory, Error>
ensure_working_directory(const std::vector<std::filesystem::path>& fallbacks) {
    std::error_code error;
    if (auto current = std::filesystem::current_path(error); !error) {
        return WorkingDirectory{std::move(current), false};
    }
    for (const auto& candidate : fallbacks) {
        std::filesystem::current_path(candidate, error);
        if (error) {
            continue;
        }
        if (auto current = std::filesystem::current_path(error); !error) {
            return WorkingDirectory{std::move(current), true};
        }
    }
    return std::unexpected(Error{
        ErrorCode::environment_unavailable,
        std::format("the working directory no longer exists and no fallback directory could be "
                    "entered ({} tried)",
                    fallbacks.size()),
    });
}

std::expected<WorkingDirectory, Error> ensure_working_directory() {
    return ensure_working_directory(default_fallbacks());
}

} // namespace anvil::platform
