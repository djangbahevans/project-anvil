#include "anvil/ui/seti_icons.hpp"

#include "seti_icon_table.hpp"

#include <QFile>
#include <QFontDatabase>
#include <QGuiApplication>
#include <algorithm>
#include <string>

namespace anvil::ui {

namespace {

const SetiIcon* find(std::span<const SetiIconEntry> table, std::string_view key) {
    const auto found = std::ranges::lower_bound(table, key, {}, &SetiIconEntry::key);
    return found != table.end() && found->key == key ? &found->icon : nullptr;
}

// Takes from `rule`, which is less specific than those taken before, what they did not say.
void fill(SetiGlyph& glyph, const SetiGlyph& rule) {
    if (glyph.code == 0) {
        glyph.code = rule.code;
    }
    if (glyph.color == 0) {
        glyph.color = rule.color;
    }
}

void fill(SetiIcon& icon, const SetiIcon& rule) {
    fill(icon.dark, rule.dark);
    fill(icon.light, rule.light);
}

bool complete(const SetiIcon& icon) {
    return icon.dark.code != 0 && icon.dark.color != 0 && icon.light.code != 0 &&
           icon.light.color != 0;
}

// What the name and the extensions of a file say of its icon.
SetiIcon named_part(std::string_view file_name) {
    std::string name(file_name);
    std::ranges::transform(name, name.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    SetiIcon icon;
    if (const SetiIcon* rule = find(seti_file_name_table_data, name)) {
        fill(icon, *rule);
    }
    const std::string_view view = name;
    for (auto dot = view.find('.'); dot != std::string_view::npos && !complete(icon);
         dot = view.find('.', dot + 1)) {
        if (const SetiIcon* rule = find(seti_file_extension_table_data, view.substr(dot + 1))) {
            fill(icon, *rule);
        }
    }
    return icon;
}

} // namespace

std::span<const SetiIconEntry> seti_icon_table() {
    return ::anvil::ui::seti_icon_table_data;
}

std::span<const SetiIconEntry> seti_file_name_table() {
    return ::anvil::ui::seti_file_name_table_data;
}

std::span<const SetiIconEntry> seti_file_extension_table() {
    return ::anvil::ui::seti_file_extension_table_data;
}

SetiIcon seti_default_icon() {
    return seti_default_icon_data;
}

SetiIcon seti_icon(std::string_view language_id) {
    const SetiIcon* found = find(seti_icon_table_data, language_id);
    return found ? *found : seti_default_icon_data;
}

SetiIcon seti_file_icon(std::string_view file_name, std::string_view language_id) {
    SetiIcon icon = named_part(file_name);
    fill(icon, seti_icon(language_id));
    // The icon of a language can lack a color as well.
    fill(icon, seti_default_icon_data);
    return icon;
}

std::optional<SetiIcon> seti_icon_by_name(std::string_view file_name) {
    const SetiIcon icon = named_part(file_name);
    return complete(icon) ? std::optional(icon) : std::nullopt;
}

QString seti_font_family() {
    static const QString family = [] {
        if (!QGuiApplication::instance()) {
            return QString();
        }
        QFile file(QStringLiteral(":/anvil/seti/seti.woff"));
        if (!file.open(QIODevice::ReadOnly)) {
            return QString();
        }
        const int id = QFontDatabase::addApplicationFontFromData(file.readAll());
        const QStringList families =
            id < 0 ? QStringList() : QFontDatabase::applicationFontFamilies(id);
        return families.isEmpty() ? QString() : families.front();
    }();
    return family;
}

} // namespace anvil::ui
