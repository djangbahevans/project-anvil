#include "anvil/ui/scroll_animator.hpp"

#include <QAbstractScrollArea>
#include <QScrollBar>
#include <algorithm>
#include <chrono>

namespace anvil::ui {
namespace {

constexpr int frame_ms = 16;

} // namespace

ScrollAnimator::ScrollAnimator(QAbstractScrollArea& area) : area_(area) {
    now_ = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };
    timer_.setTimerType(Qt::PreciseTimer);
    timer_.setInterval(frame_ms);
    connect(&timer_, &QTimer::timeout, this, &ScrollAnimator::step);
    for (QScrollBar* bar : {area_.horizontalScrollBar(), area_.verticalScrollBar()}) {
        connect(bar, &QScrollBar::valueChanged, this, [this] {
            if (!putting_) {
                stop();
            }
        });
    }
}

QPoint ScrollAnimator::position() const {
    return {area_.horizontalScrollBar()->value(), area_.verticalScrollBar()->value()};
}

QPoint ScrollAnimator::target() const {
    return movement_ ? QPoint(movement_->to().left, movement_->to().top) : position();
}

void ScrollAnimator::put(QPoint position) {
    putting_ = true;
    area_.horizontalScrollBar()->setValue(position.x());
    area_.verticalScrollBar()->setValue(position.y());
    putting_ = false;
}

void ScrollAnimator::scroll_to(QPoint wanted, bool smooth) {
    const auto within = [](const QScrollBar* bar, int value) {
        return std::clamp(value, bar->minimum(), bar->maximum());
    };
    const QPoint to(within(area_.horizontalScrollBar(), wanted.x()),
                    within(area_.verticalScrollBar(), wanted.y()));
    if (!smooth) {
        stop();
        put(to);
        return;
    }
    // A movement that already goes there goes on as it is.
    if (to == target()) {
        return;
    }
    const QPoint from = position();
    movement_.emplace(core::ScrollPosition{from.x(), from.y()},
                      core::ScrollPosition{to.x(), to.y()}, area_.viewport()->width(),
                      area_.viewport()->height(), now_());
    timer_.start();
}

void ScrollAnimator::step() {
    const std::int64_t now = now_();
    const core::ScrollPosition at = movement_->at(now);
    const bool done = movement_->done(now);
    if (done) {
        stop();
    }
    put({at.left, at.top});
}

void ScrollAnimator::finish() {
    if (movement_) {
        const QPoint to = target();
        stop();
        put(to);
    }
}

void ScrollAnimator::stop() {
    timer_.stop();
    movement_.reset();
}

} // namespace anvil::ui
