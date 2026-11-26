#include "anvil/ui/zoom.hpp"

#include "anvil/core/zoom.hpp"

#include <QAbstractScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <algorithm>

namespace anvil::ui {

Zoom::Zoom(QObject* parent) : QObject(parent) {}

int Zoom::scaled(int size) const {
    return core::scale_for_zoom(size, level_);
}

void Zoom::set_level(double level) {
    level = std::clamp(level, core::min_zoom_level, core::max_zoom_level);
    if (level == level_) {
        return;
    }
    level_ = level;
    factor_ = core::zoom_factor(level);
    emit changed();
}

void scale_scroll_bars(QAbstractScrollArea& area, double level) {
    for (QScrollBar* bar : {area.verticalScrollBar(), area.horizontalScrollBar()}) {
        const int extent = area.style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, bar);
        const int scaled = core::scale_for_zoom(extent, level);
        if (bar->orientation() == Qt::Vertical) {
            bar->setFixedWidth(scaled);
        } else {
            bar->setFixedHeight(scaled);
        }
    }
}

} // namespace anvil::ui
