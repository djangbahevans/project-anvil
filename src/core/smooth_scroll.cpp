#include "anvil/core/smooth_scroll.hpp"

#include <cmath>

namespace anvil::core {
namespace {

// VS Code counts a movement as having begun a little before it was asked for, so that the first
// step shows at once.
constexpr std::int64_t head_start = 10;
constexpr std::int64_t duration = 125 + head_start;

constexpr double long_way_in_views = 2.5;
constexpr double part_of_a_view = 0.75;
constexpr double first_part = 0.33;

double ease_out(double from, double to, double completion) {
    const double left = 1 - completion;
    return from + (to - from) * (1 - left * left * left);
}

int axis(int from, int to, int view, double completion) {
    if (std::abs(static_cast<double>(to) - from) <= long_way_in_views * view) {
        return static_cast<int>(ease_out(from, to, completion));
    }
    const double step = from < to ? part_of_a_view * view : -part_of_a_view * view;
    return static_cast<int>(
        completion < first_part
            ? ease_out(from, from + step, completion / first_part)
            : ease_out(to - step, to, (completion - first_part) / (1 - first_part)));
}

} // namespace

SmoothScroll::SmoothScroll(ScrollPosition from, ScrollPosition to, int width, int height,
                           std::int64_t now)
    : from_(from), to_(to), width_(width), height_(height), start_(now - head_start) {}

bool SmoothScroll::done(std::int64_t now) const {
    return now - start_ >= duration;
}

ScrollPosition SmoothScroll::at(std::int64_t now) const {
    if (done(now)) {
        return to_;
    }
    const double completion = static_cast<double>(now - start_) / static_cast<double>(duration);
    return {axis(from_.left, to_.left, width_, completion),
            axis(from_.top, to_.top, height_, completion)};
}

} // namespace anvil::core
