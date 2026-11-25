#pragma once

#include <QString>
#include <filesystem>
#include <optional>
#include <string>

namespace anvil::ui {

// The versions that the About dialog shows. A version that is unknown is empty.
struct AboutVersions {
    std::string anvil;
    std::string qt_running;
    std::string qt_built_with;
    std::string vscode;
};

// The version of Anvil: the one of the `project()` call in the top-level CMakeLists.txt.
std::string anvil_version();

// The version of VS Code whose extension host runs the extensions: the `version` of the
// `package.json` in its directory. None when the file is missing, is not JSON or has no such text.
std::optional<std::string> read_vscode_version(const std::filesystem::path& vscode_dir);

// The versions of this build and of the VS Code directory (which may be empty).
AboutVersions about_versions(const std::optional<std::filesystem::path>& vscode_dir);

// The text of the dialog. A version that is empty reads "not available".
QString about_text(const AboutVersions& versions);

} // namespace anvil::ui
