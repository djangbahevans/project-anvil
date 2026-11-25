#pragma once

namespace anvil::core {

// The zoom levels of VS Code's window: a level is a step of a factor of 1.2, from -8 to 8.
inline constexpr double min_zoom_level = -8;
inline constexpr double max_zoom_level = 8;

// 1.2 raised to `level`.
double zoom_factor(double level);

// A size in pixels at a zoom level: the size times the factor, rounded to the nearest whole pixel
// and at least 1 for a size above 0. A size of 0 or less is returned as it is.
int scale_for_zoom(int size, double level);

} // namespace anvil::core
