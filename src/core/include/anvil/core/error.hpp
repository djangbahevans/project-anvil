#pragma once

#include <string>

namespace anvil {

enum class ErrorCode {
    invalid_encoding,
    environment_unavailable,
    protocol_error,
    io_error,
    unsupported,
    invalid_argument,
    // An operation gave up because it exceeded a resource bound that depends on its input, such
    // as the backtracking limit of a regular expression on a particular text. The same request may
    // succeed on other input.
    limit_exceeded,
    // What a file system operation reports for the cases that callers tell apart; any other
    // failure of the operating system is `io_error`.
    not_found,
    already_exists,
    not_a_directory,
    is_a_directory,
    permission_denied,
};

struct Error {
    ErrorCode code;
    std::string message;
};

} // namespace anvil
