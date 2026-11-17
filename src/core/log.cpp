#include "anvil/core/log.hpp"

namespace anvil::core {

std::string_view log_level_name(LogLevel level) {
    switch (level) {
    case LogLevel::off:
        return "off";
    case LogLevel::trace:
        return "trace";
    case LogLevel::debug:
        return "debug";
    case LogLevel::info:
        return "info";
    case LogLevel::warning:
        return "warning";
    case LogLevel::error:
        return "error";
    }
    return "info";
}

} // namespace anvil::core
