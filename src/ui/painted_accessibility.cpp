#include "anvil/ui/painted_accessibility.hpp"

#include "anvil/ui/status_bar.hpp"
#include "anvil/ui/tab_strip.hpp"

#include <QAccessibleWidget>
#include <QHash>
#include <QPointer>
#include <algorithm>

namespace anvil::ui {
namespace {

class PaintedItemAccessible : public QAccessibleInterface, public QAccessibleActionInterface {
public:
    PaintedItemAccessible(QWidget* widget, AccessibleItems* items, QString key)
        : widget_(widget), items_(items), key_(std::move(key)) {}

    const QString& key() const {
        return key_;
    }

    bool isValid() const override {
        return widget_ && find().has_value();
    }
    QObject* object() const override {
        return nullptr;
    }
    QWindow* window() const override {
        return widget_ ? widget_->windowHandle() : nullptr;
    }
    QAccessibleInterface* parent() const override {
        return widget_ ? QAccessible::queryAccessibleInterface(widget_) : nullptr;
    }
    QAccessibleInterface* child(int) const override {
        return nullptr;
    }
    QAccessibleInterface* childAt(int, int) const override {
        return nullptr;
    }
    int childCount() const override {
        return 0;
    }
    int indexOfChild(const QAccessibleInterface*) const override {
        return -1;
    }
    QList<QPair<QAccessibleInterface*, QAccessible::Relation>>
    relations(QAccessible::Relation) const override {
        return {};
    }

    QString text(QAccessible::Text text) const override {
        const auto item = find();
        if (!item) {
            return {};
        }
        switch (text) {
        case QAccessible::Name:
            return item->name;
        case QAccessible::Description:
            return item->description;
        default:
            return {};
        }
    }
    void setText(QAccessible::Text, const QString&) override {}
    QRect rect() const override {
        const auto item = find();
        if (!item || !widget_) {
            return {};
        }
        return QRect(widget_->mapToGlobal(item->rect.topLeft()), item->rect.size());
    }
    QAccessible::Role role() const override {
        const auto item = find();
        return item ? item->role : QAccessible::NoRole;
    }
    QAccessible::State state() const override {
        const auto item = find();
        return item ? item->state : QAccessible::State();
    }

    void* interface_cast(QAccessible::InterfaceType type) override {
        return type == QAccessible::ActionInterface ? static_cast<QAccessibleActionInterface*>(this)
                                                    : nullptr;
    }

    QStringList actionNames() const override {
        const auto item = find();
        return item ? item->actions : QStringList();
    }
    void doAction(const QString& name) override {
        if (isValid() && actionNames().contains(name)) {
            items_->do_accessible_action(key_, name);
        }
    }
    QStringList keyBindingsForAction(const QString&) const override {
        return {};
    }

private:
    std::optional<AccessibleItem> find() const {
        if (!widget_) {
            return std::nullopt;
        }
        for (AccessibleItem& item : items_->accessible_items()) {
            if (item.key == key_) {
                return std::move(item);
            }
        }
        return std::nullopt;
    }

    QPointer<QWidget> widget_;
    AccessibleItems* items_;
    QString key_;
};

class PaintedItemsAccessible : public QAccessibleWidget {
public:
    PaintedItemsAccessible(QWidget* widget, AccessibleItems* items, QAccessible::Role role)
        : QAccessibleWidget(widget, role), widget_(widget), items_(items) {}

    ~PaintedItemsAccessible() override {
        for (const QAccessible::Id id : ids_) {
            QAccessible::deleteAccessibleInterface(id);
        }
    }

    int childCount() const override {
        const auto items = items_->accessible_items();
        prune(items);
        return static_cast<int>(items.size());
    }

    QAccessibleInterface* child(int index) const override {
        const auto items = items_->accessible_items();
        if (index < 0 || index >= static_cast<int>(items.size())) {
            return nullptr;
        }
        return interface_of(items[static_cast<std::size_t>(index)].key);
    }

    int indexOfChild(const QAccessibleInterface* child) const override {
        const auto* item = dynamic_cast<const PaintedItemAccessible*>(child);
        if (!item) {
            return -1;
        }
        const auto items = items_->accessible_items();
        const auto found = std::ranges::find(items, item->key(), &AccessibleItem::key);
        return found == items.end() ? -1 : static_cast<int>(found - items.begin());
    }

    QAccessibleInterface* childAt(int x, int y) const override {
        const QPoint local = widget_->mapFromGlobal(QPoint(x, y));
        for (const AccessibleItem& item : items_->accessible_items()) {
            if (item.rect.contains(local)) {
                return interface_of(item.key);
            }
        }
        return nullptr;
    }

private:
    QAccessibleInterface* interface_of(const QString& key) const {
        auto found = ids_.find(key);
        if (found == ids_.end()) {
            const QAccessible::Id id = QAccessible::registerAccessibleInterface(
                new PaintedItemAccessible(widget_, items_, key));
            found = ids_.insert(key, id);
        }
        return QAccessible::accessibleInterface(*found);
    }

    // Forgets the interfaces of items that are gone, so that a bar whose items come and go does
    // not collect them.
    void prune(const std::vector<AccessibleItem>& items) const {
        for (auto it = ids_.begin(); it != ids_.end();) {
            const QString& key = it.key();
            if (std::ranges::none_of(items,
                                     [&](const AccessibleItem& item) { return item.key == key; })) {
                QAccessible::deleteAccessibleInterface(it.value());
                it = ids_.erase(it);
            } else {
                ++it;
            }
        }
    }

    QWidget* widget_;
    AccessibleItems* items_;
    mutable QHash<QString, QAccessible::Id> ids_;
};

QAccessibleInterface* painted_factory(const QString&, QObject* object) {
    if (auto* bar = qobject_cast<StatusBar*>(object)) {
        return new PaintedItemsAccessible(bar, bar, QAccessible::StatusBar);
    }
    if (auto* strip = qobject_cast<TabStrip*>(object)) {
        return new PaintedItemsAccessible(strip, strip, QAccessible::PageTabList);
    }
    return nullptr;
}

} // namespace

void install_painted_accessibility() {
    static const bool installed = [] {
        QAccessible::installFactory(&painted_factory);
        return true;
    }();
    (void)installed;
}

namespace {

AccessibleEventObserver observer = nullptr;

} // namespace

bool accessible_events_wanted() {
    return observer != nullptr || QAccessible::isActive();
}

void set_accessible_event_observer(AccessibleEventObserver new_observer) {
    observer = new_observer;
}

void notify_accessible_item(QWidget* widget, QAccessible::Event event, int index) {
    if (observer) {
        observer(widget, event, index);
    }
    if (!QAccessible::isActive()) {
        return;
    }
    QAccessibleEvent accessible_event(widget, event);
    accessible_event.setChild(index);
    QAccessible::updateAccessibility(&accessible_event);
}

void notify_accessible_item_state(QWidget* widget, QAccessible::State changed, int index) {
    if (observer) {
        observer(widget, QAccessible::StateChanged, index);
    }
    if (!QAccessible::isActive()) {
        return;
    }
    QAccessibleStateChangeEvent accessible_event(widget, changed);
    accessible_event.setChild(index);
    QAccessible::updateAccessibility(&accessible_event);
}

} // namespace anvil::ui
