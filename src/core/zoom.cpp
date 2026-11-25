#include "anvil/core/zoom.hpp"

#include <algorithm>
#include <cmath>

namespace anvil::core {

double zoom_factor(double level) {
    return std::pow(1.2, level);
}

int scale_for_zoom(int size, double level) {
    if (size <= 0) {
        return size;
    }
    const long scaled = std::lround(static_cast<double>(size) * zoom_factor(level));
    return static_cast<int>(std::max(1L, scaled));
}

} // namespace anvil::core
