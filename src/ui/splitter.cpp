#include "anvil/ui/splitter.hpp"

#include <QEnterEvent>
#include <QPainter>
#include <QSplitterHandle>
#include <algorithm>

namespace anvil::ui {
namespace {

// QSplitter lays the handles over the widgets beside them, by their contents margins, which is how
// Qt widens the grab area of a handle of at most a pixel (QSplitterHandle::resizeEvent). Qt's
// margins do not follow the zoom, so the handle sets its own. The widgets show through the margins
// until the whole grab area takes the hover color.
class Sash : public QSplitterHandle {
public:
    Sash(Qt::Orientation orientation, const WorkbenchColors& colors, std::string_view border,
         int reach, QSplitter* parent)
        : QSplitterHandle(orientation, parent), colors_(colors), border_(border) {
        set_reach(reach);
    }

    void set_reach(int reach) {
        setContentsMargins(orientation() == Qt::Horizontal ? QMargins(reach, 0, reach, 0)
                                                           : QMargins(0, reach, 0, reach));
    }

protected:
    void enterEvent(QEnterEvent* event) override {
        QSplitterHandle::enterEvent(event);
        hovered_ = true;
        update();
    }
    void leaveEvent(QEvent* event) override {
        QSplitterHandle::leaveEvent(event);
        hovered_ = false;
        update();
    }
    void mousePressEvent(QMouseEvent* event) override {
        // QSplitterHandle remembers where in its widget the press was and moves the line to the
        // pointer less that, but the widget begins before the line, by the margin.
        QMouseEvent from_line(event->type(), event->position() - contentsRect().topLeft(),
                              event->globalPosition(), event->button(), event->buttons(),
                              event->modifiers());
        QSplitterHandle::mousePressEvent(&from_line);
        pressed_ = true;
        update();
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        QSplitterHandle::mouseReleaseEvent(event);
        pressed_ = false;
        update();
    }
    // Not QSplitterHandle's, which would set its own margins.
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
    }
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        if (hovered_ || pressed_) {
            painter.fillRect(rect(), colors_.color("sash.hoverBorder"));
            return;
        }
        painter.fillRect(contentsRect(), colors_.color("editor.background"));
        painter.fillRect(contentsRect(), colors_.color(border_));
    }

private:
    const WorkbenchColors& colors_;
    std::string_view border_;
    bool hovered_ = false;
    bool pressed_ = false;
};

} // namespace

Splitter::Splitter(Qt::Orientation orientation, const WorkbenchColors& colors,
                   std::string_view border, QWidget* parent)
    : QSplitter(orientation, parent), colors_(colors), border_(border) {
    setHandleWidth(line_width);
    setChildrenCollapsible(false);
}

void Splitter::set_zoom(const Zoom* zoom) {
    if (zoom_) {
        disconnect(zoom_, &Zoom::changed, this, nullptr);
    }
    zoom_ = zoom;
    const auto apply = [this] {
        for (int i = 0; i < count(); ++i) {
            static_cast<Sash*>(handle(i))->set_reach(reach());
        }
    };
    if (zoom_) {
        connect(zoom_, &Zoom::changed, this, apply);
    }
    apply();
}

int Splitter::reach() const {
    return std::max(2, scaled_by(zoom_, sash_width) / 2);
}

QSplitterHandle* Splitter::createHandle() {
    return new Sash(orientation(), colors_, border_, reach(), this);
}

} // namespace anvil::ui
