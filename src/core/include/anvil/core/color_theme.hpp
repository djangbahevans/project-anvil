#pragma once

#include "anvil/core/error.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace anvil::core {

// Whether a theme is dark or light, and whether it is a high contrast one. A theme file seldom says
// so itself: the extension that contributes it does, in its manifest's `uiTheme`.
enum class ThemeKind {
    dark,
    light,
    high_contrast_dark,
    high_contrast_light,
};

// `vs-dark`, `vs`, `hc-black` and `hc-light`; anything else, including no value, is dark, as in
// VS Code.
ThemeKind theme_kind_from_ui_theme(std::string_view ui_theme);

struct Rgba {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
    std::uint8_t alpha = 255;

    friend bool operator==(const Rgba&, const Rgba&) = default;
};

// `#rgb`, `#rgba`, `#rrggbb` or `#rrggbbaa`, in either case. Nothing for anything else.
std::optional<Rgba> parse_color(std::string_view text);

// A VS Code color theme: the workbench colors and the token color rules of a theme file and of the
// files it includes (`dark_modern.json` includes `dark_plus.json`, which includes `dark_vs.json`).
// The rules follow `colorThemeData.ts` of VS Code:
// - An included file is applied first and the including file on top of it, so the including file's
//   colors win and its token rules come later, which makes them win at equal specificity.
// - `colors` maps a workbench color key to a color; the value `"default"` removes a key that an
//   included file set. A value that is not a string is ignored. A string that is not a color is
//   skipped, with a warning, so the key keeps what an included file gave it, or none.
// - `tokenColors` is an array of rules, kept as the JSON that was written.
// - The theme's name is the `name` of the file that was asked for.
//
// Not supported, and an error: a `tokenColors` that names a TextMate theme file, the legacy
// top-level `settings` array, and an include of anything but a `.json` file. `semanticTokenColors`
// and `semanticHighlighting` are ignored.
class ColorTheme {
public:
    // The text of the file at a path, or why it cannot be read. The core does no file I/O.
    using Loader = std::function<std::expected<std::string, Error>(const std::filesystem::path&)>;

    static constexpr int max_include_depth = 16;

    // Loads the theme file at `path` and what it includes. An include is found relative to the
    // directory of the file that names it. An error says which file, and for an include which chain
    // of files, and for a syntax error where in the file.
    static std::expected<ColorTheme, Error> load(const std::filesystem::path& path, ThemeKind kind,
                                                 const Loader& loader);

    const std::string& name() const {
        return name_;
    }
    ThemeKind kind() const {
        return kind_;
    }

    // The color of a workbench color key, which is case-sensitive.
    std::optional<Rgba> color(std::string_view key) const;
    const std::unordered_map<std::string, Rgba>& colors() const {
        return colors_;
    }

    // The token color rules, as an array of objects in priority order: the rules of included files
    // first, and within a file in the order written.
    const nlohmann::json& token_colors() const {
        return token_colors_;
    }

    // Problems that did not stop the theme from loading, one per entry, each naming the key and the
    // file.
    const std::vector<std::string>& warnings() const {
        return warnings_;
    }

private:
    friend class ThemeReader;

    std::string name_;
    ThemeKind kind_ = ThemeKind::dark;
    std::unordered_map<std::string, Rgba> colors_;
    nlohmann::json token_colors_ = nlohmann::json::array();
    std::vector<std::string> warnings_;
};

} // namespace anvil::core
