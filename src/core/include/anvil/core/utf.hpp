#pragma once

#include "anvil/core/error.hpp"

#include <cstddef>
#include <expected>
#include <string_view>

namespace anvil::core {

// Number of UTF-16 code units needed to encode `utf8`. Extension and LSP
// positions count in these units while the text buffer stores UTF-8.
// Fails with ErrorCode::invalid_encoding on malformed input, reporting the
// byte offset of the first invalid sequence.
std::expected<std::size_t, Error> utf16_length(std::string_view utf8);

} // namespace anvil::core
