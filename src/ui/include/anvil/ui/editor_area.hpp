#pragma once

#include "anvil/core/document.hpp"
#include "anvil/core/editor_settings.hpp"
#include "anvil/core/uri.hpp"
#include "anvil/ui/editor_view.hpp"
#include "anvil/ui/find_bar.hpp"
#include "anvil/ui/tab_strip.hpp"
#include "anvil/ui/workbench_colors.hpp"
#include "anvil/workbench/editor_layout.hpp"

#include <QPointer>
#include <QWidget>
#include <functional>
#include <memory>
#include <vector>

class QSplitter;
class QVBoxLayout;

namespace anvil::ui {

// One group of the editor area: the strip of tabs, the find bar and the editor of a group. A group
// without tabs shows nothing but the editor background.
class EditorGroupWidget : public QWidget {
    Q_OBJECT

public:
    EditorGroupWidget(workbench::GroupId id, const WorkbenchColors& colors, QWidget* parent);

    workbench::GroupId id() const {
        return id_;
    }
    TabStrip* tabs() {
        return tabs_;
    }
    FindBar* find_bar() {
        return find_bar_;
    }
    EditorView* view() {
        return view_;
    }
    // The zoom that the strip of tabs and the find bar read their sizes through.
    void set_zoom(const Zoom* zoom);
    // How the strip of tabs shows the tabs, or that it is not shown at all: the group keeps its
    // editor and find bar.
    void set_tabs_mode(core::TabsMode mode);
    // Hides the editor and the find bar of a group that has no tab to show.
    void set_empty(bool empty);
    bool is_empty() const {
        return empty_;
    }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    workbench::GroupId id_;
    const WorkbenchColors& colors_;
    TabStrip* tabs_ = nullptr;
    FindBar* find_bar_ = nullptr;
    EditorView* view_ = nullptr;
    bool empty_ = false;
};

// The editor area of the window, built from a `workbench::EditorLayout` and kept in step with it:
// a splitter for each splitter of the tree, and a group widget with a tab strip, a find bar and an
// editor for each group. It changes nothing in the layout by itself, except for the ratios when the
// user moves a splitter: what the user does is reported by signals, and the window answers by
// changing the layout, whose notifications change the widgets.
//
// The tabs of a group are described by `tab_info` and the document of a tab comes from
// `document_for`; the widgets hold no state about documents beyond what they show.
class EditorArea : public QWidget, private workbench::EditorLayout::Observer {
    Q_OBJECT

public:
    using TabInfo = std::function<TabStrip::Tab(const core::Uri&)>;
    using DocumentLookup = std::function<std::shared_ptr<core::Document>(const core::Uri&)>;
    // The highlighter for the views of a document, or null for plain text.
    using HighlighterLookup =
        std::function<std::shared_ptr<const core::Highlighter>(const core::Uri&)>;

    // The layout and the colors must outlive the area.
    EditorArea(workbench::EditorLayout& layout, const WorkbenchColors& colors, TabInfo tab_info,
               DocumentLookup document_for, QWidget* parent = nullptr);
    ~EditorArea() override;

    EditorGroupWidget* group(workbench::GroupId id) const;
    EditorGroupWidget* focused_group() const;
    std::vector<EditorGroupWidget*> groups() const;

    // How the lines of a document wrap in its editors.
    struct Wrapping {
        EditorView::Wrapping mode = EditorView::Wrapping::off;
        std::size_t column = 80;
    };
    using WrappingLookup = std::function<Wrapping(const std::shared_ptr<core::Document>&)>;

    // Where the editors learn how to wrap the document they show. Without it they do not wrap.
    // `refresh_wrapping` asks again for every editor.
    void set_wrapping_lookup(WrappingLookup lookup);
    void refresh_wrapping();

    // Where the editors get the highlighter of the document they show. Without one they show plain
    // text. `refresh_highlighters` asks again for every editor, which is what a change of the
    // language of a document needs.
    void set_highlighter_lookup(HighlighterLookup lookup);
    void refresh_highlighters();

    // Applies the settings to the editors of every group, and to the groups that come later.
    void apply_settings(const core::EditorSettings& settings);
    // Sets how every group shows its tabs, and how the groups that come later do.
    void set_tabs_mode(core::TabsMode mode);
    // The zoom that the strips of tabs read their sizes through, which must outlive the area.
    void set_zoom(const Zoom* zoom);
    // Describes the tab again, which does nothing when it is unchanged.
    void refresh_tab(workbench::GroupId group, std::size_t index);

    // While a tab is dragged out of its strip: the part of the area that is shaded to show where
    // the drop goes, empty when there is none, and whether the picture of the tab is shown.
    QRect drop_shade() const;
    bool drag_picture_visible() const;
    // The size of the picture of the dragged tab; empty before one was shown.
    QSize drag_picture_size() const;

    // The splitter that holds the groups, or null while there is only one group.
    QSplitter* root_splitter() const;

    // Centered layout: while it is on and the area has a single group, the group is narrower than
    // the area and the same space is left at each side, so that lines keep a readable width. With
    // several groups the margins are gone and come back when one group is left. The margin is the
    // part of the width that VS Code's centered layout starts with.
    static constexpr double centered_margin_ratio = 0.1909;
    bool centered() const {
        return centered_;
    }
    void set_centered(bool centered);
    // The width of the space at each side of the groups now.
    int margin() const;

signals:
    void tab_selected(anvil::workbench::GroupId group, int index);
    void tab_moved(anvil::workbench::GroupId group, int from, int to);
    // The strip of the group was double-clicked where it has no tab.
    void new_file_requested(anvil::workbench::GroupId group);
    void close_requested(anvil::workbench::GroupId group, int index);
    void close_others_requested(anvil::workbench::GroupId group, int index);
    void close_to_the_right_requested(anvil::workbench::GroupId group, int index);
    void close_saved_requested(anvil::workbench::GroupId group);
    void copy_path_requested(anvil::workbench::GroupId group, int index);
    // The user changed the text of the group's editor.
    void text_changed(anvil::workbench::GroupId group, const anvil::core::DocumentChange& change);
    void selections_changed(anvil::workbench::GroupId group);
    // The keyboard focus is in the group, which the layout may not know yet.
    void focus_requested(anvil::workbench::GroupId group);
    // A tab was dragged out of its strip and dropped: on the strip of a group, at this position
    // among its tabs; on the middle of a group; or on the edge of a group, which asks to split the
    // group in that direction and to put the tab in the new group. The area changes nothing itself.
    void tab_dropped_on_strip(anvil::workbench::GroupId from, int index,
                              anvil::workbench::GroupId to, int position);
    void tab_dropped_on_group(anvil::workbench::GroupId from, int index,
                              anvil::workbench::GroupId to);
    void tab_dropped_on_edge(anvil::workbench::GroupId from, int index,
                             anvil::workbench::GroupId to, anvil::workbench::Direction direction);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void update_margins();
    void structure_changed() override;
    void ratios_changed() override;
    void tabs_changed(workbench::GroupId group) override;
    void active_tab_changed(workbench::GroupId group) override;
    void focus_changed(workbench::GroupId group) override;

    EditorGroupWidget* add_group(workbench::GroupId id);
    void rebuild();
    QWidget* build(const workbench::LayoutNode& node, std::vector<std::size_t>& path);
    void apply_sizes(const workbench::LayoutNode& node, QWidget* widget);
    void sync_tabs(EditorGroupWidget& widget);
    void sync_document(EditorGroupWidget& widget);
    void sync_highlighter(EditorGroupWidget& widget);
    // Marks the group that has the focus in the layout and moves the keyboard focus into it.
    void apply_focus();
    void on_focus_changed(QWidget* now);

    struct DropTarget;
    // What dropping at a position of the screen would do for the tab dragged from `from`.
    DropTarget drop_target_at(workbench::GroupId from, const QPoint& global) const;
    void drag_out_moved(workbench::GroupId from, int index, const QPoint& global);
    void drag_out_dropped(workbench::GroupId from, int index, const QPoint& global);
    void end_drag_out();

    workbench::EditorLayout& layout_;
    const WorkbenchColors& colors_;
    const Zoom* zoom_ = nullptr;
    TabInfo tab_info_;
    DocumentLookup document_for_;
    HighlighterLookup highlighter_for_;
    WrappingLookup wrapping_for_;
    core::EditorSettings settings_;
    std::vector<EditorGroupWidget*> groups_;
    QVBoxLayout* box_ = nullptr;
    class DropOverlay;
    class DragGhost;
    DropOverlay* overlay_ = nullptr;
    DragGhost* ghost_ = nullptr;
    QMetaObject::Connection focus_connection_;
    QPointer<QWidget> root_;
    // While the sizes of the splitters are being set from the layout, a splitter that moves is not
    // the user's doing.
    bool applying_sizes_ = false;
    bool rebuilding_ = false;
    bool centered_ = false;
    core::TabsMode tabs_mode_ = core::TabsMode::multiple;
};

} // namespace anvil::ui
