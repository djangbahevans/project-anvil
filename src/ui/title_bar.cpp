#include "anvil/ui/title_bar.hpp"

#include "anvil/ui/codicons.hpp"

#include <QAbstractButton>
#include <QEnterEvent>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QSizePolicy>
#include <algorithm>

namespace anvil::ui {
namespace {

constexpr int icon_size = 16;
constexpr int icon_margin = 10;
// VS Code gives the close button this color whatever the theme.
const QColor close_hover(0xE8, 0x11, 0x23);
constexpr int layout_button_size = 28;
constexpr int command_center_height = 24;
constexpr int command_center_margin = 8;
constexpr int command_center_min_width = 120;
constexpr int command_center_max_width = 450;
constexpr int radius = 5;

void draw_glyph(QPainter& painter, const QRect& rect, std::string_view name, const QColor& color,
                int size = icon_size) {
    const auto code = codicon(name);
    if (!code) {
        return;
    }
    QFont font(codicon_font_family());
    font.setPixelSize(size);
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

// A button that shows or hides a part of the window. It is checked while the part is shown, and a
// click does not change that: the window decides, and says so with `set_shown`.
class TitleBar::LayoutButton : public QAbstractButton {
public:
    LayoutButton(const WorkbenchColors& colors, std::string_view shown_icon,
                 std::string_view hidden_icon, const QString& name, QWidget* parent)
        : QAbstractButton(parent), colors_(colors), shown_icon_(shown_icon),
          hidden_icon_(hidden_icon) {
        setCheckable(true);
        setFixedSize(layout_button_size, layout_button_size);
        setFocusPolicy(Qt::NoFocus);
        setAccessibleName(name);
        setToolTip(name);
    }

    void set_shown(bool shown) {
        setChecked(shown);
        update();
    }
    void set_icons(std::string_view shown_icon, std::string_view hidden_icon) {
        shown_icon_ = shown_icon;
        hidden_icon_ = hidden_icon;
        update();
    }
    void set_active(bool active) {
        active_ = active;
        update();
    }

protected:
    void nextCheckState() override {}
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
        painter.setRenderHint(QPainter::Antialiasing);
        if (hovered_) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(colors_.color("toolbar.hoverBackground"));
            painter.drawRoundedRect(rect(), radius, radius);
        }
        draw_glyph(
            painter, rect(), isChecked() ? shown_icon_ : hidden_icon_,
            colors_.color(active_ ? "titleBar.activeForeground" : "titleBar.inactiveForeground"));
    }

private:
    const WorkbenchColors& colors_;
    std::string shown_icon_;
    std::string hidden_icon_;
    bool active_ = true;
    bool hovered_ = false;
};

// The box in the middle of the bar: a search icon and the name of the workspace in a rounded
// border that turns to its active colors under the pointer.
class TitleBar::CommandCenter : public QAbstractButton {
public:
    CommandCenter(const WorkbenchColors& colors, QWidget* parent)
        : QAbstractButton(parent), colors_(colors) {
        setFixedHeight(command_center_height);
        setFocusPolicy(Qt::NoFocus);
        setAccessibleName(tr("Command center"));
    }

    void set_active(bool active) {
        active_ = active;
        update();
    }
    void set_label(const QString& label) {
        setText(label);
        setAccessibleDescription(label);
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
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        if (hovered_) {
            painter.setBrush(colors_.color("commandCenter.activeBackground"));
        } else if (colors_.is_themed("commandCenter.background")) {
            painter.setBrush(colors_.color("commandCenter.background"));
        } else {
            painter.setBrush(Qt::NoBrush);
        }
        painter.setPen(colors_.color(hovered_  ? "commandCenter.activeBorder"
                                     : active_ ? "commandCenter.border"
                                               : "commandCenter.inactiveBorder"));
        painter.drawRoundedRect(box, radius, radius);

        const QColor foreground = colors_.color(hovered_  ? "commandCenter.activeForeground"
                                                : active_ ? "commandCenter.foreground"
                                                          : "commandCenter.inactiveForeground");
        QFont font = painter.font();
        font.setPixelSize(13);
        painter.setFont(font);
        const QFontMetrics metrics(font);
        constexpr int glyph = 14;
        constexpr int padding = 8;
        const QString text = metrics.elidedText(this->text(), Qt::ElideRight,
                                                width() - 2 * padding - glyph - padding);
        const int content = glyph + padding + metrics.horizontalAdvance(text);
        const int x = std::max(padding, (width() - content) / 2);
        draw_glyph(painter, QRect(x, 0, glyph, height()), "search", foreground, glyph);
        painter.setPen(foreground);
        painter.drawText(QRect(x + glyph + padding, 0, width(), height()),
                         Qt::AlignVCenter | Qt::AlignLeft, text);
    }

private:
    const WorkbenchColors& colors_;
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

    sidebar_button_ = new LayoutButton(colors_, "layout-sidebar-left", "layout-sidebar-left-off",
                                       tr("Toggle Primary Side Bar"), this);
    panel_button_ =
        new LayoutButton(colors_, "layout-panel", "layout-panel-off", tr("Toggle Panel"), this);
    layout_spacer_ = new QWidget(this);
    layout_spacer_->setFixedWidth(6);
    for (QWidget* widget : {static_cast<QWidget*>(sidebar_button_),
                            static_cast<QWidget*>(panel_button_), layout_spacer_}) {
        layout_->addWidget(widget);
        widget->hide();
    }
    command_center_ = new CommandCenter(colors_, this);
    command_center_->hide();
    connect(command_center_, &QAbstractButton::clicked, this, &TitleBar::command_center_requested);
    connect(sidebar_button_, &QAbstractButton::clicked, this, &TitleBar::toggle_sidebar_requested);
    connect(panel_button_, &QAbstractButton::clicked, this, &TitleBar::toggle_panel_requested);

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
        for (QWidget* child : findChildren<QWidget*>(Qt::FindDirectChildrenOnly)) {
            child->update();
        }
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
    static_cast<LayoutButton*>(sidebar_button_)->set_active(active);
    static_cast<LayoutButton*>(panel_button_)->set_active(active);
    static_cast<CommandCenter*>(command_center_)->set_active(active);
    style_menu_bar();
    update();
}

void TitleBar::set_maximized(bool maximized) {
    static_cast<ControlButton*>(maximize_)->set_icon(maximized ? "chrome-restore"
                                                               : "chrome-maximize");
    maximize_->setAccessibleName(maximized ? tr("Restore") : tr("Maximize"));
    maximize_->setToolTip(maximize_->accessibleName());
}

void TitleBar::set_command_center_visible(bool visible) {
    command_center_wanted_ = visible;
    place_command_center();
    update();
}

void TitleBar::set_command_center_text(const QString& text) {
    static_cast<CommandCenter*>(command_center_)->set_label(text);
}

void TitleBar::set_layout_controls_visible(bool visible) {
    layout_controls_wanted_ = visible;
    for (QWidget* widget : {static_cast<QWidget*>(sidebar_button_),
                            static_cast<QWidget*>(panel_button_), layout_spacer_}) {
        widget->setVisible(visible);
    }
    layout_->activate();
    place_command_center();
    update();
}

void TitleBar::set_sidebar_shown(bool shown) {
    static_cast<LayoutButton*>(sidebar_button_)->set_shown(shown);
}

void TitleBar::set_sidebar_side(Side side) {
    static_cast<LayoutButton*>(sidebar_button_)
        ->set_icons(side == Side::left ? "layout-sidebar-left" : "layout-sidebar-right",
                    side == Side::left ? "layout-sidebar-left-off" : "layout-sidebar-right-off");
}

void TitleBar::set_panel_shown(bool shown) {
    static_cast<LayoutButton*>(panel_button_)->set_shown(shown);
}

int TitleBar::buttons_left() const {
    return layout_controls_wanted_ ? sidebar_button_->geometry().left()
                                   : minimize_->geometry().left();
}

// The box is centered in the bar, and sits between the menu bar and the buttons when the window is
// too narrow for that. Too narrow for even a small box, it is left out.
void TitleBar::place_command_center() {
    const int left = (menu_bar_ ? menu_bar_->geometry().right() : icon_->geometry().right()) + 1 +
                     command_center_margin;
    const int right = buttons_left() - command_center_margin;
    const int box_width = std::min({command_center_max_width, width() * 2 / 5, right - left});
    const bool fits = box_width >= command_center_min_width;
    command_center_->setVisible(command_center_wanted_ && fits);
    if (fits) {
        const int x = std::clamp((width() - box_width) / 2, left, right - box_width);
        command_center_->setGeometry(x, (bar_height - command_center_height) / 2, box_width,
                                     command_center_height);
    }
}

bool TitleBar::event(QEvent* event) {
    const bool handled = QWidget::event(event);
    // The layout has placed the widgets by the time the request reaches the bar.
    if (event->type() == QEvent::LayoutRequest) {
        place_command_center();
    }
    return handled;
}

void TitleBar::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    place_command_center();
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
    const int right = buttons_left();
    if (!command_center_wanted_ && right > left) {
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
