#pragma once

#include <QRgb>
#include <QString>
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
    std::string_view language;
    SetiIcon icon;
};

// The languages that the theme gives an icon, ordered by language id.
std::span<const SetiIconEntry> seti_icon_table();

// The icon of a file that the theme has nothing else for.
SetiIcon seti_default_icon();

// The icon of a language, which is the default one for a language that the theme does not name.
SetiIcon seti_icon(std::string_view language_id);

// The family of the Seti font, which is loaded on the first call; empty when it cannot be.
QString seti_font_family();

} // namespace anvil::ui
