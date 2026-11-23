#include "anvil/ui/status_bar.hpp"

#include <QFontMetrics>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>
#include <algorithm>

namespace anvil::ui {

StatusBar::StatusBar(const WorkbenchColors& colors, QWidget* parent)
    : QWidget(parent), colors_(colors) {
    setFixedHeight(height_pixels);
    setMouseTracking(true);
    // Before the name is set, which makes Qt look for the interface of the widget and keep it.
    install_painted_accessibility();
    setAccessibleName(tr("Status Bar"));
    QFont bar_font = font();
    bar_font.setPixelSize(font_pixels);
    setFont(bar_font);

    message_timer_.setSingleShot(true);
    connect(&message_timer_, &QTimer::timeout, this, &StatusBar::clear_message);
    connect(&colors_, &WorkbenchColors::changed, this, qOverload<>(&StatusBar::update));
}

void StatusBar::set_item(const std::string& id, StatusBarSide side, StatusBarItem item) {
    const auto found = std::ranges::find(entries_, id, &Entry::id);
    if (found == entries_.end()) {
        entries_.push_back({.id = id, .side = side, .item = std::move(item), .rect = {}});
        layout_valid_ = false;
        update();
        return;
    }
    if (found->side == side && found->item == item) {
        return;
    }
    const bool retexted = found->item.text != item.text;
    const bool retipped = found->item.tooltip != item.tooltip;
    const bool moves = found->side != side || found->item.text != item.text ||
                       found->item.priority != item.priority;
    found->side = side;
    found->item = std::move(item);
    if (found->item.command.empty() && hovered_ == id) {
        set_hovered({});
    }
    if (moves) {
        layout_valid_ = false;
    }
    update();
    if (accessible_events_wanted() && (retexted || retipped)) {
        const int index = accessible_index(id);
        if (index >= 0) {
            if (retexted) {
                notify_accessible_item(this, QAccessible::NameChanged, index);
            }
            if (retipped) {
                notify_accessible_item(this, QAccessible::DescriptionChanged, index);
            }
        }
    }
}

bool StatusBar::remove_item(const std::string& id) {
    const auto found = std::ranges::find(entries_, id, &Entry::id);
    if (found == entries_.end()) {
        return false;
    }
    entries_.erase(found);
    if (hovered_ == id) {
        set_hovered({});
    }
    layout_valid_ = false;
    update();
    return true;
}

bool StatusBar::has_item(const std::string& id) const {
    return item(id) != nullptr;
}

const StatusBarItem* StatusBar::item(const std::string& id) const {
    const auto found = std::ranges::find(entries_, id, &Entry::id);
    return found == entries_.end() ? nullptr : &found->item;
}

std::vector<const StatusBar::Entry*> StatusBar::ordered(StatusBarSide side) const {
    std::vector<const Entry*> result;
    for (const Entry& entry : entries_) {
        if (entry.side == side) {
            result.push_back(&entry);
        }
    }
    std::ranges::stable_sort(
        result, [](const Entry* a, const Entry* b) { return a->item.priority > b->item.priority; });
    return result;
}

std::vector<std::string> StatusBar::item_ids(StatusBarSide side) const {
    std::vector<std::string> ids;
    for (const Entry* entry : ordered(side)) {
        ids.push_back(entry->id);
    }
    return ids;
}

QRect StatusBar::item_rect(const std::string& id) const {
    const auto found = std::ranges::find(entries_, id, &Entry::id);
    if (found == entries_.end() || !shown(*found)) {
        return {};
    }
    ensure_layout();
    return found->rect;
}

std::vector<AccessibleItem> StatusBar::accessible_items() const {
    ensure_layout();
    std::vector<AccessibleItem> items;
    for (const StatusBarSide side : {StatusBarSide::left, StatusBarSide::right}) {
        for (const Entry* entry : ordered(side)) {
            if (!shown(*entry)) {
                continue;
            }
            const bool has_command = !entry->item.command.empty();
            items.push_back({.key = QString::fromStdString(entry->id),
                             .name = entry->item.text,
                             .description = entry->item.tooltip,
                             .role = has_command ? QAccessible::Button : QAccessible::StaticText,
                             .state = {},
                             .rect = entry->rect,
                             .actions = has_command
                                            ? QStringList{QAccessibleActionInterface::pressAction()}
                                            : QStringList()});
        }
    }
    return items;
}

void StatusBar::do_accessible_action(const QString& key, const QString& action) {
    if (action != QAccessibleActionInterface::pressAction()) {
        return;
    }
    const std::string id = key.toStdString();
    const auto found = std::ranges::find(entries_, id, &Entry::id);
    if (found != entries_.end() && shown(*found) && !found->item.command.empty()) {
        emit command_requested(QString::fromStdString(found->item.command));
    }
}

int StatusBar::accessible_index(const std::string& id) const {
    const auto items = accessible_items();
    const auto found = std::ranges::find(items, QString::fromStdString(id), &AccessibleItem::key);
    return found == items.end() ? -1 : static_cast<int>(found - items.begin());
}

bool StatusBar::shown(const Entry& entry) const {
    return message_.isEmpty() || entry.side == StatusBarSide::right;
}

void StatusBar::ensure_layout() const {
    if (layout_valid_) {
        return;
    }
    const QFontMetrics metrics(font());
    const auto width_of = [&](const Entry* entry) {
        return metrics.horizontalAdvance(entry->item.text) + 2 * item_padding;
    };
    int x = item_margin;
    for (const Entry* entry : ordered(StatusBarSide::left)) {
        const int width = width_of(entry);
        entry->rect = QRect(x, 0, width, height_pixels);
        x += width + 2 * item_margin;
    }
    x = width() - item_margin;
    const auto right = ordered(StatusBarSide::right);
    for (auto it = right.rbegin(); it != right.rend(); ++it) {
        const int item_width = width_of(*it);
        x -= item_width;
        (*it)->rect = QRect(x, 0, item_width, height_pixels);
        x -= 2 * item_margin;
    }
    layout_valid_ = true;
}

const StatusBar::Entry* StatusBar::entry_at(const QPoint& position) const {
    ensure_layout();
    for (const Entry& entry : entries_) {
        if (shown(entry) && !entry.item.command.empty() && entry.rect.contains(position)) {
            return &entry;
        }
    }
    return nullptr;
}

QColor StatusBar::resolve(const StatusBarColor& color, std::string_view fallback_key) const {
    if (!color.key.empty()) {
        return colors_.color(color.key);
    }
    if (color.color.isValid()) {
        return color.color;
    }
    return colors_.color(fallback_key);
}

void StatusBar::show_message(const QString& text, int timeout_ms) {
    message_timer_.stop();
    if (timeout_ms > 0) {
        message_timer_.start(timeout_ms);
    }
    if (message_ == text) {
        return;
    }
    message_ = text;
    if (!hovered_.empty() && !text.isEmpty()) {
        set_hovered({});
    }
    update();
}

void StatusBar::clear_message() {
    show_message({});
}

void StatusBar::paintEvent(QPaintEvent*) {
    ensure_layout();
    QPainter painter(this);
    painter.fillRect(rect(), colors_.color("statusBar.background"));
    if (colors_.is_themed("statusBar.border")) {
        painter.fillRect(QRect(0, 0, width(), 1), colors_.color("statusBar.border"));
    }

    const auto draw = [&](const QRect& area, const QString& text, const QColor& foreground) {
        painter.setPen(foreground);
        painter.drawText(area.adjusted(item_padding, 0, -item_padding, 0),
                         Qt::AlignVCenter | Qt::AlignLeft, text);
    };

    if (!message_.isEmpty()) {
        int right_edge = width();
        for (const Entry& entry : entries_) {
            if (entry.side == StatusBarSide::right) {
                right_edge = std::min(right_edge, entry.rect.left() - item_margin);
            }
        }
        painter.save();
        painter.setClipRect(QRect(0, 0, std::max(0, right_edge), height()));
        draw(QRect(item_margin, 0, width(), height_pixels), message_,
             colors_.color("statusBar.foreground"));
        painter.restore();
    }

    for (const Entry& entry : entries_) {
        if (!shown(entry)) {
            continue;
        }
        const QRect& area = entry.rect;
        if (!entry.item.background.key.empty() || entry.item.background.color.isValid()) {
            painter.fillRect(area, resolve(entry.item.background, "statusBar.background"));
        }
        if (!hovered_.empty() && hovered_ == entry.id) {
            painter.fillRect(area,
                             colors_.color(pressed_ == entry.id ? "statusBarItem.activeBackground"
                                                                : "statusBarItem.hoverBackground"));
        }
        draw(area, entry.item.text, resolve(entry.item.foreground, "statusBar.foreground"));
    }
}

void StatusBar::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    layout_valid_ = false;
}

void StatusBar::set_hovered(const std::string& id) {
    if (hovered_ == id) {
        return;
    }
    hovered_ = id;
    setCursor(id.empty() ? QCursor() : QCursor(Qt::PointingHandCursor));
    update();
}

void StatusBar::mouseMoveEvent(QMouseEvent* event) {
    const Entry* entry = entry_at(event->position().toPoint());
    set_hovered(entry ? entry->id : std::string());
}

void StatusBar::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        return;
    }
    const Entry* entry = entry_at(event->position().toPoint());
    pressed_ = entry ? entry->id : std::string();
    update();
}

void StatusBar::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        return;
    }
    const std::string pressed = std::exchange(pressed_, {});
    update();
    const Entry* entry = entry_at(event->position().toPoint());
    if (entry && entry->id == pressed) {
        emit command_requested(QString::fromStdString(entry->item.command));
    }
}

void StatusBar::leaveEvent(QEvent* event) {
    QWidget::leaveEvent(event);
    set_hovered({});
}

bool StatusBar::event(QEvent* event) {
    if (event->type() == QEvent::ToolTip) {
        const auto* help = static_cast<QHelpEvent*>(event);
        ensure_layout();
        for (const Entry& entry : entries_) {
            if (shown(entry) && !entry.item.tooltip.isEmpty() && entry.rect.contains(help->pos())) {
                QToolTip::showText(help->globalPos(), entry.item.tooltip, this, entry.rect);
                return true;
            }
        }
        QToolTip::hideText();
        event->ignore();
        return true;
    }
    return QWidget::event(event);
}

} // namespace anvil::ui
