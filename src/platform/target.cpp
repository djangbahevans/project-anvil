#include "anvil/platform/target.hpp"

namespace anvil::platform {

std::string vscode_target_platform() {
#if defined(_WIN32)
    constexpr const char* os = "win32";
#elif defined(__APPLE__)
    constexpr const char* os = "darwin";
#elif defined(__linux__)
    constexpr const char* os = "linux";
#else
    constexpr const char* os = "unknown";
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
    constexpr const char* arch = "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    constexpr const char* arch = "x64";
#else
    constexpr const char* arch = "unknown";
#endif
    return std::string(os) + "-" + arch;
}

} // namespace anvil::platform
