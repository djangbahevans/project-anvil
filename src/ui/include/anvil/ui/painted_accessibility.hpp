#pragma once

#include <QAccessible>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <vector>

namespace anvil::ui {

// What assistive technology sees of an item that a widget paints itself.
struct AccessibleItem {
    // Identifies the item for as long as it exists, whatever its position becomes.
    QString key;
    QString name;
    QString description;
    QAccessible::Role role = QAccessible::StaticText;
    QAccessible::State state = {};
    // In the coordinates of the widget.
    QRect rect;
    // The actions the item offers, such as `QAccessibleActionInterface::pressAction()`.
    QStringList actions;
};

// A widget that paints its items, instead of holding a widget for each, implements this to expose
// them as the children of its accessible interface.
class AccessibleItems {
public:
    virtual ~AccessibleItems() = default;
    // The items that are shown, in the order they are read.
    virtual std::vector<AccessibleItem> accessible_items() const = 0;
    // Does what the user does with the item when they ask for `action`.
    virtual void do_accessible_action(const QString& key, const QString& action) = 0;
};

// Makes the status bar and the tab strip expose their items, once for the application.
void install_painted_accessibility();

// Tells assistive technology that the item at `index` of `widget` changed. Does nothing, and costs
// nothing, while no assistive technology is running.
void notify_accessible_item(QWidget* widget, QAccessible::Event event, int index);
void notify_accessible_item_state(QWidget* widget, QAccessible::State changed, int index);
// Whether those notifications go anywhere, so that work done only to make one can be skipped.
bool accessible_events_wanted();

// Receives the notifications in place of assistive technology, which a platform without
// accessibility, such as the one the tests run on, never reports as running. Null removes it.
using AccessibleEventObserver = void (*)(QWidget* widget, QAccessible::Event event, int index);
void set_accessible_event_observer(AccessibleEventObserver observer);

} // namespace anvil::ui
