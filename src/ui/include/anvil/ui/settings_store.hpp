#pragma once

#include "anvil/core/editor_settings.hpp"
#include "anvil/core/log.hpp"
#include "anvil/core/settings.hpp"
#include "anvil/ui/watched_file.hpp"

#include <QObject>
#include <QString>
#include <chrono>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace anvil::ui {

// The user's settings file, read at startup and again whenever it changes on disk. A missing file
// means the defaults. A file that cannot be read, parsed or validated leaves the previous settings
// in force and is reported through `load_failed`, once per distinct problem and in the log.
//
// The file is written only when a command asks for a change (`change`), by editing its text in
// place ([0017](docs/adr/0017-commands-change-settings-by-editing-the-file.md)); a change that the
// file cannot take is made for the running window only, in an override that is kept above the
// file until the same key is changed again.
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
        // Whether the file is open in the window with changes that are not saved; the store does
        // not write over them. Nothing is open when unset.
        std::function<bool(const std::filesystem::path&)> is_unsaved{};
        // Reads a file; `platform::read_file` when unset. The store reads the settings file twice
        // around an edit, and a test changes the file between the two reads through this.
        std::function<std::expected<std::string, Error>(const std::filesystem::path&)> read_file{};
    };

    // What `change` did.
    struct ChangeResult {
        enum class Outcome {
            // The file was edited and the new settings are in force.
            written,
            // The file was left as it is, and the setting is changed for the running window.
            session,
            // The value is not valid for the key, and nothing changed.
            rejected,
        };

        Outcome outcome = Outcome::written;
        // For `session` and `rejected`: why the file was not changed, or why the value was not
        // accepted, as a phrase that begins with the file name.
        std::string reason{};
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

    // Sets the setting `key` to `value`: the text of the settings file is edited in place and the
    // new settings are in force at once, with one `changed` and none more when the watcher reads
    // the file back. The file is left as it is, and the change is made for the running window
    // only, when there is no settings file, the file does not parse or validate, is open with
    // unsaved changes, changed between the two reads that surround the edit, or cannot be written.
    // An override lasts until `change` is called for the same key again.
    ChangeResult change(std::string_view key, nlohmann::json value);

signals:
    // A reload produced settings that differ from the ones in force.
    void changed();
    // The file could not be used. `message` names the file and the position or key at fault. The
    // same problem is not reported again until the file has been valid in between.
    void load_failed(const QString& message);

private:
    void reload(bool notify);
    void report(const std::string& problem);
    // The settings and the editor's settings with the overrides over `loaded`, or why they are
    // not valid.
    struct Effective {
        core::Settings settings;
        core::EditorSettings editor;
    };
    std::expected<Effective, std::string>
    with_overrides(const core::Settings& loaded,
                   const std::map<std::string, nlohmann::json>& overrides) const;
    // Makes `loaded` and the overrides the settings in force; `changed` when they differ.
    void adopt(core::Settings loaded, Effective effective);
    // Edits the file; the reason it was left alone, or nothing when it was written.
    std::optional<std::string> write_change(const std::string& key, const nlohmann::json& value);
    std::expected<std::string, Error> read(const std::filesystem::path& path) const;

    Options options_;
    // What the file says, and what is in force: the file with the overrides over it.
    core::Settings loaded_;
    std::map<std::string, nlohmann::json> overrides_;
    core::Settings settings_;
    core::EditorSettings editor_;
    std::string last_problem_;
    WatchedFile file_;
};

} // namespace anvil::ui
