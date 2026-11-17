#pragma once

#include <string>

namespace anvil {

enum class ErrorCode {
    invalid_encoding,
    environment_unavailable,
};

struct Error {
    ErrorCode code;
    std::string message;
};

} // namespace anvil
