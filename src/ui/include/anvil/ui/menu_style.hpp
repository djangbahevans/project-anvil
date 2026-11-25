#pragma once

#include "anvil/ui/workbench_colors.hpp"
#include "anvil/ui/zoom.hpp"

#include <QProxyStyle>

namespace anvil::ui {

// Draws the menus of the menu bar as VS Code does: a `check` codicon in a column at the left of a
// checked item, the label after it, the shortcut right-aligned at a fixed distance from the right
// edge, and a `chevron-right` codicon at the edge of a submenu item. The colors are the `menu.*`
// keys of `colors`, read when a menu is drawn, so a theme change shows in an open menu.
//
// A menu must not have a style sheet of its own, which makes `QStyleSheetStyle` draw the items;
// each menu, submenus included, is given this style with `QWidget::setStyle`.
class MenuStyle : public QProxyStyle {
public:
    // The measures of an item, in logical pixels at zoom level 0.
    static constexpr int item_height = 24;
    static constexpr int separator_height = 9;
    static constexpr int label_left = 30;
    static constexpr int check_column = 32;
    // The distance from the right edge of the menu to the end of a shortcut.
    static constexpr int shortcut_right = 30;
    static constexpr int chevron_center_from_right = 21;
    static constexpr int label_to_shortcut = 32;
    static constexpr int glyph_size = 16;
    static constexpr int frame_width = 1;
    static constexpr int vertical_margin = 4;

    explicit MenuStyle(const WorkbenchColors& colors);

    // The zoom that the measures are read through, which must outlive the style. The menus lay
    // themselves out when they are shown, so a change applies from the next time one opens. Null
    // means level 0.
    void set_zoom(const Zoom* zoom) {
        zoom_ = zoom;
    }
    int zoomed(int size) const {
        return scaled_by(zoom_, size);
    }

    void drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter,
                       const QWidget* widget) const override;
    void drawControl(ControlElement element, const QStyleOption* option, QPainter* painter,
                     const QWidget* widget) const override;
    QSize sizeFromContents(ContentsType type, const QStyleOption* option, const QSize& size,
                           const QWidget* widget) const override;
    int pixelMetric(PixelMetric metric, const QStyleOption* option,
                    const QWidget* widget) const override;

private:
    const WorkbenchColors& colors_;
    const Zoom* zoom_ = nullptr;
};

} // namespace anvil::ui
