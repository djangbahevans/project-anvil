#include "anvil/ui/window_state_keeper.hpp"

#include "anvil/ui/panel.hpp"
#include "anvil/ui/sidebar.hpp"
#include "anvil/ui/workbench.hpp"

#include <QEvent>
#include <QGuiApplication>
#include <QScreen>
#include <QWidget>
#include <algorithm>
#include <array>
#include <cstdint>

namespace anvil::ui {
namespace {

using workbench::StateScope;

constexpr const char* sidebar_visible_key = "layout.sidebar.visible";
constexpr const char* sidebar_container_key = "layout.sidebar.activeContainer";
constexpr const char* sidebar_width_key = "layout.sidebar.width";
constexpr const char* panel_visible_key = "layout.panel.visible";
constexpr const char* panel_container_key = "layout.panel.activeContainer";
constexpr const char* panel_position_key = "layout.panel.position";
constexpr const char* panel_maximized_key = "layout.panel.maximized";
constexpr const char* window_x_key = "window.x";
constexpr const char* window_y_key = "window.y";
constexpr const char* window_width_key = "window.width";
constexpr const char* window_height_key = "window.height";
constexpr const char* window_maximized_key = "window.maximized";

constexpr std::array panel_positions{PanelPosition::top, PanelPosition::left, PanelPosition::right,
                                     PanelPosition::bottom};

// How much of the strip along the top of a window a screen has to show for the user to be able to
// take hold of the window.
constexpr int reachable_strip_height = 35;
constexpr int reachable_width = 100;
constexpr int reachable_height = 10;

// A saved number beyond this is damage rather than the place or size of a window or a part.
constexpr std::int64_t largest_saved = 100'000;

std::string panel_size_key(PanelPosition position) {
    return "layout.panel.size." + std::string(panel_position_name(position));
}

std::optional<int> within(std::optional<std::int64_t> value, std::int64_t least) {
    if (!value || *value < least || *value > largest_saved) {
        return std::nullopt;
    }
    return static_cast<int>(*value);
}

} // namespace

QRect fit_to_screens(const QRect& saved, const std::vector<QRect>& screens) {
    if (screens.empty()) {
        return saved;
    }
    const QRect strip(saved.left(), saved.top(), saved.width(),
                      std::min(saved.height(), reachable_strip_height));
    const QRect* target = &screens.front();
    qint64 most = 0;
    for (const QRect& screen : screens) {
        const QRect shown = screen.intersected(strip);
        if (shown.width() >= std::min(reachable_width, strip.width()) &&
            shown.height() >= std::min(reachable_height, strip.height())) {
            return saved;
        }
        const QRect overlap = screen.intersected(saved);
        const qint64 area = qint64{overlap.width()} * overlap.height();
        if (area > most) {
            most = area;
            target = &screen;
        }
    }
    QRect moved(QPoint(), saved.size().boundedTo(target->size()));
    moved.moveCenter(target->center());
    return moved;
}

WindowStateKeeper::WindowStateKeeper(workbench::StateStore& store, QWidget& window,
                                     Workbench& workbench, Sidebar& sidebar, Panel& panel,
                                     QObject* parent)
    : QObject(parent), store_(store), window_(window), workbench_(workbench), sidebar_(sidebar),
      panel_(panel) {
    timer_.setSingleShot(true);
    timer_.setInterval(write_delay);
    connect(&timer_, &QTimer::timeout, this, &WindowStateKeeper::save);
}

void WindowStateKeeper::set_parts_outside_mode(
    std::function<std::optional<PartsShown>()> outside_mode) {
    parts_outside_mode_ = std::move(outside_mode);
}

void WindowStateKeeper::set_content_capture(std::function<void(workbench::StateStore&)> capture) {
    content_capture_ = std::move(capture);
}

void WindowStateKeeper::schedule() {
    timer_.start();
}

void WindowStateKeeper::restore() {
    // The window first, so that the parts are laid out in the size they were saved in.
    restore_geometry();
    restore_layout();
    watch();
}

void WindowStateKeeper::restore_geometry() {
    const auto x = within(store_.integer(StateScope::global, window_x_key), -largest_saved);
    const auto y = within(store_.integer(StateScope::global, window_y_key), -largest_saved);
    const auto width = within(store_.integer(StateScope::global, window_width_key), 1);
    const auto height = within(store_.integer(StateScope::global, window_height_key), 1);
    if (x && y && width && height) {
        std::vector<QRect> screens;
        for (const QScreen* screen : QGuiApplication::screens()) {
            screens.push_back(screen->availableGeometry());
        }
        window_.setGeometry(fit_to_screens(QRect(*x, *y, *width, *height), screens));
    }
    if (store_.boolean(StateScope::global, window_maximized_key).value_or(false)) {
        window_.setWindowState(window_.windowState() | Qt::WindowMaximized);
    }
}

void WindowStateKeeper::restore_layout() {
    if (const auto width = within(store_.integer(StateScope::global, sidebar_width_key), 1)) {
        workbench_.set_size(Part::sidebar, *width);
    }
    for (const PanelPosition position : panel_positions) {
        if (const auto size =
                within(store_.integer(StateScope::global, panel_size_key(position)), 1)) {
            workbench_.set_panel_size_at(position, *size);
        }
    }
    if (const auto name = store_.string(StateScope::workspace, panel_position_key)) {
        for (const PanelPosition position : panel_positions) {
            if (panel_position_name(position) == *name) {
                workbench_.set_panel_position(position);
            }
        }
    }
    // The containers before the visibility, so that a part that shows has the right one.
    if (const auto id = store_.string(StateScope::workspace, sidebar_container_key)) {
        sidebar_.set_active_container(*id);
    }
    if (const auto id = store_.string(StateScope::workspace, panel_container_key)) {
        panel_.set_active_container(*id);
    }
    // As in VS Code, a folder shows the sidebar until it is hidden there.
    workbench_.set_visible(Part::sidebar, store_.boolean(StateScope::workspace, sidebar_visible_key)
                                              .value_or(store_.has_workspace()));
    if (const auto shown = store_.boolean(StateScope::workspace, panel_visible_key)) {
        workbench_.set_visible(Part::panel, *shown);
    }
    if (workbench_.is_visible(Part::panel) &&
        store_.boolean(StateScope::workspace, panel_maximized_key).value_or(false)) {
        workbench_.set_maximized(Part::panel, true);
    }
}

void WindowStateKeeper::watch() {
    const auto changed = [this] { schedule(); };
    connect(&workbench_, &Workbench::visibility_changed, this, changed);
    connect(&workbench_, &Workbench::maximized_changed, this, changed);
    connect(&workbench_, &Workbench::panel_position_changed, this, changed);
    connect(&workbench_, &Workbench::sizes_changed, this, changed);
    connect(&sidebar_, &Sidebar::active_container_changed, this, changed);
    connect(&panel_, &Panel::active_container_changed, this, changed);
    window_.installEventFilter(this);
}

bool WindowStateKeeper::eventFilter(QObject* watched, QEvent* event) {
    if (watched == &window_ && (event->type() == QEvent::Move || event->type() == QEvent::Resize ||
                                event->type() == QEvent::WindowStateChange)) {
        schedule();
    }
    return QObject::eventFilter(watched, event);
}

void WindowStateKeeper::capture() {
    const std::optional<PartsShown> outside =
        parts_outside_mode_ ? parts_outside_mode_() : std::nullopt;
    store_.set(StateScope::workspace, sidebar_visible_key,
               outside ? outside->sidebar : workbench_.is_visible(Part::sidebar));
    store_.set(StateScope::workspace, panel_visible_key,
               outside ? outside->panel : workbench_.is_visible(Part::panel));
    store_.set(StateScope::workspace, panel_position_key,
               std::string(panel_position_name(workbench_.panel_position())));
    store_.set(StateScope::workspace, panel_maximized_key, workbench_.is_maximized(Part::panel));
    if (const std::string& id = sidebar_.active_container(); !id.empty()) {
        store_.set(StateScope::workspace, sidebar_container_key, id);
    }
    if (const std::string& id = panel_.active_container(); !id.empty()) {
        store_.set(StateScope::workspace, panel_container_key, id);
    }

    store_.set(StateScope::global, sidebar_width_key, workbench_.size(Part::sidebar));
    for (const PanelPosition position : panel_positions) {
        store_.set(StateScope::global, panel_size_key(position),
                   workbench_.panel_size_at(position));
    }

    // Full screen is not restored: the window keeps the place it has outside of it.
    const Qt::WindowStates states = window_.windowState();
    const QRect place = states & (Qt::WindowMaximized | Qt::WindowFullScreen)
                            ? window_.normalGeometry()
                            : window_.geometry();
    if (!place.isEmpty()) {
        store_.set(StateScope::global, window_x_key, place.x());
        store_.set(StateScope::global, window_y_key, place.y());
        store_.set(StateScope::global, window_width_key, place.width());
        store_.set(StateScope::global, window_height_key, place.height());
    }
    store_.set(StateScope::global, window_maximized_key, states.testFlag(Qt::WindowMaximized));

    if (content_capture_) {
        content_capture_(store_);
    }
}

void WindowStateKeeper::save() {
    timer_.stop();
    capture();
    // The store logs what it cannot write, and keeps it for the next time.
    static_cast<void>(store_.flush());
}

void WindowStateKeeper::set_workspace(const std::optional<std::string>& workspace_id) {
    save();
    store_.set_workspace(workspace_id);
}

} // namespace anvil::ui
