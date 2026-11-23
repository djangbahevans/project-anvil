#include "anvil/ui/title_bar.hpp"

#include "anvil/ui/codicons.hpp"

#include <QAbstractButton>
#include <QEnterEvent>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QSizePolicy>
#include <algorithm>

namespace anvil::ui {
namespace {

constexpr int icon_size = 16;
constexpr int icon_margin = 10;
// VS Code gives the close button this color whatever the theme.
const QColor close_hover(0xE8, 0x11, 0x23);

void draw_glyph(QPainter& painter, const QRect& rect, std::string_view name, const QColor& color) {
    const auto code = codicon(name);
    if (!code) {
        return;
    }
    QFont font(codicon_font_family());
    font.setPixelSize(icon_size);
    painter.setFont(font);
    painter.setPen(color);
    painter.drawText(rect, Qt::AlignCenter, QString(QChar(*code)));
}

} // namespace

// A button of the title bar. It is not focusable: the title bar is for the pointer, and a tab stop
// before the editor would be in the way.
class TitleBar::ControlButton : public QAbstractButton {
public:
    ControlButton(const WorkbenchColors& colors, std::string_view icon, bool closing,
                  const QString& name, QWidget* parent)
        : QAbstractButton(parent), colors_(colors), icon_(icon), closing_(closing) {
        setFixedSize(button_width, bar_height);
        setFocusPolicy(Qt::NoFocus);
        setAccessibleName(name);
        setToolTip(name);
    }

    void set_icon(std::string_view icon) {
        icon_ = icon;
        update();
    }
    void set_active(bool active) {
        active_ = active;
        update();
    }

protected:
    void enterEvent(QEnterEvent*) override {
        hovered_ = true;
        update();
    }
    void leaveEvent(QEvent*) override {
        hovered_ = false;
        update();
    }
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        const bool hovered = hovered_;
        if (hovered) {
            painter.fillRect(rect(),
                             closing_ ? close_hover : colors_.color("toolbar.hoverBackground"));
        }
        const QColor color = hovered && closing_
                                 ? QColor(Qt::white)
                                 : colors_.color(active_ ? "titleBar.activeForeground"
                                                         : "titleBar.inactiveForeground");
        draw_glyph(painter, rect(), icon_, color);
    }

private:
    const WorkbenchColors& colors_;
    std::string icon_;
    bool closing_;
    bool active_ = true;
    bool hovered_ = false;
};

TitleBar::TitleBar(const WorkbenchColors& colors, QWidget* parent)
    : QWidget(parent), colors_(colors) {
    setFixedHeight(bar_height);
    setAccessibleName(tr("Title bar"));

    layout_ = new QHBoxLayout(this);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(0);

    icon_ = new QWidget(this);
    icon_->setFixedSize(icon_size + 2 * icon_margin, bar_height);
    icon_->setAttribute(Qt::WA_TransparentForMouseEvents);
    layout_->addWidget(icon_);
    layout_->addStretch(1);

    minimize_ = new ControlButton(colors_, "chrome-minimize", false, tr("Minimize"), this);
    maximize_ = new ControlButton(colors_, "chrome-maximize", false, tr("Maximize"), this);
    close_ = new ControlButton(colors_, "chrome-close", true, tr("Close"), this);
    for (QAbstractButton* button : {minimize_, maximize_, close_}) {
        layout_->addWidget(button);
    }
    connect(minimize_, &QAbstractButton::clicked, this, &TitleBar::minimize_requested);
    connect(maximize_, &QAbstractButton::clicked, this, &TitleBar::toggle_maximized_requested);
    connect(close_, &QAbstractButton::clicked, this, &TitleBar::close_requested);
    connect(&colors_, &WorkbenchColors::changed, this, [this] {
        style_menu_bar();
        update();
    });
}

// The bar takes the title bar's background and text, and an item under the pointer or open the
// color of a hovered button.
void TitleBar::style_menu_bar() {
    if (!menu_bar_) {
        return;
    }
    const QString foreground =
        colors_.color(active_ ? "titleBar.activeForeground" : "titleBar.inactiveForeground").name();
    menu_bar_->setStyleSheet(
        QStringLiteral("QMenuBar { background: transparent; color: %1; }"
                       "QMenuBar::item { background: transparent; padding: %3px 8px; }"
                       "QMenuBar::item:selected { background: %2; }")
            .arg(foreground, colors_.color("toolbar.hoverBackground").name(QColor::HexArgb))
            .arg(std::max(0, (bar_height - menu_bar_->fontMetrics().height()) / 2)));
}

void TitleBar::set_title(const QString& title) {
    if (title_ == title) {
        return;
    }
    title_ = title;
    update();
}

void TitleBar::set_menu_bar(QWidget* menu_bar) {
    menu_bar_ = menu_bar;
    menu_bar->setParent(this);
    menu_bar->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    menu_bar->setFixedHeight(bar_height);
    layout_->insertWidget(1, menu_bar);
    style_menu_bar();
    menu_bar->show();
}

void TitleBar::set_active(bool active) {
    if (active_ == active) {
        return;
    }
    active_ = active;
    for (QAbstractButton* button : {minimize_, maximize_, close_}) {
        static_cast<ControlButton*>(button)->set_active(active);
    }
    style_menu_bar();
    update();
}

void TitleBar::set_maximized(bool maximized) {
    static_cast<ControlButton*>(maximize_)->set_icon(maximized ? "chrome-restore"
                                                               : "chrome-maximize");
    maximize_->setAccessibleName(maximized ? tr("Restore") : tr("Maximize"));
    maximize_->setToolTip(maximize_->accessibleName());
}

void TitleBar::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), colors_.color(active_ ? "titleBar.activeBackground"
                                                   : "titleBar.inactiveBackground"));
    const QColor foreground =
        colors_.color(active_ ? "titleBar.activeForeground" : "titleBar.inactiveForeground");
    if (colors_.is_themed("titleBar.border")) {
        painter.fillRect(QRect(0, height() - 1, width(), 1), colors_.color("titleBar.border"));
    }

    draw_glyph(painter, QRect(icon_->pos(), icon_->size()), "code", foreground);

    // Between the menu bar, or the icon, and the buttons.
    const int left = (menu_bar_ ? menu_bar_->geometry().right() : icon_->geometry().right()) + 1;
    const int right = minimize_->geometry().left();
    if (right > left) {
        QFont font = painter.font();
        font.setPixelSize(13);
        painter.setFont(font);
        painter.setPen(foreground);
        const QFontMetrics metrics(font);
        const QString text =
            metrics.elidedText(title_, Qt::ElideRight, right - left - 2 * icon_margin);
        const int text_width = metrics.horizontalAdvance(text);
        int x = (width() - text_width) / 2;
        x = std::clamp(x, left + icon_margin,
                       std::max(left + icon_margin, right - icon_margin - text_width));
        painter.drawText(QRect(x, 0, text_width, height()), Qt::AlignVCenter | Qt::AlignLeft, text);
    }
}

void TitleBar::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        emit move_requested();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void TitleBar::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        emit toggle_maximized_requested();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

} // namespace anvil::ui
