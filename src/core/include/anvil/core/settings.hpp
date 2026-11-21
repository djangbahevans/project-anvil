#pragma once

#include "anvil/core/error.hpp"
#include "anvil/core/text_file.hpp"

#include <cstdint>
#include <expected>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace anvil::core {

// The values of a settings file, keyed like VS Code's: "editor.fontSize". The data is the file's
// JSON object as written, so keys that nothing reads are kept, and a setting is found whether the
// file spells it flat (`"editor.fontSize": 14`) or nested (`"editor": {"fontSize": 14}`).
class Settings {
public:
    Settings() = default;

    // Parses the text of a settings file, which is JSON with comments. Text with no content is an
    // empty set of settings; anything else must be an object.
    static std::expected<Settings, Error> parse(std::string_view jsonc);
    // Wraps an object; fails when `root` is not one.
    static std::expected<Settings, Error> from_json(nlohmann::json root);

    const nlohmann::json& root() const {
        return root_;
    }

    // The value of a dotted key, or null when there is none. A key spelled flat at some level wins
    // over the same key spelled nested.
    const nlohmann::json* find(std::string_view key) const;
    bool contains(std::string_view key) const {
        return find(key) != nullptr;
    }

    // The value of a key as `T` (bool, std::int64_t, double or std::string), or nothing when the
    // key is absent or its value has another type. An integer reads as a double.
    template <class T> std::optional<T> get(std::string_view key) const;

    template <class T> T get_or(std::string_view key, T fallback) const {
        return get<T>(key).value_or(std::move(fallback));
    }

private:
    nlohmann::json root_ = nlohmann::json::object();
};

extern template std::optional<bool> Settings::get<bool>(std::string_view) const;
extern template std::optional<std::int64_t> Settings::get<std::int64_t>(std::string_view) const;
extern template std::optional<double> Settings::get<double>(std::string_view) const;
extern template std::optional<std::string> Settings::get<std::string>(std::string_view) const;

} // namespace anvil::core
