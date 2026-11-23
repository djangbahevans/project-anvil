#pragma once

#include "anvil/ui/workbench_colors.hpp"

#include <QPointer>
#include <QWidget>
#include <optional>
#include <string_view>

class QSplitter;
class QVBoxLayout;

namespace anvil::ui {

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

// Arranges the parts of the window with splitters. The sidebar and the panel resize with a sash
// and show or hide with their size remembered; hiding one gives its space to the editor area. The
// parts other than the editor area are empty containers for views to be added to: the sidebar and
// the panel start hidden, and so does the activity bar, because nothing is in them.
//
// The status bar and the title bar belong to the window, which hands them over; the status bar goes
// along the bottom edge and the title bar along the top, and they show and hide like the other
// parts. A window without a title bar of its own, one that has the system's frame, hands none over.
class Workbench : public QWidget {
    Q_OBJECT

public:
    static constexpr int activity_bar_width = 48;
    static constexpr int sidebar_minimum_width = 170;
    static constexpr int panel_minimum_height = 77;
    // What the editor area keeps however small the window gets.
    static constexpr int editor_minimum_width = 220;
    static constexpr int editor_minimum_height = 70;
    // Where a part starts when it is first shown.
    static constexpr int default_size = 300;

    // The colors, which must outlive the widget, come from the `activityBar`, `sideBar`, `panel`
    // and `sash` keys.
    explicit Workbench(const WorkbenchColors& colors, QWidget* parent = nullptr);
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

    // How far the part reaches along the axis it is resized on: the width of the activity bar, the
    // sidebar and the editor area, the height of the panel, the status bar and the title bar. A
    // hidden sidebar or panel reports the size it has when shown again.
    int size(Part part) const;
    // Resizes the sidebar or the panel, to its minimum at least; the editor area keeps its own
    // minimum whatever is asked. Other parts keep their size.
    void set_size(Part part, int pixels);

signals:
    void visibility_changed(anvil::ui::Part part, bool visible);
    void maximized_changed(anvil::ui::Part part, bool maximized);

protected:
    void showEvent(QShowEvent* event) override;

private:
    class PartWidget;

    void apply_sizes();

    const WorkbenchColors& colors_;
    PartWidget* activity_bar_ = nullptr;
    PartWidget* sidebar_ = nullptr;
    PartWidget* panel_ = nullptr;
    QWidget* editor_host_ = nullptr;
    QSplitter* across_ = nullptr;
    QSplitter* down_ = nullptr;
    QVBoxLayout* root_layout_ = nullptr;
    QPointer<QWidget> status_bar_;
    QPointer<QWidget> title_bar_;
    int sidebar_size_ = default_size;
    int panel_size_ = default_size;
    bool panel_maximized_ = false;
};

} // namespace anvil::ui
