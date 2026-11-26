#include "anvil/platform/editor_defaults.hpp"

namespace anvil::platform {

core::EditorDefaults editor_defaults() {
#if defined(_WIN32)
    return {"Consolas, 'Courier New', monospace", 14, core::LineEnding::crlf};
#elif defined(__APPLE__)
    core::EditorDefaults defaults{"Menlo, Monaco, 'Courier New', monospace", 12,
                                  core::LineEnding::lf};
    defaults.line_height_ratio = 1.5;
    return defaults;
#else
    return {"'Droid Sans Mono', monospace", 14, core::LineEnding::lf};
#endif
}

} // namespace anvil::platform
