#pragma once

#include "anvil/ui/workbench_colors.hpp"

#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QSize>
#include <QWidget>
#include <Qt>

namespace anvil::ui {

// Which edges of a window of `size` the point is on, with handles that are the outermost `margin`
// pixels of the window itself. Two edges are a corner.
Qt::Edges resize_edges_at(QSize size, QPoint point, int margin);
// The cursor that shows a resize from these edges, the arrow for none.
Qt::CursorShape resize_cursor(Qt::Edges edges);

// The resize handles of a window without a frame (ADR 0015). The handles are inside the window,
// over whatever the window shows there: a transparent margin around the painted window would be
// outside it and gets no input under some compositors. An event filter on the application sees the
// mouse before the widget under it does, shows the resize cursor over a handle, and starts a
// resize by the window system when the left button goes down on one. Handles are off while the
// window is maximized or fullscreen, and while the filter is disabled.
class WindowFrame : public QObject {
    Q_OBJECT

public:
    static constexpr int handle_width = 5;

    explicit WindowFrame(QWidget* window);
    ~WindowFrame() override;

    bool enabled() const {
        return enabled_;
    }
    void set_enabled(bool enabled);

signals:
    // The window system did not start the resize that was asked for.
    void resize_failed();
    // The pointer moved over the window while it is fullscreen, to a new distance `y` from the top.
    void fullscreen_pointer_moved(int y);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    bool active() const;
    void show_cursor(Qt::Edges edges);
    void track_mouse(QWidget* widget);

    QPointer<QWidget> window_;
    bool enabled_ = true;
    bool cursor_overridden_ = false;
    int last_fullscreen_y_ = -1;
};

// A 1 px line around a window that has no frame, in `titleBar.border`, so that its edge shows when
// the window system draws no shadow. It covers the window, lets the mouse through and stays above
// the window's other children.
class FrameBorder : public QWidget {
    Q_OBJECT

public:
    FrameBorder(const WorkbenchColors& colors, QWidget* window);

protected:
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    const WorkbenchColors& colors_;
};

} // namespace anvil::ui
