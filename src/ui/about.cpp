#include "anvil/ui/about.hpp"

#include "anvil/core/jsonc.hpp"
#include "anvil/platform/file.hpp"
#include "anvil/ui/project_links.hpp"

#include <QCoreApplication>
#include <QtGlobal>

namespace anvil::ui {

std::string anvil_version() {
    return ANVIL_VERSION;
}

std::optional<std::string> read_vscode_version(const std::filesystem::path& vscode_dir) {
    const auto text = platform::read_file(vscode_dir / "package.json");
    if (!text) {
        return std::nullopt;
    }
    const auto package = core::parse_jsonc(*text);
    if (!package || !package->is_object()) {
        return std::nullopt;
    }
    const auto version = package->find("version");
    if (version == package->end() || !version->is_string() || version->get<std::string>().empty()) {
        return std::nullopt;
    }
    return version->get<std::string>();
}

AboutVersions about_versions(const std::optional<std::filesystem::path>& vscode_dir) {
    AboutVersions versions;
    versions.anvil = anvil_version();
    versions.qt_running = qVersion();
    versions.qt_built_with = QT_VERSION_STR;
    if (vscode_dir) {
        versions.vscode = read_vscode_version(*vscode_dir).value_or(std::string());
    }
    return versions;
}

QString about_text(const AboutVersions& versions) {
    const auto version = [](const std::string& value) {
        return value.empty() ? QCoreApplication::translate("About", "not available")
                             : QString::fromStdString(value);
    };
    const QString qt_running = version(versions.qt_running);
    const QString qt_built_with = version(versions.qt_built_with);
    const QString qt = qt_running == qt_built_with
                           ? qt_running
                           : QCoreApplication::translate("About", "%1 (built with %2)")
                                 .arg(qt_running, qt_built_with);
    return QCoreApplication::translate("About", "Anvil\n\n"
                                                "Version: %1\n"
                                                "Qt: %2\n"
                                                "VS Code extension host: %3\n\n"
                                                "Anvil is released under the MIT license.\n%4")
        .arg(version(versions.anvil), qt, version(versions.vscode),
             QString::fromUtf8(project_url.data(), static_cast<qsizetype>(project_url.size())));
}

} // namespace anvil::ui
