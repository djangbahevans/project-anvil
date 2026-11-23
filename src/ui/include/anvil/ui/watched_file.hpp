#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QTimer>
#include <chrono>
#include <filesystem>

namespace anvil::ui {

// Tells when a file that may not exist yet is created, changed, replaced or deleted. It watches the
// file and the nearest existing directory above it, and watches both again after every change, so
// that an editor which saves by writing a temporary file and renaming it over the original is
// followed. Changes are collected for the debounce time, so that a save that touches the file
// several times is one `modified`.
class WatchedFile : public QObject {
    Q_OBJECT

public:
    WatchedFile(std::filesystem::path file, std::chrono::milliseconds debounce,
                QObject* parent = nullptr);

    // Starts watching. Nothing is emitted for the state of the file at this time.
    void start();

signals:
    void modified();

private:
    void watch();

    std::filesystem::path file_;
    QFileSystemWatcher watcher_;
    QTimer debounce_;
};

} // namespace anvil::ui
