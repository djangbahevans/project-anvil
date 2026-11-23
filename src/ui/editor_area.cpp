#include "anvil/ui/editor_area.hpp"

#include "anvil/ui/splitter.hpp"

#include <QApplication>
#include <QPainter>
#include <QSplitter>
#include <QVBoxLayout>
#include <algorithm>

namespace anvil::ui {

EditorGroupWidget::EditorGroupWidget(workbench::GroupId id, const WorkbenchColors& colors,
                                     QWidget* parent)
    : QWidget(parent), id_(id), colors_(colors) {
    setFocusPolicy(Qt::StrongFocus);
    tabs_ = new TabStrip(colors, this);
    view_ = new EditorView(this);
    view_->set_colors(&colors);
    find_bar_ = new FindBar(view_, this);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(tabs_);
    layout->addWidget(find_bar_);
    layout->addWidget(view_, 1);
    // Takes the space of the editor while it is hidden, so that the strip stays at the top.
    layout->addStretch(0);
    connect(&colors_, &WorkbenchColors::changed, this, qOverload<>(&QWidget::update));
}

void EditorGroupWidget::set_empty(bool empty) {
    if (empty_ == empty) {
        return;
    }
    empty_ = empty;
    view_->setVisible(!empty);
    if (empty) {
        find_bar_->close_bar();
    }
    update();
}

void EditorGroupWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), colors_.color("editor.background"));
}

// The shade over the part of a group where a dragged tab would go.
class EditorArea::DropOverlay : public QWidget {
public:
    DropOverlay(const WorkbenchColors& colors, QWidget* parent) : QWidget(parent), colors_(colors) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        hide();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QColor color = colors_.color("editorGroup.dropBackground");
        if (!colors_.is_themed("editorGroup.dropBackground")) {
            color.setAlpha(80);
        }
        QPainter(this).fillRect(rect(), color);
    }

private:
    const WorkbenchColors& colors_;
};

// The dragged tab that follows the pointer, also outside the window.
class EditorArea::DragGhost : public QWidget {
public:
    DragGhost(const WorkbenchColors& colors, const QFont& font)
        : QWidget(nullptr, Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowTransparentForInput),
          colors_(colors) {
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TranslucentBackground);
        setFont(font);
    }

    void set_title(const QString& title) {
        title_ = title;
        resize(std::max(TabStrip::minimum_tab_width,
                        fontMetrics().horizontalAdvance(title) + 2 * TabStrip::title_padding),
               TabStrip::height_pixels);
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setOpacity(0.85);
        painter.fillRect(rect(), colors_.color("tab.activeBackground"));
        painter.setPen(colors_.color("tab.activeForeground"));
        painter.drawText(rect().adjusted(TabStrip::title_padding, 0, 0, 0),
                         Qt::AlignVCenter | Qt::AlignLeft, title_);
    }

private:
    const WorkbenchColors& colors_;
    QString title_;
};

struct EditorArea::DropTarget {
    enum class Kind { none, strip, group, edge };
    Kind kind = Kind::none;
    EditorGroupWidget* group = nullptr;
    int position = 0;
    workbench::Direction direction = workbench::Direction::right;
    // The part of the area to shade, in the coordinates of the area.
    QRect shade;
};

EditorArea::EditorArea(workbench::EditorLayout& layout, const WorkbenchColors& colors,
                       TabInfo tab_info, DocumentLookup document_for, QWidget* parent)
    : QWidget(parent), layout_(layout), colors_(colors), tab_info_(std::move(tab_info)),
      document_for_(std::move(document_for)) {
    box_ = new QVBoxLayout(this);
    box_->setContentsMargins(0, 0, 0, 0);
    box_->setSpacing(0);
    layout_.add_observer(this);
    overlay_ = new DropOverlay(colors_, this);
    rebuild();
    focus_connection_ = connect(qApp, &QApplication::focusChanged, this,
                                [this](QWidget*, QWidget* now) { on_focus_changed(now); });
}

EditorArea::~EditorArea() {
    // The groups are destroyed after this body, and a group that has the focus gives it up then,
    // which would call a half-destroyed area.
    disconnect(focus_connection_);
    layout_.remove_observer(this);
    delete ghost_;
}

EditorGroupWidget* EditorArea::group(workbench::GroupId id) const {
    const auto found =
        std::ranges::find_if(groups_, [id](const EditorGroupWidget* g) { return g->id() == id; });
    return found == groups_.end() ? nullptr : *found;
}

EditorGroupWidget* EditorArea::focused_group() const {
    return group(layout_.focused());
}

std::vector<EditorGroupWidget*> EditorArea::groups() const {
    std::vector<EditorGroupWidget*> ordered;
    for (const workbench::GroupId id : layout_.group_ids()) {
        ordered.push_back(group(id));
    }
    return ordered;
}

QRect EditorArea::drop_shade() const {
    return overlay_->isVisible() ? overlay_->geometry() : QRect();
}

bool EditorArea::drag_picture_visible() const {
    return ghost_ && ghost_->isVisible();
}

QSplitter* EditorArea::root_splitter() const {
    return qobject_cast<QSplitter*>(root_);
}

void EditorArea::apply_settings(const core::EditorSettings& settings) {
    settings_ = settings;
    for (EditorGroupWidget* widget : groups_) {
        widget->view()->apply_settings(settings_);
    }
}

void EditorArea::refresh_tab(workbench::GroupId id, std::size_t index) {
    EditorGroupWidget* widget = group(id);
    const workbench::EditorGroup* state = layout_.group(id);
    if (widget && state && index < state->tabs.size()) {
        widget->tabs()->set_tab(static_cast<int>(index), tab_info_(state->tabs[index]));
    }
}

EditorGroupWidget* EditorArea::add_group(workbench::GroupId id) {
    auto* widget = new EditorGroupWidget(id, colors_, this);
    widget->view()->apply_settings(settings_);
    groups_.push_back(widget);

    TabStrip* tabs = widget->tabs();
    connect(tabs, &TabStrip::current_changed, this,
            [this, id](int index) { emit tab_selected(id, index); });
    connect(tabs, &TabStrip::tab_moved, this,
            [this, id](int from, int to) { emit tab_moved(id, from, to); });
    connect(tabs, &TabStrip::close_requested, this,
            [this, id](int index) { emit close_requested(id, index); });
    connect(tabs, &TabStrip::close_others_requested, this,
            [this, id](int index) { emit close_others_requested(id, index); });
    connect(tabs, &TabStrip::close_to_the_right_requested, this,
            [this, id](int index) { emit close_to_the_right_requested(id, index); });
    connect(tabs, &TabStrip::close_saved_requested, this,
            [this, id] { emit close_saved_requested(id); });
    connect(tabs, &TabStrip::copy_path_requested, this,
            [this, id](int index) { emit copy_path_requested(id, index); });
    connect(tabs, &TabStrip::drag_out_moved, this,
            [this, id](int index, const QPoint& global) { drag_out_moved(id, index, global); });
    connect(tabs, &TabStrip::drag_out_dropped, this,
            [this, id](int index, const QPoint& global) { drag_out_dropped(id, index, global); });
    connect(tabs, &TabStrip::drag_out_cancelled, this, [this] { end_drag_out(); });
    connect(widget->view(), &EditorView::text_changed, this,
            [this, id](const core::DocumentChange& change) { emit text_changed(id, change); });
    connect(widget->view(), &EditorView::selections_changed, this,
            [this, id] { emit selections_changed(id); });

    sync_tabs(*widget);
    sync_document(*widget);
    widget->tabs()->set_focused(layout_.focused() == id);
    return widget;
}

void EditorArea::rebuild() {
    const std::vector<workbench::GroupId> ids = layout_.group_ids();
    std::erase_if(groups_, [&](const EditorGroupWidget* widget) {
        return std::ranges::find(ids, widget->id()) == ids.end();
    });
    for (const workbench::GroupId id : ids) {
        if (!group(id)) {
            add_group(id);
        }
    }

    // Moving a widget that has the focus into another splitter takes the focus from it, which is
    // not the user's doing.
    rebuilding_ = true;
    const QPointer<QWidget> old_root = root_;
    if (old_root) {
        box_->removeWidget(old_root);
    }
    applying_sizes_ = true;
    std::vector<std::size_t> path;
    root_ = build(layout_.root(), path);
    box_->addWidget(root_);
    root_->show();
    apply_sizes(layout_.root(), root_);
    applying_sizes_ = false;
    rebuilding_ = false;
    apply_focus();

    // The splitters that are no longer used, with the groups that were closed inside them, go after
    // the signal that may have led here has returned. A single group that was the root is kept.
    if (old_root && qobject_cast<QSplitter*>(old_root)) {
        old_root->hide();
        old_root->deleteLater();
    }
}

QWidget* EditorArea::build(const workbench::LayoutNode& node, std::vector<std::size_t>& path) {
    if (node.is_group()) {
        EditorGroupWidget* widget = group(*node.group);
        widget->show();
        return widget;
    }
    auto* splitter = new Splitter(
        node.orientation == workbench::Orientation::horizontal ? Qt::Horizontal : Qt::Vertical,
        colors_, "editorGroup.border");
    for (std::size_t i = 0; i < node.children.size(); ++i) {
        path.push_back(i);
        splitter->addWidget(build(node.children[i], path));
        path.pop_back();
    }
    connect(splitter, &QSplitter::splitterMoved, this, [this, splitter, path] {
        if (applying_sizes_) {
            return;
        }
        std::vector<double> ratios;
        for (const int size : splitter->sizes()) {
            ratios.push_back(std::max(1, size));
        }
        layout_.set_ratios(path, std::move(ratios));
    });
    return splitter;
}

void EditorArea::apply_sizes(const workbench::LayoutNode& node, QWidget* widget) {
    auto* splitter = qobject_cast<QSplitter*>(widget);
    if (node.is_group() || !splitter) {
        return;
    }
    constexpr double scale = 100000;
    QList<int> sizes;
    for (const double ratio : node.ratios) {
        sizes.push_back(std::max(1, static_cast<int>(ratio * scale)));
    }
    splitter->setSizes(sizes);
    for (std::size_t i = 0; i < node.children.size(); ++i) {
        apply_sizes(node.children[i], splitter->widget(static_cast<int>(i)));
    }
}

void EditorArea::sync_tabs(EditorGroupWidget& widget) {
    const workbench::EditorGroup* state = layout_.group(widget.id());
    TabStrip* tabs = widget.tabs();
    const int count = static_cast<int>(state->tabs.size());
    while (tabs->count() > count) {
        tabs->remove_tab(tabs->count() - 1);
    }
    for (int i = 0; i < count; ++i) {
        const TabStrip::Tab info = tab_info_(state->tabs[static_cast<std::size_t>(i)]);
        if (i < tabs->count()) {
            tabs->set_tab(i, info);
        } else {
            tabs->add_tab(info);
        }
    }
    if (state->active) {
        tabs->set_current_index(static_cast<int>(*state->active));
    }
}

void EditorArea::sync_document(EditorGroupWidget& widget) {
    const workbench::EditorGroup* state = layout_.group(widget.id());
    std::shared_ptr<core::Document> document;
    if (state->active) {
        document = document_for_(state->tabs[*state->active]);
    }
    if (widget.view()->document() != document) {
        widget.view()->set_document(document);
    }
    widget.set_empty(!document);
    if (state->active) {
        widget.tabs()->set_current_index(static_cast<int>(*state->active));
    }
}

void EditorArea::structure_changed() {
    rebuild();
}

void EditorArea::ratios_changed() {
    applying_sizes_ = true;
    apply_sizes(layout_.root(), root_);
    applying_sizes_ = false;
}

void EditorArea::tabs_changed(workbench::GroupId id) {
    if (EditorGroupWidget* widget = group(id)) {
        sync_tabs(*widget);
    }
}

void EditorArea::active_tab_changed(workbench::GroupId id) {
    if (EditorGroupWidget* widget = group(id)) {
        sync_document(*widget);
    }
}

void EditorArea::focus_changed(workbench::GroupId) {
    apply_focus();
}

void EditorArea::apply_focus() {
    const workbench::GroupId id = layout_.focused();
    for (EditorGroupWidget* widget : groups_) {
        widget->tabs()->set_focused(widget->id() == id);
    }
    EditorGroupWidget* widget = group(id);
    if (!widget) {
        return;
    }
    // A find bar or a strip of the group that has the focus keeps it.
    QWidget* current = QApplication::focusWidget();
    if (current && widget->isAncestorOf(current)) {
        return;
    }
    if (widget->is_empty()) {
        widget->setFocus();
    } else {
        widget->view()->setFocus();
    }
}

EditorArea::DropTarget EditorArea::drop_target_at(workbench::GroupId from,
                                                  const QPoint& global) const {
    DropTarget target;
    for (EditorGroupWidget* widget : groups_) {
        const QPoint local = widget->mapFromGlobal(global);
        if (!widget->rect().contains(local) || !widget->isVisible()) {
            continue;
        }
        target.group = widget;
        const QRect strip = widget->tabs()->geometry();
        if (strip.contains(local)) {
            target.kind = DropTarget::Kind::strip;
            target.position = widget->tabs()->insertion_index_at(local.x() - strip.left());
            return target;
        }
        const QRect content(0, strip.bottom() + 1, widget->width(),
                            widget->height() - strip.bottom() - 1);
        const double x = static_cast<double>(local.x() - content.left()) / content.width();
        const double y = static_cast<double>(local.y() - content.top()) / content.height();
        const double distances[] = {x, 1 - x, y, 1 - y};
        constexpr workbench::Direction directions[] = {
            workbench::Direction::left, workbench::Direction::right, workbench::Direction::up,
            workbench::Direction::down};
        const auto nearest = std::ranges::min_element(distances) - std::begin(distances);
        QRect shade = content;
        if (distances[nearest] < 0.25) {
            // Splitting a group that has only the dragged tab would change nothing.
            if (widget->id() == from && layout_.group(from)->tabs.size() < 2) {
                return target;
            }
            target.kind = DropTarget::Kind::edge;
            target.direction = directions[nearest];
            switch (target.direction) {
            case workbench::Direction::left:
                shade.setWidth(content.width() / 2);
                break;
            case workbench::Direction::right:
                shade.setLeft(content.left() + content.width() / 2);
                break;
            case workbench::Direction::up:
                shade.setHeight(content.height() / 2);
                break;
            case workbench::Direction::down:
                shade.setTop(content.top() + content.height() / 2);
                break;
            }
        } else if (widget->id() != from) {
            target.kind = DropTarget::Kind::group;
        } else {
            return target;
        }
        target.shade = QRect(widget->mapTo(this, shade.topLeft()), shade.size());
        return target;
    }
    return target;
}

void EditorArea::drag_out_moved(workbench::GroupId from, int index, const QPoint& global) {
    const workbench::EditorGroup* state = layout_.group(from);
    if (!state || static_cast<std::size_t>(index) >= state->tabs.size()) {
        return;
    }
    if (!ghost_) {
        ghost_ = new DragGhost(colors_, font());
    }
    ghost_->set_title(tab_info_(state->tabs[static_cast<std::size_t>(index)]).title);
    ghost_->move(global + QPoint(12, 12));
    ghost_->show();

    const DropTarget target = drop_target_at(from, global);
    for (EditorGroupWidget* widget : groups_) {
        widget->tabs()->set_drop_indicator(
            target.kind == DropTarget::Kind::strip && target.group == widget ? target.position
                                                                             : -1);
    }
    if (target.kind == DropTarget::Kind::group || target.kind == DropTarget::Kind::edge) {
        overlay_->setGeometry(target.shade);
        overlay_->show();
        overlay_->raise();
    } else {
        overlay_->hide();
    }
}

void EditorArea::drag_out_dropped(workbench::GroupId from, int index, const QPoint& global) {
    const DropTarget target = drop_target_at(from, global);
    end_drag_out();
    switch (target.kind) {
    case DropTarget::Kind::strip:
        emit tab_dropped_on_strip(from, index, target.group->id(), target.position);
        break;
    case DropTarget::Kind::group:
        emit tab_dropped_on_group(from, index, target.group->id());
        break;
    case DropTarget::Kind::edge:
        emit tab_dropped_on_edge(from, index, target.group->id(), target.direction);
        break;
    case DropTarget::Kind::none:
        break;
    }
}

void EditorArea::end_drag_out() {
    if (ghost_) {
        ghost_->hide();
    }
    overlay_->hide();
    for (EditorGroupWidget* widget : groups_) {
        widget->tabs()->set_drop_indicator(-1);
    }
}

void EditorArea::on_focus_changed(QWidget* now) {
    if (!now || rebuilding_) {
        return;
    }
    for (const EditorGroupWidget* widget : groups_) {
        if (widget == now || widget->isAncestorOf(now)) {
            if (widget->id() != layout_.focused()) {
                emit focus_requested(widget->id());
            }
            return;
        }
    }
}

} // namespace anvil::ui
