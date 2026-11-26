#pragma once

#include "anvil/core/smooth_scroll.hpp"

#include <QObject>
#include <QPoint>
#include <QTimer>
#include <cstdint>
#include <functional>
#include <optional>

class QAbstractScrollArea;

namespace anvil::ui {

// Moves the scroll bars of an area to a position, at once or over time (core::SmoothScroll). A
// change to a bar's value by anything else ends a movement where that change put it.
class ScrollAnimator : public QObject {
public:
    // `area` must have its scroll bars already, and keep them.
    explicit ScrollAnimator(QAbstractScrollArea& area);

    // Where the area is going: the end of the movement, or where it is.
    QPoint target() const;
    // Goes to `position`, kept within the ranges of the bars.
    void scroll_to(QPoint position, bool smooth);
    // Ends a movement at its target.
    void finish();
    // Ends a movement where it is.
    void stop();
    bool moving() const {
        return timer_.isActive();
    }

    // The clock of the movements, in milliseconds.
    void set_clock(std::function<std::int64_t()> now) {
        now_ = std::move(now);
    }

private:
    QPoint position() const;
    void put(QPoint position);
    void step();

    QAbstractScrollArea& area_;
    QTimer timer_;
    std::optional<core::SmoothScroll> movement_;
    std::function<std::int64_t()> now_;
    bool putting_ = false;
};

} // namespace anvil::ui
