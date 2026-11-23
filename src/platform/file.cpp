#include "anvil/platform/file.hpp"

#include <cerrno>
#include <fstream>
#include <iterator>
#include <system_error>

namespace anvil::platform {
namespace {

Error io_error(const std::string& what, const std::filesystem::path& path,
               const std::string& detail, ErrorCode code = ErrorCode::io_error) {
    return Error{code, what + " " + path.string() + ": " + detail};
}

// What a failed open reports, by the error number that the failed call left behind.
Error open_error(const std::string& what, const std::filesystem::path& path,
                 const std::string& detail, int error_number) {
    const std::error_code error(error_number, std::generic_category());
    ErrorCode code = ErrorCode::io_error;
    if (error == std::errc::no_such_file_or_directory) {
        code = ErrorCode::not_found;
    } else if (error == std::errc::permission_denied ||
               error == std::errc::operation_not_permitted) {
        code = ErrorCode::permission_denied;
    } else if (error == std::errc::is_a_directory) {
        code = ErrorCode::is_a_directory;
    } else if (error == std::errc::not_a_directory) {
        code = ErrorCode::not_a_directory;
    }
    return io_error(what, path, detail, code);
}

} // namespace

std::expected<std::string, Error> read_file(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        return std::unexpected(io_error("cannot read", path, "not a regular file"));
    }
    errno = 0;
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::unexpected(open_error("cannot open", path, "the file cannot be opened", errno));
    }
    std::string contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (file.bad()) {
        return std::unexpected(io_error("cannot read", path, "a read error occurred"));
    }
    return contents;
}

std::expected<std::string, Error> read_file_range(const std::filesystem::path& path,
                                                  std::uint64_t offset, std::size_t length) {
    errno = 0;
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::unexpected(open_error("cannot open", path, "the file cannot be opened", errno));
    }
    file.seekg(static_cast<std::streamoff>(offset));
    std::string contents(length, '\0');
    file.read(contents.data(), static_cast<std::streamsize>(length));
    if (file.bad()) {
        return std::unexpected(io_error("cannot read", path, "a read error occurred"));
    }
    contents.resize(static_cast<std::size_t>(file.gcount()));
    return contents;
}

std::expected<void, Error> write_file(const std::filesystem::path& path, std::string_view data) {
    std::filesystem::path temporary = path;
    temporary += ".anvil-tmp";
    {
        errno = 0;
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            return std::unexpected(
                open_error("cannot write", temporary, "the file cannot be created", errno));
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
