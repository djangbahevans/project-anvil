#include "anvil/exthost/contributions.hpp"

#include <fstream>
#include <nlohmann/json.hpp>

namespace anvil::exthost {
namespace {

using nlohmann::json;

std::optional<json> read_json(const std::filesystem::path& file) {
    std::ifstream stream(file);
    if (!stream) {
        return std::nullopt;
    }
    json value = json::parse(stream, nullptr, false);
    if (value.is_discarded()) {
        return std::nullopt;
    }
    return value;
}

// The strings of `package.nls.json`, which `%key%` in the manifest refers to.
class Translations {
public:
    explicit Translations(const std::filesystem::path& directory) {
        if (auto file = read_json(directory / "package.nls.json"); file && file->is_object()) {
            strings_ = std::move(*file);
        }
    }

    // A string that is `%key%` becomes the translation of the key, or stays as written when the
    // file has none.
    std::string apply(const std::string& text) const {
        if (text.size() > 2 && text.front() == '%' && text.back() == '%') {
            const auto found = strings_.find(text.substr(1, text.size() - 2));
            if (found != strings_.end()) {
                if (found->is_string()) {
                    return found->get<std::string>();
                }
                if (found->is_object() && found->contains("message") &&
                    (*found)["message"].is_string()) {
                    return (*found)["message"].get<std::string>();
                }
            }
        }
        return text;
    }

private:
    json strings_ = json::object();
};

// A title or category is a string, or an object with the translated `value` and the `original`.
std::optional<std::string> localized(const json& value, const Translations& translations) {
    if (value.is_string()) {
        return translations.apply(value.get<std::string>());
    }
    if (value.is_object() && value.contains("value") && value["value"].is_string()) {
        return translations.apply(value["value"].get<std::string>());
    }
    return std::nullopt;
}

class SectionReader {
public:
    SectionReader(core::Logger& logger, const std::string& extension)
        : logger_(logger), extension_(extension) {}

    void problem(const std::string& message) const {
        logger_.log(core::LogLevel::warning, "extensions", extension_ + ": " + message);
    }

    // The entries of `contributes.<name>`, or none when the section is missing or not an array.
    const json* array(const json& contributes, const std::string& name) const {
        const auto found = contributes.find(name);
        if (found == contributes.end()) {
            return nullptr;
        }
        if (!found->is_array()) {
            problem("contributes." + name + " is not an array");
            return nullptr;
        }
        return &*found;
    }

    // The string at `key` of an entry, empty when absent; nullopt, with a problem logged, when it
    // is there and not a string.
    std::optional<std::string> text(const json& entry, const std::string& key,
                                    const std::string& section, std::size_t index) const {
        const auto found = entry.find(key);
        if (found == entry.end() || found->is_null()) {
            return std::string();
        }
        if (!found->is_string()) {
            problem("contributes." + section + "[" + std::to_string(index) + "]." + key +
                    " is not a string");
            return std::nullopt;
        }
        return found->get<std::string>();
    }

    const json* object(const json& entry, const std::string& section, std::size_t index) const {
        if (!entry.is_object()) {
            problem("contributes." + section + "[" + std::to_string(index) + "] is not an object");
            return nullptr;
        }
        return &entry;
    }

private:
    core::Logger& logger_;
    const std::string& extension_;
};

void read_commands(const json& contributes, const SectionReader& reader,
                   const Translations& translations, const std::string& extension,
                   Contributions& out) {
    const json* section = reader.array(contributes, "commands");
    if (!section) {
        return;
    }
    std::size_t index = 0;
    for (const json& entry : *section) {
        const std::size_t at = index++;
        if (!reader.object(entry, "commands", at)) {
            continue;
        }
        const auto command = reader.text(entry, "command", "commands", at);
        const auto enablement = reader.text(entry, "enablement", "commands", at);
        if (!command || !enablement) {
            continue;
        }
        if (command->empty()) {
            reader.problem("contributes.commands[" + std::to_string(at) + "] has no command");
            continue;
        }
        const auto title =
            entry.contains("title") ? localized(entry["title"], translations) : std::nullopt;
        if (!title) {
            reader.problem("contributes.commands[" + std::to_string(at) + "] has no title");
            continue;
        }
        std::string category;
        if (entry.contains("category") && !entry["category"].is_null()) {
            const auto read = localized(entry["category"], translations);
            if (!read) {
                reader.problem("contributes.commands[" + std::to_string(at) +
                               "].category is not a string");
                continue;
            }
            category = *read;
        }
        out.commands.push_back({extension, *command, *title, std::move(category), *enablement});
    }
}

void read_keybindings(const json& contributes, const SectionReader& reader,
                      const std::string& extension, Contributions& out) {
    const json* section = reader.array(contributes, "keybindings");
    if (!section) {
        return;
    }
    std::size_t index = 0;
    for (const json& entry : *section) {
        const std::size_t at = index++;
        if (!reader.object(entry, "keybindings", at)) {
            continue;
        }
        ContributedKeybinding binding;
        binding.extension = extension;
        const auto command = reader.text(entry, "command", "keybindings", at);
        const auto key = reader.text(entry, "key", "keybindings", at);
        const auto mac = reader.text(entry, "mac", "keybindings", at);
        const auto linux = reader.text(entry, "linux", "keybindings", at);
        const auto win = reader.text(entry, "win", "keybindings", at);
        const auto when = reader.text(entry, "when", "keybindings", at);
        if (!command || !key || !mac || !linux || !win || !when) {
            continue;
        }
        if (command->empty()) {
            reader.problem("contributes.keybindings[" + std::to_string(at) + "] has no command");
            continue;
        }
        binding.command = *command;
        binding.key = *key;
        binding.mac = *mac;
        binding.linux = *linux;
        binding.win = *win;
        binding.when = *when;
        if (entry.contains("args") && !entry["args"].is_null()) {
            binding.args = entry["args"].dump();
        }
        out.keybindings.push_back(std::move(binding));
    }
}

void read_palette_rules(const json& contributes, const SectionReader& reader,
                        const std::string& extension, Contributions& out) {
    const auto menus = contributes.find("menus");
    if (menus == contributes.end()) {
        return;
    }
    if (!menus->is_object()) {
        reader.problem("contributes.menus is not an object");
        return;
    }
    const json* section = reader.array(*menus, "commandPalette");
    if (!section) {
        return;
    }
    std::size_t index = 0;
    for (const json& entry : *section) {
        const std::size_t at = index++;
        if (!reader.object(entry, "menus.commandPalette", at)) {
            continue;
        }
        const auto command = reader.text(entry, "command", "menus.commandPalette", at);
        const auto when = reader.text(entry, "when", "menus.commandPalette", at);
        if (!command || !when) {
            continue;
        }
        if (command->empty()) {
            reader.problem("contributes.menus.commandPalette[" + std::to_string(at) +
                           "] has no command");
            continue;
        }
        out.palette_rules.push_back({extension, *command, *when});
    }
}

} // namespace

std::string ContributedCommand::label() const {
    return category.empty() ? title : category + ": " + title;
}

Contributions read_contributions(std::span<const std::filesystem::path> directories,
                                 core::Logger& logger) {
    Contributions contributions;
    for (const std::filesystem::path& directory : directories) {
        const auto manifest = read_json(directory / "package.json");
        if (!manifest || !manifest->is_object() || !manifest->contains("name") ||
            !manifest->contains("publisher") || !(*manifest)["name"].is_string() ||
            !(*manifest)["publisher"].is_string()) {
            continue;
        }
        const std::string extension = (*manifest)["publisher"].get<std::string>() + "." +
                                      (*manifest)["name"].get<std::string>();
        const auto found = manifest->find("contributes");
        if (found == manifest->end()) {
            continue;
        }
        const SectionReader reader(logger, extension);
        if (!found->is_object()) {
            reader.problem("contributes is not an object");
            continue;
        }
        const Translations translations(directory);
        read_commands(*found, reader, translations, extension, contributions);
        read_keybindings(*found, reader, extension, contributions);
        read_palette_rules(*found, reader, extension, contributions);
    }
    return contributions;
}

core::KeybindingFile extension_keybindings(const Contributions& contributions,
                                           core::KeyPlatform platform) {
    core::KeybindingFile result;
    for (const ContributedKeybinding& binding : contributions.keybindings) {
        const std::string& specific = platform == core::KeyPlatform::macos     ? binding.mac
                                      : platform == core::KeyPlatform::windows ? binding.win
                                                                               : binding.linux;
        const std::string& key = specific.empty() ? binding.key : specific;
        if (key.empty()) {
            continue;
        }
        json entry = {{"key", key}, {"command", binding.command}};
        if (!binding.when.empty()) {
            entry["when"] = binding.when;
        }
        if (!binding.args.empty()) {
            entry["args"] = json::parse(binding.args, nullptr, false);
        }
        auto read = core::read_keybindings(json::array({std::move(entry)}),
                                           core::KeybindingSource::extension);
        for (core::Keybinding& usable : read->bindings) {
            result.bindings.push_back(std::move(usable));
        }
        for (const std::string& problem : read->problems) {
            const std::string prefix = "entry 0: ";
            result.problems.push_back(
                binding.extension + ": key binding of " + binding.command + ": " +
                (problem.starts_with(prefix) ? problem.substr(prefix.size()) : problem));
        }
    }
    return result;
}

} // namespace anvil::exthost
