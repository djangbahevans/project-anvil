#include "anvil/ui/wheel_input.hpp"

#include <QWheelEvent>

namespace anvil::ui {
namespace {

constexpr double angle_per_notch = 120;

} // namespace

core::WheelTurn wheel_turn(const QWheelEvent& event, QStringView platform) {
    QPoint angle = event.angleDelta();
    // The X11 and Windows plugins of Qt swap the axes of the wheel while Alt is held, which would
    // turn fast scrolling sideways.
    if (event.modifiers().testFlag(Qt::AltModifier) &&
        (platform == u"xcb" || platform == u"windows")) {
        angle = angle.transposed();
    }
    return {angle.x() / angle_per_notch, angle.y() / angle_per_notch};
}

} // namespace anvil::ui
