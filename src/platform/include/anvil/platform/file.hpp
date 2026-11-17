#pragma once

#include "anvil/core/error.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace anvil::platform {

// The entire contents of a file.
std::expected<std::string, Error> read_file(const std::filesystem::path& path);

// Up to `length` bytes of a file starting at `offset`; fewer when the file ends first, and none
// when `offset` is at or past the end.
std::expected<std::string, Error> read_file_range(const std::filesystem::path& path,
                                                  std::uint64_t offset, std::size_t length);

// Replaces the contents of a file. The data is written to a temporary file next to it and moved
// into place, so a failure part way never leaves a truncated file behind.
std::expected<void, Error> write_file(const std::filesystem::path& path, std::string_view data);

} // namespace anvil::platform
