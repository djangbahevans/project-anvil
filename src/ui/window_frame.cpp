#include "anvil/ui/window_frame.hpp"

#include <QChildEvent>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QWindow>

namespace anvil::ui {

Qt::Edges resize_edges_at(QSize size, QPoint point, int margin) {
    if (point.x() < 0 || point.y() < 0 || point.x() >= size.width() || point.y() >= size.height()) {
        return {};
    }
    Qt::Edges edges;
    if (point.x() < margin) {
        edges |= Qt::LeftEdge;
    }
    if (point.x() >= size.width() - margin) {
        edges |= Qt::RightEdge;
    }
    if (point.y() < margin) {
        edges |= Qt::TopEdge;
    }
    if (point.y() >= size.height() - margin) {
        edges |= Qt::BottomEdge;
    }
    return edges;
}

Qt::CursorShape resize_cursor(Qt::Edges edges) {
    const bool horizontal = edges & (Qt::LeftEdge | Qt::RightEdge);
    const bool vertical = edges & (Qt::TopEdge | Qt::BottomEdge);
    if (horizontal && vertical) {
        const bool main_diagonal = (edges & Qt::LeftEdge && edges & Qt::TopEdge) ||
                                   (edges & Qt::RightEdge && edges & Qt::BottomEdge);
        return main_diagonal ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor;
    }
    if (horizontal) {
        return Qt::SizeHorCursor;
    }
    return vertical ? Qt::SizeVerCursor : Qt::ArrowCursor;
}

WindowFrame::WindowFrame(QWidget* window) : QObject(window), window_(window) {
    track_mouse(window);
    qApp->installEventFilter(this);
}

WindowFrame::~WindowFrame() {
    show_cursor({});
}

void WindowFrame::set_enabled(bool enabled) {
    enabled_ = enabled;
    if (!enabled) {
        show_cursor({});
    }
}

bool WindowFrame::active() const {
    return enabled_ && window_ && !window_->isMaximized() && !window_->isFullScreen();
}

void WindowFrame::show_cursor(Qt::Edges edges) {
    if (edges) {
        if (cursor_overridden_) {
            QGuiApplication::changeOverrideCursor(resize_cursor(edges));
        } else {
            QGuiApplication::setOverrideCursor(resize_cursor(edges));
            cursor_overridden_ = true;
        }
    } else if (cursor_overridden_) {
        QGuiApplication::restoreOverrideCursor();
        cursor_overridden_ = false;
    }
}

// A mouse move without a button reaches only the widgets that ask for it, and the cursor over a
// handle follows the moves over whatever lies under it.
void WindowFrame::track_mouse(QWidget* widget) {
    widget->setMouseTracking(true);
    for (QWidget* child : widget->findChildren<QWidget*>()) {
        child->setMouseTracking(true);
    }
}

bool WindowFrame::eventFilter(QObject* watched, QEvent* event) {
    if (!window_) {
        return false;
    }
    switch (event->type()) {
    case QEvent::ChildAdded: {
        auto* child = qobject_cast<QWidget*>(static_cast<QChildEvent*>(event)->child());
        if (child && child->window() == window_) {
            track_mouse(child);
        }
        return false;
    }
    case QEvent::Leave:
        if (watched == window_) {
            show_cursor({});
        }
        return false;
    case QEvent::MouseMove:
    case QEvent::MouseButtonPress:
        break;
    default:
        return false;
    }

    const auto* widget = qobject_cast<QWidget*>(watched);
    if (!widget || widget->window() != window_) {
        return false;
    }
    const auto* mouse = static_cast<QMouseEvent*>(event);
    if (event->type() == QEvent::MouseMove && window_->isFullScreen()) {
        // A move that a widget ignores reaches its parents too, and reaches the filter again.
        const int y = window_->mapFromGlobal(mouse->globalPosition().toPoint()).y();
        if (y != last_fullscreen_y_) {
            last_fullscreen_y_ = y;
            emit fullscreen_pointer_moved(y);
        }
    }
    const Qt::Edges edges =
        active() ? resize_edges_at(window_->size(),
                                   window_->mapFromGlobal(mouse->globalPosition().toPoint()),
                                   handle_width)
                 : Qt::Edges();
    if (event->type() == QEvent::MouseMove) {
        show_cursor(edges);
        return false;
    }
    if (mouse->button() != Qt::LeftButton || !edges) {
        return false;
    }
    if (!window_->windowHandle() || !window_->windowHandle()->startSystemResize(edges)) {
        emit resize_failed();
    }
    return true;
}

FrameBorder::FrameBorder(const WorkbenchColors& colors, QWidget* window)
    : QWidget(window), colors_(colors) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setGeometry(window->rect());
    window->installEventFilter(this);
    connect(&colors_, &WorkbenchColors::changed, this, qOverload<>(&QWidget::update));
    raise();
}

void FrameBorder::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setPen(colors_.color("titleBar.border"));
    painter.drawRect(rect().adjusted(0, 0, -1, -1));
}

bool FrameBorder::eventFilter(QObject* watched, QEvent* event) {
    if (watched == parentWidget() &&
        (event->type() == QEvent::Resize || event->type() == QEvent::ChildAdded)) {
        setGeometry(parentWidget()->rect());
        // A child added later is stacked above this one.
        QMetaObject::invokeMethod(this, qOverload<>(&QWidget::raise), Qt::QueuedConnection);
    }
    return false;
}

} // namespace anvil::ui
