#include "anvil/ui/splitter.hpp"

#include <QEnterEvent>
#include <QPainter>
#include <QSplitterHandle>

namespace anvil::ui {
namespace {

class Sash : public QSplitterHandle {
public:
    Sash(Qt::Orientation orientation, const WorkbenchColors& colors, std::string_view border,
         QSplitter* parent)
        : QSplitterHandle(orientation, parent), colors_(colors), border_(border) {}

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
        QSplitterHandle::mousePressEvent(event);
        pressed_ = true;
        update();
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        QSplitterHandle::mouseReleaseEvent(event);
        pressed_ = false;
        update();
    }
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), colors_.color("editor.background"));
        if (hovered_ || pressed_) {
            painter.fillRect(rect(), colors_.color("sash.hoverBorder"));
            return;
        }
        const QColor line = colors_.color(border_);
        const int middle = orientation() == Qt::Horizontal ? width() / 2 : height() / 2;
        if (orientation() == Qt::Horizontal) {
            painter.fillRect(QRect(middle, 0, 1, height()), line);
        } else {
            painter.fillRect(QRect(0, middle, width(), 1), line);
        }
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
    setHandleWidth(sash_width);
    setChildrenCollapsible(false);
}

QSplitterHandle* Splitter::createHandle() {
    return new Sash(orientation(), colors_, border_, this);
}

} // namespace anvil::ui
