#include "anvil/platform/file.hpp"

#include <fstream>
#include <iterator>
#include <system_error>

namespace anvil::platform {
namespace {

Error io_error(const std::string& what, const std::filesystem::path& path,
               const std::string& detail) {
    return Error{ErrorCode::io_error, what + " " + path.string() + ": " + detail};
}

} // namespace

std::expected<std::string, Error> read_file(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        return std::unexpected(io_error("cannot read", path, "not a regular file"));
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::unexpected(io_error("cannot open", path, "the file cannot be opened"));
    }
    std::string contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (file.bad()) {
        return std::unexpected(io_error("cannot read", path, "a read error occurred"));
    }
    return contents;
}

std::expected<void, Error> write_file(const std::filesystem::path& path, std::string_view data) {
    std::filesystem::path temporary = path;
    temporary += ".anvil-tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            return std::unexpected(
                io_error("cannot write", temporary, "the file cannot be created"));
        }
        file.write(data.data(), static_cast<std::streamsize>(data.size()));
        file.flush();
        if (!file) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return std::unexpected(io_error("cannot write", temporary, "a write error occurred"));
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return std::unexpected(io_error("cannot replace", path, error.message()));
    }
    return {};
}

} // namespace anvil::platform
