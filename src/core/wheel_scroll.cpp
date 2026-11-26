#include "anvil/core/wheel_scroll.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace anvil::core {
namespace {

constexpr double pixels_per_notch = 50;
// Far enough to reach either end of anything that scrolls, and safe to add to a position.
constexpr double farthest = 1 << 30;

int distance(double notches) {
    const double exact = pixels_per_notch * notches;
    const double whole = exact < 0 ? std::floor(exact) : std::ceil(exact);
    return static_cast<int>(-std::clamp(whole, -farthest, farthest));
}

} // namespace

ScrollDistance wheel_scroll(WheelTurn turn, WheelKeys keys, bool macos,
                            const WheelSettings& settings) {
    double x = turn.x * settings.sensitivity;
    double y = turn.y * settings.sensitivity;
    if (settings.predominant_axis) {
        (std::abs(y) >= std::abs(x) ? x : y) = 0;
    }
    if (keys.shift && !macos && x == 0) {
        std::swap(x, y);
    }
    if (keys.alt) {
        x *= settings.fast_sensitivity;
        y *= settings.fast_sensitivity;
    }
    return {distance(x), distance(y)};
}

} // namespace anvil::core
