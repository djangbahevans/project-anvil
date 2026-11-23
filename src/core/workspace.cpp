#include "anvil/core/workspace.hpp"

#include <cstdint>

namespace anvil::core {
namespace {

std::string fnv1a_hex(const std::string& bytes) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char byte : bytes) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= 1099511628211ULL;
    }
    static constexpr char digits[] = "0123456789abcdef";
    std::string text(16, '0');
    for (int i = 15; i >= 0; --i) {
        text[static_cast<std::size_t>(i)] = digits[hash & 0xf];
        hash >>= 4;
    }
    return text;
}

} // namespace

std::expected<Workspace, Error> Workspace::open(const std::filesystem::path& folder) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(fs::absolute(folder, ec), ec);
    if (ec) {
        return std::unexpected(Error{ErrorCode::io_error, folder.string() + ": " + ec.message()});
    }
    if (!fs::is_directory(canonical, ec)) {
        return std::unexpected(
            Error{fs::exists(canonical, ec) ? ErrorCode::not_a_directory : ErrorCode::not_found,
                  folder.string() + " is not a folder"});
    }
    Workspace workspace;
    workspace.name = canonical.has_filename() ? canonical.filename().generic_string()
                                              : canonical.generic_string();
    workspace.id = fnv1a_hex(canonical.generic_string());
    workspace.folder = std::move(canonical);
    return workspace;
}

LaunchArguments parse_launch_arguments(const std::vector<std::string>& arguments) {
    LaunchArguments result;
    for (const std::string& argument : arguments) {
        std::error_code ec;
        const std::filesystem::path path(argument);
        const bool names_folder =
            std::filesystem::is_directory(path, ec) ||
            (!argument.empty() && (argument.back() == '/' ||
                                   argument.back() == std::filesystem::path::preferred_separator));
        if (!names_folder) {
            result.files.push_back(path);
            continue;
        }
        if (result.workspace) {
            result.problems.push_back(argument + ": only one folder can be opened; ignored");
            continue;
        }
        if (auto workspace = Workspace::open(path)) {
            result.workspace = std::move(*workspace);
        } else {
            result.problems.push_back(workspace.error().message + "; ignored");
        }
    }
    return result;
}

} // namespace anvil::core
