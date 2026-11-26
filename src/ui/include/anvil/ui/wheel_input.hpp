#pragma once

#include "anvil/core/wheel_scroll.hpp"

#include <QStringView>

class QWheelEvent;

namespace anvil::ui {

// The turn of the wheel that an event reports, on the Qt platform plugin named `platform`
// (QGuiApplication::platformName).
core::WheelTurn wheel_turn(const QWheelEvent& event, QStringView platform);

} // namespace anvil::ui
