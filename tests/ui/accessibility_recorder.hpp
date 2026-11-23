#pragma once

#include "anvil/ui/painted_accessibility.hpp"

#include <QAccessible>
#include <QObject>
#include <vector>

// Records the notifications that the status bar and the tab strip send to assistive technology
// while it exists, which the platform of the tests would not deliver.
class AccessibilityRecorder {
public:
    struct Event {
        QAccessible::Event type;
        QObject* object;
        int child;
    };

    AccessibilityRecorder() {
        recorder_ = this;
        anvil::ui::set_accessible_event_observer(&AccessibilityRecorder::record);
    }
    ~AccessibilityRecorder() {
        anvil::ui::set_accessible_event_observer(nullptr);
        recorder_ = nullptr;
    }

    void clear() {
        events_.clear();
    }
    const std::vector<Event>& events() const {
        return events_;
    }
    bool contains(QAccessible::Event type, QObject* object, int child) const {
        for (const Event& event : events_) {
            if (event.type == type && event.object == object && event.child == child) {
                return true;
            }
        }
        return false;
    }

private:
    static void record(QWidget* widget, QAccessible::Event type, int child) {
        if (recorder_) {
            recorder_->events_.push_back({type, widget, child});
        }
    }

    inline static AccessibilityRecorder* recorder_ = nullptr;
    std::vector<Event> events_;
};
