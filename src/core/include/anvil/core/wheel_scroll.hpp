#pragma once

namespace anvil::core {

struct WheelSettings {
    // editor.mouseWheelScrollSensitivity
    double sensitivity = 1;
    // editor.fastScrollSensitivity: multiplies the sensitivity while Alt is held
    double fast_sensitivity = 5;
    // editor.scrollPredominantAxis: only the axis that turned further scrolls
    bool predominant_axis = true;

    friend bool operator==(const WheelSettings&, const WheelSettings&) = default;
};

// A turn of a wheel in notches, positive away from the user and to the left, as the angles of a
// wheel event are.
struct WheelTurn {
    double x = 0;
    double y = 0;
};

struct WheelKeys {
    bool alt = false;
    bool shift = false;
};

// Pixels to add to a scroll position: positive is toward the end of the text.
struct ScrollDistance {
    int x = 0;
    int y = 0;
};

// How far a turn of the wheel scrolls, by VS Code's rules: 50 pixels a notch, times the
// sensitivities, rounded away from zero so that the smallest turn moves a pixel. Shift turns a
// vertical turn sideways, except on macOS, where the system does that.
ScrollDistance wheel_scroll(WheelTurn turn, WheelKeys keys, bool macos,
                            const WheelSettings& settings);

} // namespace anvil::core
