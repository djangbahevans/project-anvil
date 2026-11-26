#pragma once

#include <QRgb>
#include <QString>
#include <optional>
#include <span>
#include <string_view>

namespace anvil::ui {

// A character of the Seti font and its color.
struct SetiGlyph {
    char16_t code = 0;
    QRgb color = 0;

    friend bool operator==(const SetiGlyph&, const SetiGlyph&) = default;
};

// An icon of VS Code's Seti file icon theme. `light` is the one of a light color theme; every other
// kind of theme, a high contrast light one included, shows `dark`, as VS Code does.
struct SetiIcon {
    SetiGlyph dark;
    SetiGlyph light;

    friend bool operator==(const SetiIcon&, const SetiIcon&) = default;
};

struct SetiIconEntry {
    // A language id, a file name or a file extension.
    std::string_view key;
    // A color of 0 is a glyph without a color of its own.
    SetiIcon icon;
};

// The languages that the theme gives an icon, ordered by language id.
std::span<const SetiIconEntry> seti_icon_table();

// The file names and file extensions that the theme gives an icon, in lower case and ordered.
std::span<const SetiIconEntry> seti_file_name_table();
std::span<const SetiIconEntry> seti_file_extension_table();

// The icon of a file that the theme has nothing else for.
SetiIcon seti_default_icon();

// The icon of a language, which is the default one for a language that the theme does not name.
SetiIcon seti_icon(std::string_view language_id);

// The icon of a file, from its name and language id, as VS Code chooses it: by the whole name, then
// by an extension, the longest first, then by the language, else the default one. Names are
// compared in lower case. A glyph without a color takes the color of the next of these that has
// one.
SetiIcon seti_file_icon(std::string_view file_name, std::string_view language_id);

// The icon of a file whose name alone decides it, so that its language need not be asked for.
std::optional<SetiIcon> seti_icon_by_name(std::string_view file_name);

// The size of a Seti glyph next to a text, as a share of the size of the text, which the theme
// asks for.
inline constexpr int seti_font_percent = 150;

// The family of the Seti font, which is loaded on the first call; empty when it cannot be.
QString seti_font_family();

} // namespace anvil::ui
