#pragma once

#include "anvil/core/keybindings.hpp"
#include "anvil/core/log.hpp"
#include "anvil/ui/watched_file.hpp"

#include <QObject>
#include <QString>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace anvil::ui {

// The user's `keybindings.json`, read at startup and again whenever it changes on disk. A missing
// file means no bindings of the user's own, and nothing is ever written. A file that cannot be
// parsed leaves the previous bindings in force. An entry that cannot be used is left out and the
// others apply. Each problem is reported through `load_failed`, once until the file has been
// valid in between, and in the log.
class KeybindingsStore : public QObject {
    Q_OBJECT

public:
    struct Options {
        // The file; empty means there is none.
        std::filesystem::path file;
        // Receives the problems found; discarded when unset.
        std::shared_ptr<core::Logger> logger;
        std::chrono::milliseconds debounce{150};
    };

    explicit KeybindingsStore(Options options, QObject* parent = nullptr);

    // Reads the file and starts watching it. A problem is reported; `changed` is not, because the
    // caller applies the bindings once after starting.
    void start();

    // The usable bindings of the file, with the source `user`.
    const std::vector<core::Keybinding>& bindings() const {
        return bindings_;
    }

signals:
    // A reload read different text from the one in force and the bindings may differ.
    void changed();
    // `message` names the file and the first problem, and how many more there are.
    void load_failed(const QString& message);

private:
    void reload(bool notify);
    void report(const std::vector<std::string>& problems);

    Options options_;
    WatchedFile file_;
    std::vector<core::Keybinding> bindings_;
    std::string text_;
    std::string last_problem_;
};

} // namespace anvil::ui
