#pragma once

#include "anvil/workbench/state_store.hpp"

#include <QObject>
#include <QRect>
#include <QTimer>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class QWidget;

namespace anvil::ui {

class Panel;
class Sidebar;
class Workbench;

// Where a window that was saved at `saved` goes on screens with the available areas `screens`, the
// primary one first. A window with enough of its top on a screen to be taken hold of stays; another
// one is centered on the screen that shows most of it, or the primary one, and made no larger than
// that screen.
QRect fit_to_screens(const QRect& saved, const std::vector<QRect>& screens);

// Keeps the layout of a window, and its size and position, in the state store (ADR 0021): the
// visibility and sizes of the sidebar and the panel, the position of the panel, whether it is
// maximized and the active view containers. What the views show is added by a capture of the
// window's own.
class WindowStateKeeper : public QObject {
    Q_OBJECT

public:
    // How long after the last change the state is written.
    static constexpr std::chrono::milliseconds write_delay{500};

    struct PartsShown {
        bool sidebar = false;
        bool panel = false;
    };

    // All the arguments must outlive this object.
    WindowStateKeeper(workbench::StateStore& store, QWidget& window, Workbench& workbench,
                      Sidebar& sidebar, Panel& panel, QObject* parent = nullptr);

    // While `outside_mode` returns a value, it is saved in place of what shows, so that a mode that
    // hides the parts for a while, such as Zen Mode, is not what the next window starts with.
    void set_parts_outside_mode(std::function<std::optional<PartsShown>()> outside_mode);

    // Called before each write, to put into the store what is not the layout.
    void set_content_capture(std::function<void(workbench::StateStore&)> capture);
    // Called by `set_workspace` once the store is on the new workspace and before its layout is
    // applied, to show what is saved there of the content.
    void set_content_restore(std::function<void()> restore);
    // Writes after the delay, as after a change of the layout.
    void schedule();

    // Applies the saved state, which has to happen before the window is first shown, and saves
    // changes from then on. With no saved visibility, the sidebar shows for a folder and is hidden
    // without one, and the panel is hidden.
    void restore();
    // Writes what the window shows.
    void save();
    // Writes what the window shows to the workspace that is left and applies the layout of the new
    // one: the visibility of the parts, the position of the panel, whether it is maximized and the
    // active containers, each with its default where the workspace has none saved. The sizes stay.
    void set_workspace(const std::optional<std::string>& workspace_id);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void restore_geometry();
    void restore_sizes();
    // The layout that belongs to the workspace.
    void restore_layout();
    void watch();
    void capture();

    workbench::StateStore& store_;
    QWidget& window_;
    Workbench& workbench_;
    Sidebar& sidebar_;
    Panel& panel_;
    QTimer timer_;
    std::function<std::optional<PartsShown>()> parts_outside_mode_;
    std::function<void(workbench::StateStore&)> content_capture_;
    std::function<void()> content_restore_;
};

} // namespace anvil::ui
