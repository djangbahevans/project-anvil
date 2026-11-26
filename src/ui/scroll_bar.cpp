#include "anvil/ui/scroll_bar.hpp"

#include "anvil/core/zoom.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <algorithm>
#include <cmath>

namespace anvil::ui {

ScrollBar::ScrollBar(Qt::Orientation orientation, int extent, std::string_view surface,
                     QWidget* parent)
    : QScrollBar(orientation, parent), surface_(surface), extent_(extent) {
    setCursor(Qt::ArrowCursor);
    setMouseTracking(true);
    setContextMenuPolicy(Qt::PreventContextMenu);
    set_zoom_level(0);
}

void ScrollBar::set_colors(const WorkbenchColors* colors) {
    disconnect(colors_connection_);
    colors_ = colors;
    if (colors_) {
        colors_connection_ =
            connect(colors_, &WorkbenchColors::changed, this, qOverload<>(&QWidget::update));
    }
    update();
}

void ScrollBar::set_zoom_level(double level) {
    zoom_level_ = level;
    const int scaled = core::scale_for_zoom(extent_, level);
    if (orientation() == Qt::Vertical) {
        setFixedWidth(scaled);
    } else {
        setFixedHeight(scaled);
    }
    updateGeometry();
    update();
}

QSize ScrollBar::sizeHint() const {
    QSize hint = QScrollBar::sizeHint();
    const int thickness = core::scale_for_zoom(extent_, zoom_level_);
    if (orientation() == Qt::Vertical) {
        hint.setWidth(thickness);
    } else {
        hint.setHeight(thickness);
    }
    return hint;
}

int ScrollBar::along(const QPoint& point) const {
    return orientation() == Qt::Vertical ? point.y() : point.x();
}

int ScrollBar::length() const {
    return orientation() == Qt::Vertical ? height() : width();
}

QRect ScrollBar::slider_rect() const {
    const double range = static_cast<double>(maximum()) - minimum();
    if (range <= 0) {
        return {};
    }
    // The part of the content that shows, but never too short to take hold of.
    const double shown =
        std::floor(static_cast<double>(length()) * pageStep() / (range + pageStep()));
    const int size =
        std::min(length(), std::max(core::scale_for_zoom(minimum_slider_length, zoom_level_),
                                    static_cast<int>(shown)));
    const int start = static_cast<int>(std::lround(
        (static_cast<double>(sliderPosition()) - minimum()) * (length() - size) / range));
    return orientation() == Qt::Vertical ? QRect(0, start, width(), size)
                                         : QRect(start, 0, size, height());
}

void ScrollBar::put_slider_at(double position) {
    const QRect slider = slider_rect();
    const int room = length() - (orientation() == Qt::Vertical ? slider.height() : slider.width());
    if (room <= 0) {
        return;
    }
    const double range = static_cast<double>(maximum()) - minimum();
    const double wanted = minimum() + std::round(position * range / room);
    setSliderPosition(static_cast<int>(
        std::clamp(wanted, static_cast<double>(minimum()), static_cast<double>(maximum()))));
}

void ScrollBar::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    const auto color = [this](std::string_view key, QPalette::ColorRole role) {
        return colors_ ? colors_->color(key) : palette().color(role);
    };
    painter.fillRect(rect(), color(surface_, QPalette::Base));
    if (colors_ && colors_->is_themed("scrollbar.background")) {
        painter.fillRect(rect(), colors_->color("scrollbar.background"));
    }
    const QRect slider = slider_rect();
    if (slider.isEmpty()) {
        return;
    }
    const std::string_view key = drag_ ? "scrollbarSlider.activeBackground"
                                 : pointer_ && slider.contains(*pointer_)
                                     ? "scrollbarSlider.hoverBackground"
                                     : "scrollbarSlider.background";
    painter.fillRect(slider, color(key, QPalette::Mid));
}

void ScrollBar::mousePressEvent(QMouseEvent* event) {
    event->accept();
    const QRect slider = slider_rect();
    if (event->button() != Qt::LeftButton || slider.isEmpty()) {
        return;
    }
    const QPoint point = event->position().toPoint();
    pointer_ = point;
    setSliderDown(true);
    if (!slider.contains(point)) {
        const int size = orientation() == Qt::Vertical ? slider.height() : slider.width();
        put_slider_at(along(point) - size / 2.0);
    }
    drag_ = Drag{along(point), along(slider_rect().topLeft())};
    update();
}

void ScrollBar::mouseMoveEvent(QMouseEvent* event) {
    event->accept();
    pointer_ = event->position().toPoint();
    if (drag_) {
        put_slider_at(drag_->slider + along(*pointer_) - drag_->pointer);
    }
    update();
}

void ScrollBar::mouseReleaseEvent(QMouseEvent* event) {
    event->accept();
    if (event->button() != Qt::LeftButton || !drag_) {
        return;
    }
    drag_.reset();
    setSliderDown(false);
    pointer_ = event->position().toPoint();
    update();
}

void ScrollBar::enterEvent(QEnterEvent* event) {
    pointer_ = event->position().toPoint();
    update();
}

void ScrollBar::leaveEvent(QEvent*) {
    pointer_.reset();
    update();
}

void ScrollBar::hideEvent(QHideEvent* event) {
    // A hidden bar gets no release of the button.
    drag_.reset();
    pointer_.reset();
    setSliderDown(false);
    QScrollBar::hideEvent(event);
}

} // namespace anvil::ui
