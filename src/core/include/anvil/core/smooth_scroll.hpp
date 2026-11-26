#pragma once

#include <cstdint>

namespace anvil::core {

struct ScrollPosition {
    int left = 0;
    int top = 0;

    friend bool operator==(const ScrollPosition&, const ScrollPosition&) = default;
};

// One animated movement of a scroll position, as VS Code's SmoothScrollingOperation makes it: over
// 125 ms with an ease-out cubic curve, and in two parts that skip the middle when the way on an
// axis is longer than two and a half views. A new target during a movement is a new movement from
// where the position is then. Times are in milliseconds of any steady clock.
class SmoothScroll {
public:
    // A movement asked for at `now`, in a view of `width` by `height` pixels.
    SmoothScroll(ScrollPosition from, ScrollPosition to, int width, int height, std::int64_t now);

    ScrollPosition to() const {
        return to_;
    }
    bool done(std::int64_t now) const;
    ScrollPosition at(std::int64_t now) const;

private:
    ScrollPosition from_;
    ScrollPosition to_;
    int width_;
    int height_;
    std::int64_t start_;
};

} // namespace anvil::core
