#pragma once

#include <filesystem>

namespace anvil::ui {

// The directory with the color themes that ship with the editor (`themes.json` and the theme
// files), or an empty path when there is none.
//
// An installed editor has it at `share/anvil/themes` next to the directory of the executable's
// `bin`; an editor run from its build directory has none of that and reads the source tree's
// `resources/themes`.
std::filesystem::path themes_directory(const std::filesystem::path& application_directory,
                                       const std::filesystem::path& source_directory);

// The same for the running application: the directory of the executable, and the source tree the
// application was built from.
std::filesystem::path themes_directory();

} // namespace anvil::ui
