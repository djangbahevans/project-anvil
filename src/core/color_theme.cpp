#include "anvil/core/color_theme.hpp"

#include "anvil/core/jsonc.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace anvil::core {

ThemeKind theme_kind_from_ui_theme(std::string_view ui_theme) {
    if (ui_theme == "vs") {
        return ThemeKind::light;
    }
    if (ui_theme == "hc-black") {
        return ThemeKind::high_contrast_dark;
    }
    if (ui_theme == "hc-light") {
        return ThemeKind::high_contrast_light;
    }
    return ThemeKind::dark;
}

namespace {

std::optional<unsigned> hex_value(std::string_view digits) {
    unsigned value = 0;
    const auto [end, error] =
        std::from_chars(digits.data(), digits.data() + digits.size(), value, 16);
    if (error != std::errc{} || end != digits.data() + digits.size()) {
        return std::nullopt;
    }
    return value;
}

} // namespace

std::optional<Rgba> parse_color(std::string_view text) {
    if (text.size() < 2 || text.front() != '#') {
        return std::nullopt;
    }
    const std::string_view digits = text.substr(1);
    const bool short_form = digits.size() == 3 || digits.size() == 4;
    if (!short_form && digits.size() != 6 && digits.size() != 8) {
        return std::nullopt;
    }
    // from_chars accepts a sign, so check that every character is a digit.
    if (!std::ranges::all_of(digits,
                             [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); })) {
        return std::nullopt;
    }
    const std::size_t width = short_form ? 1 : 2;
    std::uint8_t channels[4] = {0, 0, 0, 255};
    for (std::size_t i = 0; i * width < digits.size(); ++i) {
        const auto value = hex_value(digits.substr(i * width, width));
        if (!value) {
            return std::nullopt;
        }
        channels[i] = static_cast<std::uint8_t>(short_form ? *value * 17 : *value);
    }
    return Rgba{channels[0], channels[1], channels[2], channels[3]};
}

// Reads one theme file and the files it includes into a ColorTheme.
class ThemeReader {
public:
    ThemeReader(ColorTheme& theme, const ColorTheme::Loader& loader)
        : theme_(theme), loader_(loader) {}

    std::expected<void, Error> read(const std::filesystem::path& path, bool is_root) {
        if (std::ranges::find(chain_, path) != chain_.end()) {
            return fail(ErrorCode::invalid_argument, "include cycle: " + chain_text(path));
        }
        if (static_cast<int>(chain_.size()) > ColorTheme::max_include_depth) {
            return fail(ErrorCode::limit_exceeded,
                        "includes nest deeper than " +
                            std::to_string(ColorTheme::max_include_depth) +
                            " files: " + chain_text(path));
        }
        if (path.extension() != ".json") {
            return fail(ErrorCode::unsupported,
                        "only JSON theme files are supported: " + chain_text(path));
        }

        const auto text = loader_(path);
        if (!text) {
            return fail(text.error().code,
                        "cannot read " + chain_text(path) + ": " + text.error().message);
        }
        const auto parsed = parse_jsonc(*text);
        if (!parsed) {
            return fail(parsed.error().code, path.generic_string() + ": " + parsed.error().message);
        }
        const nlohmann::json& file = *parsed;
        if (!file.is_object()) {
            return fail(ErrorCode::invalid_argument,
                        path.generic_string() + ": a theme file is a JSON object");
        }

        chain_.push_back(path);
        const auto result = apply(path, file, is_root);
        chain_.pop_back();
        return result;
    }

private:
    std::expected<void, Error> apply(const std::filesystem::path& path, const nlohmann::json& file,
                                     bool is_root) {
        if (const auto include = file.find("include");
            include != file.end() && !include->is_null()) {
            if (!include->is_string()) {
                return fail(ErrorCode::invalid_argument,
                            path.generic_string() + ": `include` must be a path");
            }
            const std::filesystem::path included =
                (path.parent_path() / include->get<std::string>()).lexically_normal();
            if (auto loaded = read(included, false); !loaded) {
                return loaded;
            }
        }
        if (const auto settings = file.find("settings");
            settings != file.end() && settings->is_array()) {
            return fail(ErrorCode::unsupported,
                        path.generic_string() +
                            ": the legacy `settings` array of TextMate themes is not supported");
        }

        if (const auto colors = file.find("colors"); colors != file.end() && !colors->is_null()) {
            if (!colors->is_object()) {
                return fail(ErrorCode::invalid_argument,
                            path.generic_string() + ": `colors` must be an object");
            }
            for (const auto& [key, value] : colors->items()) {
                apply_color(path, key, value);
            }
        }

        if (const auto rules = file.find("tokenColors"); rules != file.end()) {
            if (rules->is_string()) {
                return fail(ErrorCode::unsupported,
                            path.generic_string() +
                                ": `tokenColors` naming a TextMate theme file is not supported");
            }
            if (rules->is_array()) {
                for (const nlohmann::json& rule : *rules) {
                    theme_.token_colors_.push_back(rule);
                }
            } else if (!rules->is_null() && *rules != false) {
                return fail(ErrorCode::invalid_argument,
                            path.generic_string() + ": `tokenColors` must be an array of rules");
            }
        }

        if (is_root) {
            if (const auto name = file.find("name"); name != file.end() && name->is_string()) {
                theme_.name_ = name->get<std::string>();
            }
        }
        return {};
    }

    void apply_color(const std::filesystem::path& path, const std::string& key,
                     const nlohmann::json& value) {
        if (!value.is_string()) {
            return;
        }
        const auto& text = value.get_ref<const std::string&>();
        if (text == "default") {
            theme_.colors_.erase(key);
        } else if (const auto color = parse_color(text)) {
            theme_.colors_[key] = *color;
        } else {
            theme_.warnings_.push_back(path.generic_string() + ": color '" + key + "' is '" + text +
                                       "', which is not a color");
        }
    }

    std::string chain_text(const std::filesystem::path& next) const {
        std::string text;
        for (const auto& step : chain_) {
            text += step.generic_string() + " -> ";
        }
        return text + next.generic_string();
    }

    std::unexpected<Error> fail(ErrorCode code, std::string message) const {
        return std::unexpected(Error{code, std::move(message)});
    }

    ColorTheme& theme_;
    const ColorTheme::Loader& loader_;
    std::vector<std::filesystem::path> chain_;
};

std::expected<ColorTheme, Error> ColorTheme::load(const std::filesystem::path& path, ThemeKind kind,
                                                  const Loader& loader) {
    ColorTheme theme;
    theme.kind_ = kind;
    ThemeReader reader(theme, loader);
    if (auto read = reader.read(path.lexically_normal(), true); !read) {
        return std::unexpected(std::move(read.error()));
    }
    return theme;
}

std::optional<Rgba> ColorTheme::color(std::string_view key) const {
    const auto found = colors_.find(std::string(key));
    return found == colors_.end() ? std::nullopt : std::optional<Rgba>(found->second);
}

} // namespace anvil::core
