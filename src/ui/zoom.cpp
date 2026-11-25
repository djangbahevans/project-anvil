#include "anvil/ui/zoom.hpp"

#include "anvil/core/zoom.hpp"

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

} // namespace anvil::ui
