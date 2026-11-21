#include "anvil/platform/editor_defaults.hpp"

namespace anvil::platform {

core::EditorDefaults editor_defaults() {
#if defined(_WIN32)
    return {"Consolas, 'Courier New', monospace", 14, core::LineEnding::crlf};
#elif defined(__APPLE__)
    return {"Menlo, Monaco, 'Courier New', monospace", 12, core::LineEnding::lf};
#else
    return {"'Droid Sans Mono', monospace", 14, core::LineEnding::lf};
#endif
}

} // namespace anvil::platform
