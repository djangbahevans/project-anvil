#include "anvil/ui/tab_strip.hpp"

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QFontMetrics>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>
#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace anvil::ui {

namespace {

constexpr int dot_diameter = 8;
constexpr int close_cross_half_size = 4;
constexpr int hover_radius = 5;
constexpr double cross_pen_width = 1.2;

std::uint64_t next_tab_id() {
    static std::uint64_t counter = 0;
    return ++counter;
}

} // namespace

TabStrip::TabStrip(const WorkbenchColors& colors, QWidget* parent)
    : QWidget(parent), colors_(colors) {
    setFixedHeight(strip_height());
    setMouseTracking(true);
    // Before the name is set, which makes Qt look for the interface of the widget and keep it.
    install_painted_accessibility();
    setAccessibleName(tr("Tabs"));
    QFont strip_font = font();
    strip_font.setPixelSize(zoomed(font_pixels));
    setFont(strip_font);
    connect(&colors_, &WorkbenchColors::changed, this, qOverload<>(&TabStrip::update));
}

TabStrip::~TabStrip() = default;

void TabStrip::set_zoom(const Zoom* zoom) {
    if (zoom_) {
        disconnect(zoom_, &Zoom::changed, this, nullptr);
    }
    zoom_ = zoom;
    if (zoom_) {
        connect(zoom_, &Zoom::changed, this, &TabStrip::apply_zoom);
    }
    apply_zoom();
}

void TabStrip::apply_zoom() {
    setFixedHeight(strip_height());
    QFont strip_font = font();
    strip_font.setPixelSize(zoomed(font_pixels));
    setFont(strip_font);
    // A font that changed measures the titles again in `changeEvent`; the sizes that are not the
    // font's still change the layout.
    layout_valid_ = false;
    set_scroll(scroll_);
    if (current_ >= 0) {
        scroll_into_view(current_);
    }
    update();
}

int TabStrip::strip_height() const {
    return zoomed(height_pixels);
}

int TabStrip::tab_min_width() const {
    return zoomed(minimum_tab_width);
}

int TabStrip::count() const {
    return static_cast<int>(entries_.size());
}

const TabStrip::Tab& TabStrip::tab(int index) const {
    return entries_.at(static_cast<std::size_t>(index)).tab;
}

void TabStrip::measure(Entry& entry) const {
    entry.title_width = fontMetrics().horizontalAdvance(entry.tab.title);
    entry.description_width =
        entry.tab.description.isEmpty()
            ? 0
            : zoomed(description_gap) +
                  QFontMetrics(description_font()).horizontalAdvance(entry.tab.description);
}

QFont TabStrip::description_font() const {
    QFont smaller = font();
    // 0.9 em, as VS Code writes a description.
    const int description_pixels = zoomed(font_pixels) * 9 / 10;
    smaller.setPixelSize(description_pixels);
    return smaller;
}

int TabStrip::add_tab(Tab tab) {
    Entry entry{.tab = std::move(tab), .id = next_tab_id()};
    measure(entry);
    entries_.push_back(std::move(entry));
    if (current_ < 0) {
        current_ = 0;
    }
    layout_valid_ = false;
    update();
    return count() - 1;
}

void TabStrip::remove_tab(int index) {
    if (index < 0 || index >= count()) {
        return;
    }
    entries_.erase(entries_.begin() + index);
    if (index < current_) {
        --current_;
    } else if (index == current_) {
        current_ = std::min(current_, count() - 1);
    }
    if (hovered_ >= count() || index == hovered_) {
        hovered_ = -1;
        hovered_button_ = false;
    } else if (index < hovered_) {
        --hovered_;
    }
    pressed_ = -1;
    dragging_ = false;
    layout_valid_ = false;
    set_scroll(scroll_);
    update();
}

void TabStrip::set_tab(int index, Tab tab) {
    Entry& entry = entries_.at(static_cast<std::size_t>(index));
    if (entry.tab == tab) {
        return;
    }
    const bool retitled = entry.tab.title != tab.title;
    const bool redescribed = entry.tab.description != tab.description;
    const bool iconed = entry.tab.icon.has_value() != tab.icon.has_value();
    const bool visible_in_strip = !single_ || index == current_;
    entry.tab = std::move(tab);
    if (retitled || redescribed) {
        measure(entry);
    }
    if (retitled || redescribed || iconed) {
        layout_valid_ = false;
    }
    update();
    if (retitled && visible_in_strip) {
        notify_accessible_item(this, QAccessible::NameChanged, accessible_index(index));
    }
}

int TabStrip::accessible_index(int tab) const {
    if (!single_) {
        return tab;
    }
    return tab == current_ ? 0 : -1;
}

void TabStrip::set_single(bool single) {
    if (single == single_) {
        return;
    }
    single_ = single;
    layout_valid_ = false;
    scroll_ = 0;
    hovered_ = -1;
    hovered_button_ = false;
    update();
}

void TabStrip::move_tab(int from, int to) {
    if (from == to || from < 0 || to < 0 || from >= count() || to >= count()) {
        return;
    }
    const auto first = entries_.begin();
    if (from < to) {
        std::rotate(first + from, first + from + 1, first + to + 1);
    } else {
        std::rotate(first + to, first + from, first + from + 1);
    }
    const auto follow = [&](int& index) {
        if (index == from) {
            index = to;
        } else if (from < to && index > from && index <= to) {
            --index;
        } else if (from > to && index >= to && index < from) {
            ++index;
        }
    };
    follow(current_);
    follow(hovered_);
    pressed_ = -1;
    dragging_ = false;
    layout_valid_ = false;
    update();
}

void TabStrip::set_current_index(int index) {
    if (index < 0 || index >= count()) {
        return;
    }
    if (index != current_) {
        const int previous = current_;
        const int previous_item = accessible_index(previous);
        current_ = index;
        layout_valid_ = layout_valid_ && !single_;
        update();
        if (accessible_events_wanted()) {
            QAccessible::State selected;
            selected.selected = true;
            if (previous >= 0 && previous_item >= 0) {
                notify_accessible_item_state(this, selected, previous_item);
            }
            notify_accessible_item_state(this, selected, accessible_index(index));
        }
    }
    scroll_into_view(index);
}

std::vector<AccessibleItem> TabStrip::accessible_items() const {
    ensure_layout();
    std::vector<AccessibleItem> items;
    items.reserve(entries_.size());
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (single_ && static_cast<int>(i) != current_) {
            continue;
        }
        const Entry& entry = entries_[i];
        QAccessible::State state;
        state.selectable = true;
        state.selected = static_cast<int>(i) == current_;
        items.push_back(
            {.key = QString::number(entry.id),
             .name = entry.tab.title,
             .description = entry.tab.dirty && !entry.tab.tooltip.isEmpty()
                                ? entry.tab.tooltip + QStringLiteral("\n") + tr("Unsaved changes")
                                : (entry.tab.dirty ? tr("Unsaved changes") : entry.tab.tooltip),
             .role = QAccessible::PageTab,
             .state = state,
             .rect = tab_rect(static_cast<int>(i)),
             .actions = {QAccessibleActionInterface::pressAction(), close_action()}});
    }
    return items;
}

void TabStrip::do_accessible_action(const QString& key, const QString& action) {
    const std::uint64_t id = key.toULongLong();
    const auto found = std::ranges::find(entries_, id, &Entry::id);
    if (found == entries_.end()) {
        return;
    }
    const int index = static_cast<int>(found - entries_.begin());
    if (action == close_action()) {
        emit close_requested(index);
    } else if (action == QAccessibleActionInterface::pressAction() && index != current_) {
        set_current_index(index);
        emit current_changed(index);
    }
}

void TabStrip::ensure_layout() const {
    if (layout_valid_) {
        return;
    }
    int x = 0;
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        Entry& entry = entries_[i];
        const int natural = std::max(
            tab_min_width(), zoomed(title_padding) + icon_column(entry.tab) + entry.title_width +
                                 entry.description_width + 1 + zoomed(action_width));
        if (single_) {
            entry.x = 0;
            entry.width = static_cast<int>(i) == current_ ? std::max(natural, width()) : 0;
            continue;
        }
        entry.width = natural;
        entry.x = x;
        x += entry.width;
    }
    layout_valid_ = true;
}

int TabStrip::content_width() const {
    ensure_layout();
    if (single_) {
        return 0;
    }
    return entries_.empty() ? 0 : entries_.back().x + entries_.back().width;
}

int TabStrip::max_scroll() const {
    return std::max(0, content_width() - width());
}

void TabStrip::set_scroll(int pixels) {
    const int clamped = std::clamp(pixels, 0, max_scroll());
    if (clamped != scroll_) {
        scroll_ = clamped;
        update();
    }
}

void TabStrip::scroll_into_view(int index) {
    ensure_layout();
    const Entry& entry = entries_.at(static_cast<std::size_t>(index));
    if (entry.x < scroll_) {
        set_scroll(entry.x);
    } else if (entry.x + entry.width > scroll_ + width()) {
        set_scroll(entry.x + entry.width - width());
    }
}

QRect TabStrip::tab_rect(int index) const {
    if (index < 0 || index >= count()) {
        return {};
    }
    ensure_layout();
    const Entry& entry = entries_[static_cast<std::size_t>(index)];
    return QRect(entry.x - scroll_, 0, entry.width, strip_height());
}

QRect TabStrip::close_button_rect(int index) const {
    const QRect area = tab_rect(index);
    if (area.isEmpty()) {
        return {};
    }
    const int action = zoomed(action_width);
    const int size = zoomed(close_button_size);
    const int left = area.right() + 1 - action + (action - size) / 2;
    return QRect(left, (strip_height() - size) / 2, size, size);
}

int TabStrip::tab_at(int x) const {
    ensure_layout();
    if (single_) {
        const bool on_tab =
            current_ >= 0 && x >= 0 && x < entries_[static_cast<std::size_t>(current_)].width;
        return on_tab ? current_ : -1;
    }
    const int content_x = x + scroll_;
    const auto found = std::ranges::upper_bound(
        entries_, content_x, {}, [](const Entry& entry) { return entry.x + entry.width; });
    if (found == entries_.end() || content_x < found->x || content_x < 0) {
        return -1;
    }
    return static_cast<int>(found - entries_.begin());
}

int TabStrip::insertion_index_at(int x) const {
    ensure_layout();
    const int content_x = x + scroll_;
    for (int i = 0; i < count(); ++i) {
        const Entry& entry = entries_[static_cast<std::size_t>(i)];
        if (content_x < entry.x + entry.width / 2) {
            return i;
        }
    }
    return count();
}

void TabStrip::set_drop_indicator(int position) {
    const int clamped = position < 0 ? -1 : std::min(position, count());
    if (drop_indicator_ != clamped) {
        drop_indicator_ = clamped;
        update();
    }
}

void TabStrip::set_focused(bool focused) {
    if (focused_ != focused) {
        focused_ = focused;
        update();
    }
}

void TabStrip::set_hovered(int tab, bool button) {
    if (hovered_ == tab && hovered_button_ == button) {
        return;
    }
    hovered_ = tab;
    hovered_button_ = button;
    update();
}

void TabStrip::paint_tab(QPainter& painter, int index, int x) const {
    const Entry& entry = entries_[static_cast<std::size_t>(index)];
    const bool active = index == current_;
    const bool hovered = index == hovered_;
    const QRect area(x, 0, entry.width, strip_height());

    const bool hover_fill = hovered && !active && colors_.is_themed("tab.hoverBackground");
    const char* background =
        active       ? (focused_ ? "tab.activeBackground" : "tab.unfocusedActiveBackground")
        : hover_fill ? "tab.hoverBackground"
        : focused_   ? "tab.inactiveBackground"
                     : "tab.unfocusedInactiveBackground";
    painter.fillRect(area, colors_.color(background));
    if (active) {
        const char* top = focused_ ? "tab.activeBorderTop" : "tab.unfocusedActiveBorderTop";
        if (focused_ || colors_.is_themed(top)) {
            painter.fillRect(QRect(area.left(), 0, area.width(), 1), colors_.color(top));
        }
        if (colors_.is_themed("tab.activeBorder")) {
            painter.fillRect(QRect(area.left(), strip_height() - 1, area.width(), 1),
                             colors_.color("tab.activeBorder"));
        }
    }
    painter.fillRect(QRect(area.right(), 0, 1, strip_height()), colors_.color("tab.border"));

    const char* foreground_key =
        active     ? (focused_ ? "tab.activeForeground" : "tab.unfocusedActiveForeground")
        : focused_ ? "tab.inactiveForeground"
                   : "tab.unfocusedInactiveForeground";
    const QColor foreground = colors_.color(foreground_key);
    const int title_left = area.left() + zoomed(title_padding);
    if (const QString family = seti_font_family(); entry.tab.icon && !family.isEmpty()) {
        const SetiGlyph glyph =
            colors_.kind() == core::ThemeKind::light ? entry.tab.icon->light : entry.tab.icon->dark;
        painter.save();
        QFont icon_font(family);
        const int icon_pixels = zoomed(font_pixels) * seti_font_percent / 100;
        icon_font.setPixelSize(icon_pixels);
        painter.setFont(icon_font);
        painter.setPen(QColor::fromRgba(glyph.color));
        // Wider than the column, so that a glyph that fills its square is not cut.
        const int width = zoomed(icon_width);
        painter.drawText(QRect(title_left - width / 2, 0, 2 * width, strip_height()),
                         Qt::AlignCenter, QString(QChar(glyph.code)));
        painter.restore();
    }
    painter.setPen(foreground);
    painter.drawText(
        QRect(title_left + icon_column(entry.tab), 0, entry.title_width + 2, strip_height()),
        Qt::AlignVCenter | Qt::AlignLeft, entry.tab.title);
    if (!entry.tab.description.isEmpty()) {
        painter.save();
        painter.setFont(description_font());
        QColor dimmed = foreground;
        dimmed.setAlphaF(dimmed.alphaF() * 0.7F);
        painter.setPen(dimmed);
        painter.drawText(
            QRect(title_left + icon_column(entry.tab) + entry.title_width + zoomed(description_gap),
                  0, entry.description_width, strip_height()),
            Qt::AlignVCenter | Qt::AlignLeft, entry.tab.description);
        painter.restore();
    }

    const bool on_button = hovered && hovered_button_;
    const bool dot = entry.tab.dirty && !on_button;
    if (!dot && !active && !hovered) {
        return;
    }
    const QRect button =
        close_button_rect(index).translated(area.left() - tab_rect(index).left(), 0);
    painter.setRenderHint(QPainter::Antialiasing);
    if (on_button) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(colors_.color("toolbar.hoverBackground"));
        painter.drawRoundedRect(button, zoomed(hover_radius), zoomed(hover_radius));
    }
    const QPointF center = QRectF(button).center();
    if (dot) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(foreground);
        painter.drawEllipse(center, zoomed(dot_diameter) / 2.0, zoomed(dot_diameter) / 2.0);
    } else {
        const double half = zoomed(close_cross_half_size);
        painter.setPen(QPen(foreground, cross_pen_width * (zoom_ ? zoom_->factor() : 1.0)));
        painter.drawLine(center + QPointF(-half, -half), center + QPointF(half, half));
        painter.drawLine(center + QPointF(-half, half), center + QPointF(half, -half));
    }
}

void TabStrip::paintEvent(QPaintEvent*) {
    ensure_layout();
    QPainter painter(this);
    painter.fillRect(rect(), colors_.color("editorGroupHeader.tabsBackground"));
    if (colors_.is_themed("editorGroupHeader.tabsBorder")) {
        painter.fillRect(QRect(0, strip_height() - 1, width(), 1),
                         colors_.color("editorGroupHeader.tabsBorder"));
    }

    const int dragged = dragging_ && !dragging_out_ ? pressed_ : -1;
    for (int i = 0; i < count(); ++i) {
        const QRect area = tab_rect(i);
        if (i == dragged || area.right() < 0 || area.left() > width()) {
            continue;
        }
        // A tab that is dragged out stays where it is, dimmed, until it is dropped.
        painter.setOpacity(dragging_out_ && i == pressed_ ? 0.5 : 1.0);
        paint_tab(painter, i, area.left());
        painter.setOpacity(1.0);
    }
    if (dragged >= 0) {
        paint_tab(painter, dragged, drag_left_);
    }
    if (drop_indicator_ >= 0) {
        const int x = single_                     ? (drop_indicator_ <= current_ ? 0 : width())
                      : drop_indicator_ < count() ? tab_rect(drop_indicator_).left()
                                                  : tab_rect(count() - 1).right() + 1;
        painter.fillRect(QRect(std::max(0, x - 1), 0, 2, strip_height()),
                         colors_.color("tab.dragAndDropBorder"));
    }
}

void TabStrip::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    layout_valid_ = layout_valid_ && !single_;
    set_scroll(scroll_);
    if (current_ >= 0) {
        scroll_into_view(current_);
    }
}

void TabStrip::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange) {
        for (Entry& entry : entries_) {
            measure(entry);
        }
        layout_valid_ = false;
        update();
    }
}

void TabStrip::drag_to(int x) {
    ensure_layout();
    const int dragged = pressed_;
    const int dragged_width = entries_[static_cast<std::size_t>(dragged)].width;
    drag_left_ = std::clamp(x - grab_offset_, 0, std::max(0, width() - dragged_width));
    if (single_) {
        update();
        return;
    }

    // The tab changes places with a neighbor when the mouse passes the middle of that neighbor.
    const int pointer = x + scroll_;
    const auto middle_of = [&](int index) {
        const Entry& entry = entries_[static_cast<std::size_t>(index)];
        return entry.x + entry.width / 2;
    };
    int at = dragged;
    for (;;) {
        ensure_layout();
        int target = at;
        if (at + 1 < count() && pointer > middle_of(at + 1)) {
            target = at + 1;
        } else if (at > 0 && pointer < middle_of(at - 1)) {
            target = at - 1;
        }
        if (target == at) {
            break;
        }
        move_tab(at, target);
        emit tab_moved(at, target);
        at = target;
        pressed_ = at;
        dragging_ = true;
    }
    update();
}

void TabStrip::mouseMoveEvent(QMouseEvent* event) {
    const QPoint position = event->position().toPoint();
    const int over = tab_at(position.x());
    set_hovered(over, over >= 0 && close_button_rect(over).contains(position));

    if (pressed_ >= 0 && !pressed_button_ && pressed_with_ == Qt::LeftButton &&
        (event->buttons() & Qt::LeftButton)) {
        if (!dragging_ &&
            (position - press_).manhattanLength() >= QApplication::startDragDistance()) {
            dragging_ = true;
        }
        if (!dragging_) {
            return;
        }
        if (!rect().contains(position)) {
            if (!dragging_out_) {
                dragging_out_ = true;
                grabKeyboard();
                update();
            }
            emit drag_out_moved(pressed_, event->globalPosition().toPoint());
            return;
        }
        if (dragging_out_) {
            end_drag_out();
            emit drag_out_cancelled();
        }
        drag_to(position.x());
    }
}

void TabStrip::end_drag_out() {
    dragging_out_ = false;
    releaseKeyboard();
    update();
}

void TabStrip::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape && dragging_out_) {
        pressed_ = -1;
        pressed_with_ = Qt::NoButton;
        dragging_ = false;
        end_drag_out();
        emit drag_out_cancelled();
        return;
    }
    QWidget::keyPressEvent(event);
}

void TabStrip::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && tab_at(event->position().toPoint().x()) < 0) {
        emit new_file_requested();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void TabStrip::mousePressEvent(QMouseEvent* event) {
    const QPoint position = event->position().toPoint();
    const int over = tab_at(position.x());
    pressed_ = over;
    pressed_with_ = event->button();
    pressed_button_ = false;
    dragging_ = false;
    if (over < 0 || (event->button() != Qt::LeftButton && event->button() != Qt::MiddleButton)) {
        pressed_ = -1;
        return;
    }
    if (event->button() == Qt::MiddleButton) {
        return;
    }
    if (close_button_rect(over).contains(position)) {
        pressed_button_ = true;
        return;
    }
    press_ = position;
    grab_offset_ = position.x() - tab_rect(over).left();
    scroll_into_view(over);
    if (over != current_) {
        current_ = over;
        update();
        emit current_changed(over);
    }
}

void TabStrip::mouseReleaseEvent(QMouseEvent* event) {
    const QPoint position = event->position().toPoint();
    const int target = pressed_;
    const bool on_button = pressed_button_;
    const bool was_dragging = dragging_;
    const bool was_out = dragging_out_;
    const Qt::MouseButton button = pressed_with_;
    pressed_ = -1;
    pressed_button_ = false;
    dragging_ = false;
    pressed_with_ = Qt::NoButton;
    if (was_out) {
        end_drag_out();
        emit drag_out_dropped(target, event->globalPosition().toPoint());
        return;
    }
    if (was_dragging) {
        update();
        return;
    }
    if (target < 0 || event->button() != button || tab_at(position.x()) != target) {
        return;
    }
    if (button == Qt::MiddleButton ||
        (button == Qt::LeftButton && on_button && close_button_rect(target).contains(position))) {
        emit close_requested(target);
    }
}

void TabStrip::wheelEvent(QWheelEvent* event) {
    const QPoint delta = event->pixelDelta().isNull() ? event->angleDelta() : event->pixelDelta();
    const int amount = delta.x() != 0 ? delta.x() : delta.y();
    if (amount == 0 || max_scroll() == 0) {
        event->ignore();
        return;
    }
    set_scroll(scroll_ - amount);
    event->accept();
}

void TabStrip::leaveEvent(QEvent* event) {
    QWidget::leaveEvent(event);
    set_hovered(-1, false);
}

std::unique_ptr<QMenu> TabStrip::build_context_menu(int index) {
    auto menu = std::make_unique<QMenu>(this);
    if (index < 0 || index >= count()) {
        return menu;
    }
    const Tab& subject = entries_[static_cast<std::size_t>(index)].tab;
    const std::uint64_t id = entries_[static_cast<std::size_t>(index)].id;

    // The menu can stay open while extensions change the tabs, so an action finds its tab again
    // by identity and does nothing when the tab is gone.
    const auto act = [this, id, &menu](const QString& text, bool enabled, auto emit_signal) {
        QAction* action = menu->addAction(text);
        action->setEnabled(enabled);
        connect(action, &QAction::triggered, this, [this, id, emit_signal] {
            const auto found = std::ranges::find(entries_, id, &Entry::id);
            if (found != entries_.end()) {
                emit_signal(static_cast<int>(found - entries_.begin()));
            }
        });
    };
    act(tr("Close"), true, [this](int i) { emit close_requested(i); });
    act(tr("Close Others"), count() > 1, [this](int i) { emit close_others_requested(i); });
    act(tr("Close to the Right"), index + 1 < count(),
        [this](int i) { emit close_to_the_right_requested(i); });
    act(tr("Close Saved"), true, [this](int) { emit close_saved_requested(); });
    menu->addSeparator();
    act(tr("Copy Path"), subject.has_path, [this](int i) { emit copy_path_requested(i); });
    return menu;
}

void TabStrip::contextMenuEvent(QContextMenuEvent* event) {
    const int over = tab_at(event->pos().x());
    if (over < 0) {
        return;
    }
    build_context_menu(over)->exec(event->globalPos());
}

bool TabStrip::event(QEvent* event) {
    if (event->type() == QEvent::ToolTip) {
        const auto* help = static_cast<QHelpEvent*>(event);
        const int over = tab_at(help->pos().x());
        if (over >= 0 && !entries_[static_cast<std::size_t>(over)].tab.tooltip.isEmpty()) {
            QToolTip::showText(help->globalPos(),
                               entries_[static_cast<std::size_t>(over)].tab.tooltip, this,
                               tab_rect(over));
        } else {
            QToolTip::hideText();
            event->ignore();
        }
        return true;
    }
    return QWidget::event(event);
}

} // namespace anvil::ui
