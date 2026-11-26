#pragma once

#include <array>
#include <cstddef>

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

// Whether the recent turns look like the notches of a mouse wheel and not like the movements of a
// touchpad, as VS Code's MouseWheelClassifier judges: turns of one axis, in whole notches or in
// multiples of each other, count as a wheel, and the later turns weigh more.
class WheelClassifier {
public:
    void accept(WheelTurn turn);
    // False before the first turn.
    bool is_mouse_wheel() const;

private:
    struct Turn {
        WheelTurn turn;
        // From 0 for a wheel to 1 for a touchpad.
        double score = 0;
    };

    static constexpr std::size_t remembered = 5;
    // The newest last.
    std::array<Turn, remembered> turns_;
    std::size_t count_ = 0;
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
