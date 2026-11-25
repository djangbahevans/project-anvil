#pragma once

#include "anvil/ui/workbench_colors.hpp"
#include "anvil/ui/zoom.hpp"

#include <QPointer>
#include <QWidget>
#include <array>
#include <optional>
#include <string_view>

class QHBoxLayout;
class QVBoxLayout;

namespace anvil::ui {

class Splitter;

// The fixed parts of the window, in VS Code's positions and with its part ids: the title bar above
// everything, the activity bar at the left edge, then the sidebar, then the editor area above the
// panel, and the status bar below everything (ADR 0014).
enum class Part {
    title_bar,
    activity_bar,
    sidebar,
    editor,
    panel,
    status_bar,
};

// `workbench.parts.sidebar` and so on.
std::string_view part_id(Part part);
std::optional<Part> part_from_id(std::string_view id);

// The edge of the window that the primary side bar, and the activity bar beside it, are at.
enum class Side { left, right };

// Where the panel is in relation to the editor area.
enum class PanelPosition { top, left, right, bottom };

// `top`, `left`, `right` and `bottom`, the values of VS Code's `panelPosition` context key.
std::string_view panel_position_name(PanelPosition position);

// Arranges the parts of the window with splitters. The sidebar and the panel resize with a sash
// and show or hide with their size remembered; hiding one gives its space to the editor area. The
// parts other than the editor area are empty containers for views to be added to: the sidebar and
// the panel start hidden, and so does the activity bar, because nothing is in them.
//
// The sidebar, with the activity bar, is at the left edge or the right edge. The panel is below the
// editor area, above it, or at its left or right; a position has its own size, which is a height
// for the top and the bottom and a width for the left and the right.
//
// The status bar and the title bar belong to the window, which hands them over; the status bar goes
// along the bottom edge and the title bar along the top, and they show and hide like the other
// parts. A window without a title bar of its own, one that has the system's frame, hands none over.
class Workbench : public QWidget {
    Q_OBJECT

public:
    // The width at zoom level 0.
    static constexpr int activity_bar_width = 48;
    // The sizes that follow, at zoom level 0.
    static constexpr int sidebar_minimum_width = 170;
    static constexpr int panel_minimum_height = 77;
    // The least width of a panel that is at the left or the right of the editor area.
    static constexpr int panel_minimum_width = 300;
    // What the editor area keeps however small the window gets.
    static constexpr int editor_minimum_width = 220;
    static constexpr int editor_minimum_height = 70;
    // Where a part starts when it is first shown.
    static constexpr int default_size = 300;

    // The colors, which must outlive the widget, come from the `activityBar`, `sideBar`, `panel`
    // and `sash` keys.
    explicit Workbench(const WorkbenchColors& colors, QWidget* parent = nullptr);

    // The zoom that the sizes of the workbench are read through (the width of the activity bar, the
    // minimum sizes of the parts, the sashes), which must outlive the workbench. A change of the
    // level keeps each part at the size it has, to the new minimum at least. Null means level 0.
    void set_zoom(const Zoom* zoom);
    const Zoom* zoom() const {
        return zoom_;
    }
    ~Workbench() override;

    // The editor area is the widget that fills the middle; the workbench owns it.
    void set_editor_area(QWidget* editor_area);
    void set_status_bar(QWidget* status_bar);
    void set_title_bar(QWidget* title_bar);

    // The containers that views go into.
    QWidget* activity_bar() const;
    QWidget* sidebar() const;
    QWidget* panel() const;

    bool is_visible(Part part) const;
    void set_visible(Part part, bool visible);
    void toggle(Part part);

    // The panel can take the whole editor area: the editor area is hidden while it does, and
    // restoring shows it again with the panel at the size it had. Maximizing shows a hidden panel,
    // and hiding the panel restores it. Other parts cannot be maximized.
    bool is_maximized(Part part) const;
    void set_maximized(Part part, bool maximized);

    Side sidebar_side() const;
    void set_sidebar_side(Side side);

    PanelPosition panel_position() const;
    // Moves the panel; a maximized panel stays maximized and a hidden one stays hidden.
    void set_panel_position(PanelPosition position);

    // How far the part reaches along the axis it is resized on: the width of the activity bar, the
    // sidebar and the editor area, the height of the panel at the top or the bottom and its width
    // at the left or the right, and the height of the status bar and the title bar. A hidden
    // sidebar or panel reports the size it has when shown again.
    int size(Part part) const;
    // Resizes the sidebar or the panel, to its minimum at least; the editor area keeps its own
    // minimum whatever is asked. Other parts keep their size. The panel is resized at its current
    // position.
    void set_size(Part part, int pixels);

signals:
    void visibility_changed(anvil::ui::Part part, bool visible);
    void maximized_changed(anvil::ui::Part part, bool maximized);
    void sidebar_side_changed(anvil::ui::Side side);
    void panel_position_changed(anvil::ui::PanelPosition position);

protected:
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    class PartWidget;

    void apply_sizes();
    void arrange();
    bool panel_is_beside_editor() const;
    int panel_extent() const;
    int& panel_size();
    int panel_size() const;

    const WorkbenchColors& colors_;
    PartWidget* activity_bar_ = nullptr;
    const Zoom* zoom_ = nullptr;
    PartWidget* sidebar_ = nullptr;
    PartWidget* panel_ = nullptr;
    QWidget* editor_host_ = nullptr;
    Splitter* across_ = nullptr;
    Splitter* down_ = nullptr;
    QVBoxLayout* root_layout_ = nullptr;
    QHBoxLayout* row_layout_ = nullptr;
    QPointer<QWidget> status_bar_;
    QPointer<QWidget> title_bar_;
    int zoomed(int size) const {
        return scaled_by(zoom_, size);
    }
    // Sets the minimum sizes of the parts: the ones of the zoom, but no more than the window has
    // room for, so that a window at a high level still lays out inside itself.
    void apply_minimums();
    int room_across() const;
    int room_down() const;
    int capped(int base, int room) const;
    int sidebar_minimum() const;
    int panel_minimum() const;
    int default_size_now_ = default_size;
    int sidebar_size_ = default_size;
    std::array<int, 4> panel_sizes_{default_size, default_size, default_size, default_size};
    Side sidebar_side_ = Side::left;
    PanelPosition panel_position_ = PanelPosition::bottom;
    bool panel_maximized_ = false;
};

} // namespace anvil::ui
