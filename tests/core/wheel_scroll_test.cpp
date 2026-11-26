#include "anvil/core/wheel_scroll.hpp"

#include <cmath>
#include <gtest/gtest.h>
#include <random>
#include <vector>

using anvil::core::ScrollDistance;
using anvil::core::wheel_scroll;
using anvil::core::WheelClassifier;
using anvil::core::WheelKeys;
using anvil::core::WheelSettings;
using anvil::core::WheelTurn;

namespace {

constexpr WheelKeys no_keys{};
constexpr WheelKeys alt{.alt = true};
constexpr WheelKeys shift{.shift = true};
constexpr WheelKeys alt_shift{.alt = true, .shift = true};

struct Case {
    const char* name;
    WheelTurn turn;
    WheelKeys keys;
    WheelSettings settings;
    int x;
    int y;
    bool macos = false;
};

TEST(WheelScroll, FollowsTheRules) {
    const WheelSettings both{.predominant_axis = false};
    const Case cases[] = {
        {"a notch toward the user", {0, -1}, no_keys, {}, 0, 50},
        {"a notch away", {0, 1}, no_keys, {}, 0, -50},
        {"a notch to the right", {-1, 0}, no_keys, {}, 50, 0},
        {"no turn", {0, 0}, no_keys, {}, 0, 0},
        {"a slow turn down moves a pixel", {0, -0.001}, no_keys, {}, 0, 1},
        {"a slow turn up moves a pixel", {0, 0.001}, no_keys, {}, 0, -1},
        {"a part of a pixel is rounded away from zero", {0, -0.03}, no_keys, {}, 0, 2},
        {"the sensitivity multiplies", {0, -1}, no_keys, {.sensitivity = 3}, 0, 150},
        {"a negative sensitivity reverses", {0, -1}, no_keys, {.sensitivity = -1}, 0, -50},
        {"Alt multiplies", {0, -1}, alt, {}, 0, 250},
        {"Alt multiplies by its setting", {0, -1}, alt, {.fast_sensitivity = 2}, 0, 100},
        {"both multiply", {0, -1}, alt, {.sensitivity = 2, .fast_sensitivity = 3}, 0, 300},
        {"Shift turns sideways", {0, -1}, shift, {}, 50, 0},
        {"Shift and Alt", {0, -1}, alt_shift, {}, 250, 0},
        {"Shift leaves a sideways turn", {-1, 0}, shift, {}, 50, 0},
        {"Shift does nothing on macOS", {0, -1}, shift, {}, 0, 50, true},
        {"Shift needs one axis", {-1, -2}, shift, both, 50, 100},
        {"the larger turn wins", {-1, -2}, no_keys, {}, 0, 100},
        {"the larger turn wins sideways", {-2, -1}, no_keys, {}, 100, 0},
        {"a tie scrolls up and down", {1, -1}, no_keys, {}, 0, 50},
        {"Shift applies to the winner", {-1, -2}, shift, {}, 100, 0},
        {"both axes without a winner", {-2, -1}, no_keys, both, 100, 50},
        {"a huge turn stays in range", {0, -1}, no_keys, {.sensitivity = 1e300}, 0, 1 << 30},
        {"and the other way", {0, 1}, no_keys, {.sensitivity = 1e300}, 0, -(1 << 30)},
    };
    for (const Case& c : cases) {
        const ScrollDistance result = wheel_scroll(c.turn, c.keys, c.macos, c.settings);
        EXPECT_EQ(result.x, c.x) << c.name;
        EXPECT_EQ(result.y, c.y) << c.name;
    }
}

int rounded_away_from_zero(double value) {
    return static_cast<int>(std::copysign(std::ceil(std::abs(value)), value));
}

TEST(WheelScroll, AgreesWithAPlainReference) {
    std::mt19937 random(240);
    // Angles as wheels and touchpads report them, in eighths of a degree.
    std::uniform_int_distribution<int> angle(-480, 480);
    std::uniform_real_distribution<double> sensitivity(-3, 8);
    std::bernoulli_distribution flag(0.5);
    std::bernoulli_distribution still(0.3);
    for (int run = 0; run < 20000; ++run) {
        const WheelTurn turn{still(random) ? 0 : angle(random) / 120.0,
                             still(random) ? 0 : angle(random) / 120.0};
        const WheelKeys keys{.alt = flag(random), .shift = flag(random)};
        const WheelSettings settings{.sensitivity = sensitivity(random),
                                     .fast_sensitivity = std::abs(sensitivity(random)),
                                     .predominant_axis = flag(random)};
        const bool macos = flag(random);

        const bool vertical = std::abs(turn.y) >= std::abs(turn.x);
        double x = settings.predominant_axis && vertical ? 0 : turn.x;
        double y = settings.predominant_axis && !vertical ? 0 : turn.y;
        // A sensitivity of zero leaves nothing for Shift to move.
        if (keys.shift && !macos && x * settings.sensitivity == 0) {
            x = y;
            y = 0;
        }
        const auto pixels = [&](double notches) {
            double scaled = notches * settings.sensitivity;
            if (keys.alt) {
                scaled *= settings.fast_sensitivity;
            }
            return -rounded_away_from_zero(50 * scaled);
        };

        const ScrollDistance result = wheel_scroll(turn, keys, macos, settings);
        ASSERT_EQ(result.x, pixels(x)) << "run " << run;
        ASSERT_EQ(result.y, pixels(y)) << "run " << run;
    }
}

bool wheel_after(std::initializer_list<WheelTurn> turns) {
    WheelClassifier classifier;
    for (const WheelTurn& turn : turns) {
        classifier.accept(turn);
    }
    return classifier.is_mouse_wheel();
}

TEST(WheelClassifier, TellsNotchesFromTheMovementsOfATouchpad) {
    EXPECT_FALSE(wheel_after({})) << "nothing has turned";
    EXPECT_TRUE(wheel_after({{0, -1}}));
    EXPECT_TRUE(wheel_after({{0, -1}, {0, -1}, {0, 1}}));
    EXPECT_TRUE(wheel_after({{0, -1}, {0, -3}, {0, -2}})) << "an accelerated wheel";
    EXPECT_TRUE(wheel_after({{-1, 0}, {-1, 0}})) << "a wheel that tilts";
    EXPECT_FALSE(wheel_after({{0, -0.125}}));
    EXPECT_FALSE(wheel_after({{0, -0.125}, {0, -0.125}, {0, -0.125}}));
    EXPECT_FALSE(wheel_after({{0.5, -1}})) << "both axes at once";
    EXPECT_FALSE(wheel_after({{0, -1.3}, {0, -2.45}}));
}

TEST(WheelClassifier, TheLaterTurnsWeighMore) {
    EXPECT_TRUE(wheel_after({{0, -0.125}, {0, -1}, {0, -1}}));
    EXPECT_FALSE(wheel_after({{0, -1}, {0, -1}, {0, -0.125}, {0, -0.25}, {0, -0.125}}));
    // A touchpad's turn is forgotten once five others have come.
    EXPECT_TRUE(wheel_after({{1, 1}, {0, -1}, {0, -1}, {0, -1}, {0, -1}, {0, -1}}));
}

// The classifier of `scrollableElement.ts`, over the whole history.
bool classified_as_wheel(const std::vector<WheelTurn>& turns) {
    std::vector<double> scores;
    for (std::size_t i = 0; i < turns.size(); ++i) {
        const double x = turns[i].x;
        const double y = turns[i].y;
        if (std::abs(x) > 0 && std::abs(y) > 0) {
            scores.push_back(1);
            continue;
        }
        double score = 0.5;
        const auto almost_whole = [](double v) { return std::abs(std::round(v) - v) < 0.01; };
        if (!almost_whole(x) || !almost_whole(y)) {
            score += 0.25;
        }
        if (i > 0) {
            const double px = std::abs(turns[i - 1].x);
            const double py = std::abs(turns[i - 1].y);
            const double min_x = std::max(std::min(std::abs(x), px), 1.0);
            const double min_y = std::max(std::min(std::abs(y), py), 1.0);
            if (std::fmod(std::max(std::abs(x), px), min_x) == 0 &&
                std::fmod(std::max(std::abs(y), py), min_y) == 0) {
                score -= 0.5;
            }
        }
        scores.push_back(std::min(std::max(score, 0.0), 1.0));
    }
    const std::size_t oldest = scores.size() > 5 ? scores.size() - 5 : 0;
    double left = 1;
    double score = 0;
    int iteration = 1;
    for (std::size_t i = scores.size(); i-- > oldest; ++iteration) {
        const double influence = i == oldest ? left : std::pow(2, -iteration);
        left -= influence;
        score += scores[i] * influence;
    }
    return score <= 0.5;
}

TEST(WheelClassifier, AgreesWithAPlainReference) {
    std::mt19937 random(2430);
    // Whole notches, parts of them, and none.
    const double values[] = {0, 0, 0, 1, -1, 2, -3, 0.125, -0.5, 1.5, 2.995, 15.0 / 120};
    std::uniform_int_distribution<std::size_t> pick(0, std::size(values) - 1);
    for (int run = 0; run < 2000; ++run) {
        WheelClassifier classifier;
        std::vector<WheelTurn> turns;
        for (int step = 0; step < 12; ++step) {
            turns.push_back({values[pick(random)], values[pick(random)]});
            classifier.accept(turns.back());
            ASSERT_EQ(classifier.is_mouse_wheel(), classified_as_wheel(turns))
                << "run " << run << " step " << step;
        }
    }
}

} // namespace
