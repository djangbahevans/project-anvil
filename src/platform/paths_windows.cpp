#include "anvil/platform/paths.hpp"

#include <shlobj.h>
#include <windows.h>

namespace anvil::platform {

std::expected<std::filesystem::path, Error> user_config_dir() {
    PWSTR raw = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &raw);
    if (FAILED(result)) {
        CoTaskMemFree(raw);
        return std::unexpected(Error{
            ErrorCode::environment_unavailable,
            "the roaming application data folder is unavailable",
        });
    }
    std::filesystem::path base{raw};
    CoTaskMemFree(raw);
    return base / "anvil";
}

} // namespace anvil::platform
