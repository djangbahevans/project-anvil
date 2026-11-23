#include "anvil/core/log.hpp"
#include "anvil/core/workspace.hpp"
#include "anvil/platform/paths.hpp"
#include "anvil/platform/working_directory.hpp"
#include "anvil/ui/main_window.hpp"

#include <QApplication>
#include <QDir>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

using anvil::ui::ExtensionHostBridge;

// The extension host runs when a Node.js binary and the transpiled VS Code checkout are found:
//   ANVIL_NODE         the node binary (default: node on PATH)
//   ANVIL_VSCODE_DIR   the checkout (default: .deps/vscode of the source tree)
//   ANVIL_EXTENSIONS   unpacked extension directories, separated like PATH entries
//   ANVIL_EXTENSION_HOST=off  runs the editor without it
std::optional<ExtensionHostBridge::Config> extension_host_config() {
    const QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    if (environment.value("ANVIL_EXTENSION_HOST") == "off") {
        return std::nullopt;
    }
    const std::filesystem::path vscode_dir =
        environment.contains("ANVIL_VSCODE_DIR")
            ? environment.value("ANVIL_VSCODE_DIR").toStdString()
            : std::string(ANVIL_DEFAULT_VSCODE_DIR);
    if (!std::filesystem::exists(vscode_dir / "out" / "bootstrap-fork.js")) {
        return std::nullopt;
    }
    const QString node = environment.value("ANVIL_NODE", QStandardPaths::findExecutable("node"));
    if (node.isEmpty()) {
        return std::nullopt;
    }

    ExtensionHostBridge::Config config;
    config.node_executable = node.toStdString();
    config.vscode_dir = vscode_dir;
    for (const QString& directory :
         environment.value("ANVIL_EXTENSIONS").split(QDir::listSeparator(), Qt::SkipEmptyParts)) {
        config.extension_dirs.emplace_back(directory.toStdString());
    }
    if (const auto config_dir = anvil::platform::user_config_dir()) {
        config.storage_dir = *config_dir / "extension-host";
        std::filesystem::create_directories(config.storage_dir);
    } else {
        config.storage_dir = std::filesystem::temp_directory_path() / "anvil-extension-host";
        std::filesystem::create_directories(config.storage_dir);
    }
    return config;
}

} // namespace

int main(int argc, char* argv[]) {
    // Before Qt starts: Fontconfig reads the working directory while it loads its configuration,
    // and the extension host and relative file arguments need one that exists.
    if (const auto directory = anvil::platform::ensure_working_directory()) {
        if (directory->changed) {
            std::cerr << "anvil: the working directory no longer exists; using "
                      << directory->directory.string() << '\n';
        }
    } else {
        std::cerr << "anvil: " << directory.error().message << '\n';
    }

    QApplication app(argc, argv);
    QApplication::setApplicationName("Anvil");

    std::vector<std::string> arguments;
    for (const QString& argument : app.arguments().mid(1)) {
        arguments.push_back(argument.toStdString());
    }
    anvil::core::LaunchArguments launch = anvil::core::parse_launch_arguments(arguments);
    for (const std::string& problem : launch.problems) {
        std::cerr << "anvil: " << problem << '\n';
    }

    anvil::ui::MainWindowOptions options;
    options.workspace = launch.workspace;
    options.extension_host = extension_host_config();
    if (const auto config_dir = anvil::platform::user_config_dir()) {
        options.settings_file = *config_dir / "settings.json";
        options.keybindings_file = *config_dir / "keybindings.json";
    }
    options.logger =
        std::make_shared<anvil::core::StreamLogger>(std::cerr, anvil::core::LogLevel::info);
    anvil::ui::MainWindow window(std::move(options));

    for (const std::filesystem::path& file : launch.files) {
        window.open_file(QString::fromStdString(file.string()));
    }
    window.show();
    return app.exec();
}
