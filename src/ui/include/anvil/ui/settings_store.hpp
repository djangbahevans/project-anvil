#pragma once

#include "anvil/core/editor_settings.hpp"
#include "anvil/core/log.hpp"
#include "anvil/core/settings.hpp"

#include <QFileSystemWatcher>
#include <QObject>
#include <QString>
#include <QTimer>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

namespace anvil::ui {

// The user's settings file, read at startup and again whenever it changes on disk. A missing file
// means the defaults, and nothing is ever written. A file that cannot be read, parsed or
// validated leaves the previous settings in force and is reported through `load_failed`, once per
// distinct problem and in the log.
class SettingsStore : public QObject {
    Q_OBJECT

public:
    struct Options {
        // The settings file; empty means there is none, and the settings stay at the defaults.
        std::filesystem::path file;
        core::EditorDefaults defaults;
        // Receives the problems found; discarded when unset.
        std::shared_ptr<core::Logger> logger;
        // Changes to the file are collected for this long before it is read, so that an editor's
        // save, which can touch the file several times, causes one reload.
        std::chrono::milliseconds debounce{150};
    };

    explicit SettingsStore(Options options, QObject* parent = nullptr);

    // Reads the file and starts watching it. A problem is reported; `changed` is not, because the
    // caller applies the settings once after starting.
    void start();

    const core::Settings& settings() const {
        return settings_;
    }
    const core::EditorSettings& editor() const {
        return editor_;
    }

signals:
    // A reload produced settings that differ from the ones in force.
    void changed();
    // The file could not be used. `message` names the file and the position or key at fault. The
    // same problem is not reported again until the file has been valid in between.
    void load_failed(const QString& message);

private:
    void reload(bool notify);
    void watch();
    void report(const std::string& problem);

    Options options_;
    core::Settings settings_;
    core::EditorSettings editor_;
    std::string last_problem_;
    QFileSystemWatcher watcher_;
    QTimer debounce_;
};

} // namespace anvil::ui
