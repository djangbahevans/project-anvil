#pragma once

#include <string>

namespace anvil::platform {

// The VS Code target platform identifier of this build, such as "linux-x64" or "darwin-arm64".
// Extension manifests and the extension host use it to select platform-specific extensions.
std::string vscode_target_platform();

} // namespace anvil::platform
