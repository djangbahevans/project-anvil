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

void WheelClassifier::accept(WheelTurn turn) {
    const auto whole = [](double value) { return std::abs(std::round(value) - value) < 0.01; };
    double score = 0.5;
    if (turn.x != 0 && turn.y != 0) {
        score = 1;
    } else {
        if (!whole(turn.x) || !whole(turn.y)) {
            score += 0.25;
        }
        if (count_ > 0) {
            const WheelTurn before = turns_[count_ - 1].turn;
            const auto multiple = [](double a, double b) {
                const double smaller = std::max(std::min(std::abs(a), std::abs(b)), 1.0);
                return std::fmod(std::max(std::abs(a), std::abs(b)), smaller) == 0;
            };
            if (multiple(turn.x, before.x) && multiple(turn.y, before.y)) {
                score -= 0.5;
            }
        }
    }
    if (count_ == remembered) {
        std::shift_left(turns_.begin(), turns_.end(), 1);
        --count_;
    }
    turns_[count_++] = {turn, std::clamp(score, 0.0, 1.0)};
}

bool WheelClassifier::is_mouse_wheel() const {
    if (count_ == 0) {
        return false;
    }
    // Half for the newest turn, a quarter for the one before, and so on; the oldest takes what is
    // left.
    double left = 1;
    double score = 0;
    for (std::size_t age = 0; age < count_; ++age) {
        const double weight = age + 1 == count_ ? left : std::pow(0.5, age + 1);
        left -= weight;
        score += turns_[count_ - 1 - age].score * weight;
    }
    return score <= 0.5;
}

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
