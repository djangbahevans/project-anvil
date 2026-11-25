#pragma once

#include "anvil/ui/painted_accessibility.hpp"
#include "anvil/ui/workbench_colors.hpp"

#include <QRect>
#include <QString>
#include <QWidget>
#include <cstdint>
#include <memory>
#include <vector>

class QMenu;

namespace anvil::ui {

// The strip of tabs above an editor: VS Code's tab strip, painted by the widget itself.
//
// A tab shows a title, and on its right a close button that is a dot while the tab is dirty. The
// selected tab has its own background and a line on top. The close button of the selected tab is
// always there; that of another tab shows while the mouse is over the tab, and a dirty tab keeps a
// dot there that turns into the close button when the mouse is over it. A middle click closes a
// tab, a drag moves it, and a context menu offers closing the other tabs, the tabs to the right and
// the saved tabs, and copying the path. Tabs that do not fit scroll horizontally, by wheel, and the
// selected tab is kept in view.
//
// The strip is a view: only what the user does emits a signal. Changing the model through
// `add_tab`, `remove_tab`, `set_tab`, `move_tab` and `set_current_index` does not.
class TabStrip : public QWidget, public AccessibleItems {
    Q_OBJECT

public:
    static constexpr int height_pixels = 35;
    static constexpr int font_pixels = 13;
    // A tab is at least this wide, and as wide as its title otherwise.
    static constexpr int minimum_tab_width = 120;
    // Space before the title, and the width of the area of the close button after it.
    static constexpr int title_padding = 10;
    static constexpr int action_width = 28;
    static constexpr int close_button_size = 16;

    struct Tab {
        QString title = {};
        QString tooltip = {};
        // Shown as a dot in place of the close button.
        bool dirty = false;
        // Whether "Copy Path" has a path to copy.
        bool has_path = false;

        bool operator==(const Tab&) const = default;
    };

    // The colors, which must outlive the widget, come from the `tab` and `editorGroupHeader`
    // keys. The strip repaints when they change.
    TabStrip(const WorkbenchColors& colors, QWidget* parent = nullptr);
    ~TabStrip() override;

    // Appends a tab and returns its index. The first tab becomes the selected one.
    int add_tab(Tab tab);
    // The selection keeps its tab, or, when that tab is removed, moves to the tab that takes its
    // place (the last one when there is none).
    void remove_tab(int index);
    // Setting a tab to what it already is does no work: the title of the tab being typed in is set
    // on every keystroke and must not grow with the number of tabs.
    void set_tab(int index, Tab tab);
    const Tab& tab(int index) const;
    void move_tab(int from, int to);
    int count() const;

    // -1 when there are no tabs.
    int current_index() const {
        return current_;
    }
    // Selects the tab and scrolls it into view.
    void set_current_index(int index);

    // Shows only the selected tab, as one tab as wide as the strip, instead of every tab. The other
    // tabs stay in the strip, with no area, and cannot be reached with the mouse or reordered.
    void set_single(bool single);
    bool single() const {
        return single_;
    }

    // Whether the group the strip belongs to has the focus. A strip that does not have it is
    // painted with the `tab.unfocused...` colors. A strip starts focused.
    void set_focused(bool focused);
    bool focused() const {
        return focused_;
    }

    // The area of the tab, or of its close button, in the coordinates of the widget; empty when
    // the index is out of range. The area can be partly outside the widget while it is scrolled.
    QRect tab_rect(int index) const;
    QRect close_button_rect(int index) const;
    // How far the tabs are scrolled to the left, in pixels.
    int scroll_offset() const {
        return scroll_;
    }
    // Where a tab dragged over the strip from another one would go: the position among the tabs
    // that the horizontal coordinate `x`, in the coordinates of the widget, is nearest to, from 0
    // to the number of tabs.
    int insertion_index_at(int x) const;
    // Shows a line at the position, which is what `insertion_index_at` gives, where a dragged tab
    // would go, or none for -1.
    void set_drop_indicator(int position);
    // The tab under the mouse, or -1.
    int hovered_tab() const {
        return hovered_;
    }
    bool is_dragging() const {
        return dragging_;
    }
    // True while the tab that is dragged is outside the strip, where a group or the window decides
    // what the drop does.
    bool is_dragging_out() const {
        return dragging_out_;
    }
    // The name of the action of a tab that closes it, besides the one that selects it.
    static QString close_action() {
        return QStringLiteral("Close");
    }
    // The tabs as assistive technology reads them: a page tab named by its title and described by
    // its tooltip (with "Unsaved changes" added while it is dirty) and selected when it is the
    // current one, which can be pressed to select it and closed with `close_action()`.
    std::vector<AccessibleItem> accessible_items() const override;
    void do_accessible_action(const QString& key, const QString& action) override;

    // The menu for a right click on the tab. The caller owns it.
    std::unique_ptr<QMenu> build_context_menu(int index);

signals:
    // The user selected a tab.
    void current_changed(int index);
    // The user dragged a tab past a neighbor: the tab at `from` is now at `to`.
    void tab_moved(int from, int to);
    // The user asked to close the tab with its close button or a middle click, or from the menu.
    void close_requested(int index);
    void close_others_requested(int index);
    void close_to_the_right_requested(int index);
    void close_saved_requested();
    void copy_path_requested(int index);
    // The tab that is dragged is outside the strip, at this position of the screen. It is emitted
    // for every move, and the strip changes nothing about its tabs.
    void drag_out_moved(int index, const QPoint& global_position);
    // The mouse button was released outside the strip.
    void drag_out_dropped(int index, const QPoint& global_position);
    // The drag outside the strip ended without a drop: the tab came back over the strip, or
    // Escape was pressed.
    void drag_out_cancelled();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    bool event(QEvent* event) override;

private:
    struct Entry {
        Tab tab;
        int title_width = 0;
        // In the coordinates of the content, before scrolling.
        int x = 0;
        int width = 0;
        // Identifies the tab for as long as it exists, whatever its index becomes.
        std::uint64_t id = 0;
    };

    // The index of the tab among the items that assistive technology sees, or -1 for a tab it does
    // not see.
    int accessible_index(int tab) const;
    void measure(Entry& entry) const;
    void ensure_layout() const;
    int content_width() const;
    int max_scroll() const;
    void set_scroll(int pixels);
    void scroll_into_view(int index);
    int tab_at(int x) const;
    void set_hovered(int tab, bool button);
    void drag_to(int x);
    void end_drag_out();
    void paint_tab(QPainter& painter, int index, int x) const;

    const WorkbenchColors& colors_;
    mutable std::vector<Entry> entries_;
    mutable bool layout_valid_ = false;
    int current_ = -1;
    bool single_ = false;
    int scroll_ = 0;
    bool focused_ = true;

    int hovered_ = -1;
    bool hovered_button_ = false;
    // The target of the pressed mouse button: a tab, or the close button of a tab.
    int pressed_ = -1;
    bool pressed_button_ = false;
    Qt::MouseButton pressed_with_ = Qt::NoButton;
    QPoint press_;
    int grab_offset_ = 0;
    bool dragging_ = false;
    bool dragging_out_ = false;
    int drop_indicator_ = -1;
    int drag_left_ = 0;
};

} // namespace anvil::ui
