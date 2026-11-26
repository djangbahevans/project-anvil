#include "anvil/ui/menu_style.hpp"

#include "anvil/ui/codicons.hpp"

#include <QPainter>
#include <QStyleOption>

namespace anvil::ui {
namespace {

void draw_glyph(QPainter* painter, const QRect& rect, std::string_view name, const QColor& color,
                int size) {
    const auto code = codicon(name);
    if (!code) {
        return;
    }
    QFont font(codicon_font_family());
    font.setPixelSize(size);
    painter->setFont(font);
    painter->setPen(color);
    painter->drawText(rect, Qt::AlignCenter, QString(QChar(*code)));
}

// The text of an item that is not its shortcut, which follows a tab.
QString label_of(const QString& text) {
    return text.left(text.indexOf(QLatin1Char('\t')));
}

QString shortcut_of(const QString& text) {
    const qsizetype tab = text.indexOf(QLatin1Char('\t'));
    return tab < 0 ? QString() : text.mid(tab + 1);
}

// The label as drawn: a single `&` marks the mnemonic and `&&` is an ampersand.
QString without_mnemonic_marker(QString label) {
    label.replace(QStringLiteral("&&"), QStringLiteral("\x01"));
    label.remove(QLatin1Char('&'));
    label.replace(QLatin1Char('\x01'), QLatin1Char('&'));
    return label;
}

} // namespace

MenuStyle::MenuStyle(const WorkbenchColors& colors)
    : QProxyStyle(QStringLiteral("fusion")), colors_(colors) {}

int MenuStyle::pixelMetric(PixelMetric metric, const QStyleOption* option,
                           const QWidget* widget) const {
    switch (metric) {
    case PM_MenuPanelWidth:
        return frame_width;
    case PM_MenuVMargin:
        return zoomed(vertical_margin);
    case PM_MenuHMargin:
        return 0;
    default:
        return QProxyStyle::pixelMetric(metric, option, widget);
    }
}

QSize MenuStyle::sizeFromContents(ContentsType type, const QStyleOption* option, const QSize& size,
                                  const QWidget* widget) const {
    const auto* item = qstyleoption_cast<const QStyleOptionMenuItem*>(option);
    if (type != CT_MenuItem || !item) {
        return QProxyStyle::sizeFromContents(type, option, size, widget);
    }
    if (item->menuItemType == QStyleOptionMenuItem::Separator) {
        return {size.width(), zoomed(separator_height)};
    }
    const QString shortcut = shortcut_of(item->text);
    int width = zoomed(label_left) +
                item->fontMetrics.horizontalAdvance(without_mnemonic_marker(label_of(item->text))) +
                zoomed(shortcut_right);
    if (!shortcut.isEmpty()) {
        width += zoomed(label_to_shortcut) + item->fontMetrics.horizontalAdvance(shortcut);
    }
    return {width, std::max(zoomed(item_height), item->fontMetrics.height() + zoomed(8))};
}

void MenuStyle::drawPrimitive(PrimitiveElement element, const QStyleOption* option,
                              QPainter* painter, const QWidget* widget) const {
    switch (element) {
    case PE_PanelMenu:
        painter->fillRect(option->rect, colors_.color("menu.background"));
        return;
    case PE_FrameMenu:
        painter->setPen(colors_.color("menu.border"));
        painter->setBrush(Qt::NoBrush);
        painter->drawRect(option->rect.adjusted(0, 0, -1, -1));
        return;
    default:
        QProxyStyle::drawPrimitive(element, option, painter, widget);
    }
}

void MenuStyle::drawControl(ControlElement element, const QStyleOption* option, QPainter* painter,
                            const QWidget* widget) const {
    if (element == CE_MenuEmptyArea) {
        painter->fillRect(option->rect, colors_.color("menu.background"));
        return;
    }
    const auto* item = qstyleoption_cast<const QStyleOptionMenuItem*>(option);
    if (element != CE_MenuItem || !item) {
        QProxyStyle::drawControl(element, option, painter, widget);
        return;
    }
    const QRect rect = item->rect;
    painter->fillRect(rect, colors_.color("menu.background"));
    if (item->menuItemType == QStyleOptionMenuItem::Separator) {
        painter->fillRect(QRect(rect.left(), rect.top() + rect.height() / 2, rect.width(), 1),
                          colors_.color("menu.separatorBackground"));
        return;
    }

    const bool enabled = item->state & State_Enabled;
    const bool chosen = enabled && (item->state & State_Selected);
    QColor color = colors_.color(chosen ? "menu.selectionForeground" : "menu.foreground");
    if (chosen) {
        painter->fillRect(rect, colors_.color("menu.selectionBackground"));
    }
    if (!enabled) {
        color.setAlphaF(0.4F);
    }

    if (item->checkType != QStyleOptionMenuItem::NotCheckable && item->checked) {
        draw_glyph(painter, QRect(rect.left(), rect.top(), zoomed(check_column), rect.height()),
                   "check", color, zoomed(glyph_size));
    }

    painter->setFont(item->font);
    painter->setPen(color);
    int flags = Qt::AlignVCenter | Qt::TextSingleLine | Qt::TextDontClip | Qt::TextShowMnemonic;
    if (!proxy()->styleHint(SH_UnderlineShortcut, item, widget)) {
        flags = (flags & ~Qt::TextShowMnemonic) | Qt::TextHideMnemonic;
    }
    const int label_start = zoomed(label_left);
    painter->drawText(
        QRect(rect.left() + label_start, rect.top(), rect.width() - label_start, rect.height()),
        flags | Qt::AlignLeft, label_of(item->text));
    if (const QString shortcut = shortcut_of(item->text); !shortcut.isEmpty()) {
        painter->drawText(
            QRect(rect.left(), rect.top(), rect.width() - zoomed(shortcut_right), rect.height()),
            Qt::AlignVCenter | Qt::AlignRight | Qt::TextSingleLine, shortcut);
    }
    if (item->menuItemType == QStyleOptionMenuItem::SubMenu) {
        const int glyph = zoomed(glyph_size);
        const int left = rect.right() - zoomed(chevron_center_from_right) - glyph / 2;
        draw_glyph(painter, QRect(left, rect.top(), glyph, rect.height()), "chevron-right", color,
                   glyph);
    }
}

} // namespace anvil::ui
