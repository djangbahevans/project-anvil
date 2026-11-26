#include "anvil/ui/seti_icons.hpp"

#include "seti_icon_table.hpp"

#include <QFile>
#include <QFontDatabase>
#include <QGuiApplication>
#include <algorithm>

namespace anvil::ui {

std::span<const SetiIconEntry> seti_icon_table() {
    return ::anvil::ui::seti_icon_table_data;
}

SetiIcon seti_default_icon() {
    return seti_default_icon_data;
}

SetiIcon seti_icon(std::string_view language_id) {
    const auto table = seti_icon_table();
    const auto found = std::ranges::lower_bound(table, language_id, {}, &SetiIconEntry::language);
    return found != table.end() && found->language == language_id ? found->icon
                                                                  : seti_default_icon_data;
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
