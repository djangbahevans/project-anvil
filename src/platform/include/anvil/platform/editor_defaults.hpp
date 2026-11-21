#pragma once

#include "anvil/core/editor_settings.hpp"

namespace anvil::platform {

// VS Code's defaults for this platform: the font family and size (Droid Sans Mono 14 on Linux,
// Menlo 12 on macOS, Consolas 14 on Windows) and the line ending that `files.eol: "auto"` stands
// for (CRLF on Windows, LF elsewhere).
core::EditorDefaults editor_defaults();

} // namespace anvil::platform
