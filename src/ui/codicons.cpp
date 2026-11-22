#include "anvil/ui/codicons.hpp"

#include "codicon_table.hpp"

#include <QFile>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QPainter>
#include <QScreen>
#include <algorithm>

namespace anvil::ui {

std::span<const CodiconEntry> codicon_table() {
    return ::anvil::ui::codicon_table_data;
}

std::optional<char16_t> codicon(std::string_view name) {
    const auto table = codicon_table();
    const auto found = std::ranges::lower_bound(table, name, {}, &CodiconEntry::name);
    if (found == table.end() || found->name != name) {
        return std::nullopt;
    }
    return found->code;
}

QString codicon_font_family() {
    static const QString family = [] {
        if (!QGuiApplication::instance()) {
            return QString();
        }
        QFile file(QStringLiteral(":/anvil/codicons/codicon.ttf"));
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

namespace {

QPixmap draw(char16_t code, const QColor& color, int size, qreal ratio, const QString& family) {
    QPixmap pixmap(QSize(size, size) * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::TextAntialiasing);
    QFont font(family);
    font.setPixelSize(size);
    painter.setFont(font);
    painter.setPen(color);
    painter.drawText(QRect(0, 0, size, size), Qt::AlignCenter, QString(QChar(code)));
    return pixmap;
}

} // namespace

QIcon codicon_icon(std::string_view name, const QColor& color, int size) {
    const auto code = codicon(name);
    if (!code || size <= 0 || !QGuiApplication::instance()) {
        return {};
    }
    const QString family = codicon_font_family();
    if (family.isEmpty()) {
        return {};
    }
    const QScreen* screen = QGuiApplication::primaryScreen();
    const qreal ratio = screen ? screen->devicePixelRatio() : 1.0;

    QColor faded = color;
    faded.setAlpha(qRound(color.alpha() * 0.4));
    const QPixmap normal = draw(*code, color, size, ratio, family);
    const QPixmap disabled = draw(*code, faded, size, ratio, family);
    QIcon icon;
    icon.addPixmap(normal, QIcon::Normal);
    icon.addPixmap(normal, QIcon::Active);
    icon.addPixmap(normal, QIcon::Selected);
    icon.addPixmap(disabled, QIcon::Disabled);
    return icon;
}

namespace {

bool is_name_character(QChar c) {
    const char16_t u = c.unicode();
    return (u >= u'a' && u <= u'z') || (u >= u'A' && u <= u'Z') || (u >= u'0' && u <= u'9') ||
           u == u'-';
}

bool is_letter(QChar c) {
    const char16_t u = c.unicode();
    return (u >= u'a' && u <= u'z') || (u >= u'A' && u <= u'Z');
}

struct Marker {
    qsizetype length = 0;
    std::string name;
    std::string modifier;
};

// The marker that starts at `start`, if there is one.
std::optional<Marker> marker_at(const QString& s, qsizetype start) {
    if (start + 1 >= s.size() || s[start] != u'$' || s[start + 1] != u'(') {
        return std::nullopt;
    }
    qsizetype i = start + 2;
    const qsizetype name_start = i;
    while (i < s.size() && is_name_character(s[i])) {
        ++i;
    }
    if (i == name_start) {
        return std::nullopt;
    }
    Marker marker;
    marker.name = s.mid(name_start, i - name_start).toStdString();
    if (i < s.size() && s[i] == u'~') {
        const qsizetype modifier_start = ++i;
        while (i < s.size() && is_letter(s[i])) {
            ++i;
        }
        if (i == modifier_start) {
            return std::nullopt;
        }
        marker.modifier = s.mid(modifier_start, i - modifier_start).toStdString();
    }
    if (i >= s.size() || s[i] != u')') {
        return std::nullopt;
    }
    marker.length = i + 1 - start;
    return marker;
}

} // namespace

std::vector<LabelSegment> split_label_with_icons(const QString& label) {
    std::vector<LabelSegment> segments;
    QString text;
    const auto flush = [&] {
        if (!text.isEmpty()) {
            segments.push_back({.text = std::move(text)});
            text.clear();
        }
    };

    qsizetype i = 0;
    while (i < label.size()) {
        if (label[i] == u'\\') {
            if (const auto escaped = marker_at(label, i + 1)) {
                text += label.mid(i + 1, escaped->length);
                i += 1 + escaped->length;
                continue;
            }
        } else if (const auto marker = marker_at(label, i)) {
            flush();
            segments.push_back({.icon = marker->name, .modifier = marker->modifier});
            i += marker->length;
            continue;
        }
        text += label[i];
        ++i;
    }
    flush();
    return segments;
}

} // namespace anvil::ui
