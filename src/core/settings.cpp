#include "anvil/core/settings.hpp"

#include "anvil/core/jsonc.hpp"

#include <cstdint>
#include <type_traits>

namespace anvil::core {
namespace {

using nlohmann::json;

const json* find_in(const json& object, std::string_view key) {
    if (!object.is_object()) {
        return nullptr;
    }
    if (const auto found = object.find(std::string(key)); found != object.end()) {
        return &*found;
    }
    for (std::size_t dot = key.find('.'); dot != std::string_view::npos;
         dot = key.find('.', dot + 1)) {
        if (const auto child = object.find(std::string(key.substr(0, dot)));
            child != object.end() && child->is_object()) {
            if (const json* value = find_in(*child, key.substr(dot + 1))) {
                return value;
            }
        }
    }
    return nullptr;
}

} // namespace

std::expected<Settings, Error> Settings::parse(std::string_view jsonc) {
    if (is_blank_jsonc(jsonc)) {
        return Settings();
    }
    auto root = parse_jsonc(jsonc);
    if (!root) {
        return std::unexpected(std::move(root.error()));
    }
    return from_json(std::move(*root));
}

std::expected<Settings, Error> Settings::from_json(json root) {
    if (!root.is_object()) {
        return std::unexpected(Error{ErrorCode::invalid_argument,
                                     "the settings must be a JSON object at the top level"});
    }
    Settings settings;
    settings.root_ = std::move(root);
    return settings;
}

const json* Settings::find(std::string_view key) const {
    return find_in(root_, key);
}

template <class T> std::optional<T> Settings::get(std::string_view key) const {
    const json* value = find(key);
    if (value == nullptr) {
        return std::nullopt;
    }
    if constexpr (std::is_same_v<T, bool>) {
        return value->is_boolean() ? std::optional<T>(value->get<bool>()) : std::nullopt;
    } else if constexpr (std::is_same_v<T, std::int64_t>) {
        if (!value->is_number_integer() ||
            (value->is_number_unsigned() &&
             value->get<std::uint64_t>() > static_cast<std::uint64_t>(INT64_MAX))) {
            return std::nullopt;
        }
        return value->get<std::int64_t>();
    } else if constexpr (std::is_same_v<T, double>) {
        return value->is_number() ? std::optional<T>(value->get<double>()) : std::nullopt;
    } else {
        return value->is_string() ? std::optional<T>(value->get<std::string>()) : std::nullopt;
    }
}

template std::optional<bool> Settings::get<bool>(std::string_view) const;
template std::optional<std::int64_t> Settings::get<std::int64_t>(std::string_view) const;
template std::optional<double> Settings::get<double>(std::string_view) const;
template std::optional<std::string> Settings::get<std::string>(std::string_view) const;

} // namespace anvil::core
