#include "anvil/ui/keybindings_store.hpp"

#include "anvil/core/jsonc.hpp"
#include "anvil/platform/file.hpp"

namespace anvil::ui {

KeybindingsStore::KeybindingsStore(Options options, QObject* parent)
    : QObject(parent), options_(std::move(options)), file_(options_.file, options_.debounce, this) {
    if (!options_.logger) {
        options_.logger = std::make_shared<core::NullLogger>();
    }
    connect(&file_, &WatchedFile::modified, this, [this] { reload(true); });
}

void KeybindingsStore::start() {
    if (options_.file.empty()) {
        return;
    }
    reload(false);
    file_.start();
}

void KeybindingsStore::reload(bool notify) {
    std::string text;
    std::error_code ignored;
    if (std::filesystem::exists(options_.file, ignored)) {
        auto bytes = platform::read_file(options_.file);
        if (!bytes) {
            report({bytes.error().message});
            return;
        }
        text = std::move(*bytes);
    }

    std::vector<core::Keybinding> bindings;
    std::vector<std::string> problems;
    if (text.find_first_not_of(" \t\r\n") != std::string::npos) {
        auto json = core::parse_jsonc(text);
        if (!json) {
            report({json.error().message});
            return;
        }
        auto file = core::read_keybindings(*json, core::KeybindingSource::user);
        if (!file) {
            report({file.error().message});
            return;
        }
        bindings = std::move(file->bindings);
        problems = std::move(file->problems);
    }

    report(problems);
    const bool differs = text != text_;
    text_ = std::move(text);
    bindings_ = std::move(bindings);
    if (differs && notify) {
        emit changed();
    }
}

void KeybindingsStore::report(const std::vector<std::string>& problems) {
    if (problems.empty()) {
        last_problem_.clear();
        return;
    }
    std::string summary = problems.front();
    if (problems.size() > 1) {
        summary += " (and " + std::to_string(problems.size() - 1) + " more)";
    }
    if (summary == last_problem_) {
        return;
    }
    last_problem_ = summary;
    for (const std::string& problem : problems) {
        options_.logger->log(core::LogLevel::error, "keybindings",
                             options_.file.string() + ": " + problem);
    }
    emit load_failed(QString::fromStdString(options_.file.filename().string() + ": " + summary));
}

} // namespace anvil::ui
