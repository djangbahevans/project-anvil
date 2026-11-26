#pragma once

#include "anvil/ui/workbench_colors.hpp"

#include <QScrollBar>
#include <optional>
#include <string_view>

namespace anvil::ui {

// A scroll bar as VS Code draws it: no arrow buttons, and a slider in the `scrollbarSlider.*`
// colors of the theme on a track that shows `scrollbar.background` when the theme sets it. A press
// on the track brings the middle of the slider to the pointer. The bar takes room of its own beside
// the content, so it paints the color of that content, which `surface` names, below the track. The
// keyboard and the wheel work as they do for any QScrollBar.
class ScrollBar : public QScrollBar {
    Q_OBJECT

public:
    // The sizes at zoom level 0.
    static constexpr int editor_vertical_width = 14;
    static constexpr int editor_horizontal_height = 12;
    static constexpr int list_width = 10;
    static constexpr int minimum_slider_length = 20;

    // `extent` is the width of a vertical bar or the height of a horizontal one at zoom level 0.
    // `surface` is a color key, and must outlive the bar.
    ScrollBar(Qt::Orientation orientation, int extent, std::string_view surface,
              QWidget* parent = nullptr);

    // The colors, which must outlive the bar; null takes them from the palette.
    void set_colors(const WorkbenchColors* colors);
    void set_zoom_level(double level);

    // Where the slider is, which is empty when there is nothing to scroll.
    QRect slider_rect() const;
    // A scroll area makes room for a bar by its size hint, so the hint has the bar's thickness.
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    struct Drag {
        // Along the bar: where the pointer was pressed, and where the slider was then.
        int pointer = 0;
        int slider = 0;
    };

    int along(const QPoint& point) const;
    int length() const;
    // Scrolls so that the slider starts `position` pixels along the bar.
    void put_slider_at(double position);

    const WorkbenchColors* colors_ = nullptr;
    QMetaObject::Connection colors_connection_;
    std::string_view surface_;
    int extent_;
    double zoom_level_ = 0;
    std::optional<QPoint> pointer_;
    std::optional<Drag> drag_;
};

} // namespace anvil::ui
