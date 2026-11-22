#include "anvil/ui/resources.hpp"

#include <QCoreApplication>
#include <system_error>

namespace anvil::ui {
namespace {

bool has_themes(const std::filesystem::path& directory) {
    std::error_code error;
    return !directory.empty() && std::filesystem::exists(directory / "themes.json", error);
}

} // namespace

std::filesystem::path themes_directory(const std::filesystem::path& application_directory,
                                       const std::filesystem::path& source_directory) {
    if (!application_directory.empty()) {
        const auto installed =
            (application_directory / ".." / "share" / "anvil" / "themes").lexically_normal();
        if (has_themes(installed)) {
            return installed;
        }
    }
    return has_themes(source_directory) ? source_directory : std::filesystem::path();
}

std::filesystem::path themes_directory() {
    std::filesystem::path application;
    if (QCoreApplication::instance()) {
        application = QCoreApplication::applicationDirPath().toStdString();
    }
    return themes_directory(application, ANVIL_THEMES_SOURCE_DIR);
}

} // namespace anvil::ui
