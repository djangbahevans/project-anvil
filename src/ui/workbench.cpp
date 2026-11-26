#include "anvil/ui/workbench.hpp"

#include "anvil/ui/splitter.hpp"

#include <QEnterEvent>
#include <QHBoxLayout>
#include <QPainter>
#include <QVBoxLayout>
#include <algorithm>

namespace anvil::ui {

std::string_view panel_position_name(PanelPosition position) {
    switch (position) {
    case PanelPosition::top:
        return "top";
    case PanelPosition::left:
        return "left";
    case PanelPosition::right:
        return "right";
    case PanelPosition::bottom:
        return "bottom";
    }
    return {};
}

std::string_view part_id(Part part) {
    switch (part) {
    case Part::title_bar:
        return "workbench.parts.titlebar";
    case Part::activity_bar:
        return "workbench.parts.activitybar";
    case Part::sidebar:
        return "workbench.parts.sidebar";
    case Part::editor:
        return "workbench.parts.editor";
    case Part::panel:
        return "workbench.parts.panel";
    case Part::status_bar:
        return "workbench.parts.statusbar";
    }
    return {};
}

std::optional<Part> part_from_id(std::string_view id) {
    for (const Part part : {Part::title_bar, Part::activity_bar, Part::sidebar, Part::editor,
                            Part::panel, Part::status_bar}) {
        if (part_id(part) == id) {
            return part;
        }
    }
    return std::nullopt;
}

// An empty container that paints its background from a color key.
class Workbench::PartWidget : public QWidget {
public:
    PartWidget(const WorkbenchColors& colors, std::string_view background, QWidget* parent)
        : QWidget(parent), colors_(colors), background_(background) {}

    // Draws a line of one pixel along the left or the right edge, in the color of `key`, while the
    // theme has that color.
    void set_border(std::string_view key, Qt::Edge edge) {
        border_ = key;
        edge_ = edge;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), colors_.color(background_));
        if (!border_.empty() && colors_.is_themed(border_)) {
            painter.fillRect(QRect(edge_ == Qt::LeftEdge ? 0 : width() - 1, 0, 1, height()),
                             colors_.color(border_));
        }
    }

private:
    const WorkbenchColors& colors_;
    std::string_view background_;
    std::string_view border_;
    Qt::Edge edge_ = Qt::RightEdge;
};

void Workbench::set_zoom(const Zoom* zoom) {
    if (zoom_) {
        disconnect(zoom_, &Zoom::changed, this, nullptr);
    }
    zoom_ = zoom;
    const auto apply = [this] {
        // The size a part has now is the size it keeps; only the minimum can move it.
        if (sidebar_->isVisible()) {
            sidebar_size_ = sidebar_->width();
        }
        if (panel_->isVisible() && !panel_maximized_) {
            panel_size() = panel_extent();
        }
        const int previous_default = default_size_now_;
        default_size_now_ = zoomed(default_size);
        if (sidebar_size_ == previous_default) {
            sidebar_size_ = default_size_now_;
        }
        for (int& size : panel_sizes_) {
            if (size == previous_default) {
                size = default_size_now_;
            }
        }
        activity_bar_->setFixedWidth(zoomed(activity_bar_width));
        apply_minimums();
        sidebar_size_ = std::max(sidebar_size_, sidebar_minimum());
        for (std::size_t i = 0; i < panel_sizes_.size(); ++i) {
            const bool beside = i == static_cast<std::size_t>(PanelPosition::left) ||
                                i == static_cast<std::size_t>(PanelPosition::right);
            panel_sizes_[i] = std::max(panel_sizes_[i],
                                       zoomed(beside ? panel_minimum_width : panel_minimum_height));
        }
        for (Splitter* splitter : {across_, down_}) {
            splitter->set_zoom(zoom_);
        }
        apply_sizes();
    };
    if (zoom_) {
        connect(zoom_, &Zoom::changed, this, apply);
    }
    apply();
}

// What is left of the width of the window beside the activity bar and the sashes, and of the
// height below the title bar and above the status bar.
int Workbench::room_across() const {
    const int sashes = Splitter::line_width * (panel_is_beside_editor() ? 2 : 1);
    return std::max(2, width() - (activity_bar_->isHidden() ? 0 : activity_bar_->width()) - sashes);
}

int Workbench::room_down() const {
    int taken = 0;
    for (const QWidget* bar : {title_bar_.data(), status_bar_.data()}) {
        taken += bar && !bar->isHidden() ? bar->height() : 0;
    }
    return std::max(2, height() - taken);
}

// The minimum of a part at the zoom, and at a level above 0 no more than the part may take of the
// room so that two parts that share it both fit, though never under the minimum at level 0.
int Workbench::capped(int base, int room) const {
    const int scaled = zoomed(base);
    return scaled <= base ? scaled : std::max(base, std::min(scaled, room / 2));
}

int Workbench::sidebar_minimum() const {
    return capped(sidebar_minimum_width, room_across());
}

int Workbench::panel_minimum() const {
    return panel_is_beside_editor() ? capped(panel_minimum_width, room_across() * 2 / 3)
                                    : capped(panel_minimum_height, room_down());
}

void Workbench::apply_minimums() {
    sidebar_->setMinimumWidth(sidebar_minimum());
    editor_host_->setMinimumSize(capped(editor_minimum_width, room_across()),
                                 capped(editor_minimum_height, room_down()));
    const bool beside = panel_is_beside_editor();
    panel_->setMinimumSize(beside ? panel_minimum() : 0, beside ? 0 : panel_minimum());
}

void Workbench::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    apply_minimums();
}

Workbench::Workbench(const WorkbenchColors& colors, QWidget* parent)
    : QWidget(parent), colors_(colors) {
    activity_bar_ = new PartWidget(colors_, "activityBar.background", this);
    activity_bar_->setFixedWidth(zoomed(activity_bar_width));
    activity_bar_->set_border("activityBar.border", Qt::RightEdge);
    activity_bar_->hide();

    sidebar_ = new PartWidget(colors_, "sideBar.background", this);
    sidebar_->hide();
    panel_ = new PartWidget(colors_, "panel.background", this);
    panel_->hide();

    editor_host_ = new QWidget(this);
    auto* editor_layout = new QVBoxLayout(editor_host_);
    editor_layout->setContentsMargins(0, 0, 0, 0);

    down_ = new Splitter(Qt::Vertical, colors_, "panel.border", this);
    down_->addWidget(editor_host_);
    down_->addWidget(panel_);
    down_->setStretchFactor(0, 1);
    down_->setStretchFactor(1, 0);

    across_ = new Splitter(Qt::Horizontal, colors_, "sideBar.border", this);
    across_->addWidget(sidebar_);
    across_->addWidget(down_);
    across_->setStretchFactor(0, 0);
    across_->setStretchFactor(1, 1);

    auto* row = new QWidget(this);
    row_layout_ = new QHBoxLayout(row);
    row_layout_->setContentsMargins(0, 0, 0, 0);
    row_layout_->setSpacing(0);
    row_layout_->addWidget(activity_bar_);
    row_layout_->addWidget(across_, 1);
    apply_minimums();

    // A drag is recorded at once, so that the size of a part is known when it is not on screen.
    connect(across_, &QSplitter::splitterMoved, this, [this] {
        if (sidebar_->isVisible()) {
            sidebar_size_ = sidebar_->width();
        }
        emit sizes_changed();
    });
    connect(down_, &QSplitter::splitterMoved, this, [this] {
        if (panel_->isVisible() && !panel_maximized_) {
            panel_size() = panel_extent();
        }
        emit sizes_changed();
    });

    root_layout_ = new QVBoxLayout(this);
    root_layout_->setContentsMargins(0, 0, 0, 0);
    root_layout_->setSpacing(0);
    root_layout_->addWidget(row, 1);

    connect(&colors_, &WorkbenchColors::changed, this, [this] {
        for (QWidget* widget : {static_cast<QWidget*>(activity_bar_),
                                static_cast<QWidget*>(sidebar_), static_cast<QWidget*>(panel_)}) {
            widget->update();
        }
        for (QSplitter* splitter : {across_, down_}) {
            for (int i = 1; i < splitter->count(); ++i) {
                splitter->handle(i)->update();
            }
        }
    });
}

Workbench::~Workbench() = default;

void Workbench::set_editor_area(QWidget* editor_area) {
    editor_host_->layout()->addWidget(editor_area);
}

void Workbench::set_status_bar(QWidget* status_bar) {
    status_bar_ = status_bar;
    root_layout_->addWidget(status_bar);
}

void Workbench::set_title_bar(QWidget* title_bar) {
    title_bar_ = title_bar;
    root_layout_->insertWidget(0, title_bar);
}

QWidget* Workbench::activity_bar() const {
    return activity_bar_;
}

QWidget* Workbench::sidebar() const {
    return sidebar_;
}

QWidget* Workbench::panel() const {
    return panel_;
}

bool Workbench::is_visible(Part part) const {
    switch (part) {
    case Part::title_bar:
        return title_bar_ && !title_bar_->isHidden();
    case Part::activity_bar:
        return !activity_bar_->isHidden();
    case Part::sidebar:
        return !sidebar_->isHidden();
    case Part::editor:
        return !editor_host_->isHidden();
    case Part::panel:
        return !panel_->isHidden();
    case Part::status_bar:
        return status_bar_ && !status_bar_->isHidden();
    }
    return false;
}

void Workbench::set_visible(Part part, bool visible) {
    if (part == Part::editor || is_visible(part) == visible) {
        return;
    }
    if (part == Part::title_bar && !title_bar_) {
        return;
    }
    switch (part) {
    case Part::title_bar:
        title_bar_->setVisible(visible);
        break;
    case Part::activity_bar:
        activity_bar_->setVisible(visible);
        break;
    case Part::sidebar:
        if (visible) {
            sidebar_->show();
            apply_sizes();
        } else {
            if (sidebar_->isVisible()) {
                sidebar_size_ = sidebar_->width();
            }
            sidebar_->hide();
        }
        break;
    case Part::panel:
        if (visible) {
            panel_->show();
            apply_sizes();
        } else {
            if (panel_maximized_) {
                set_maximized(Part::panel, false);
            } else if (panel_->isVisible()) {
                panel_size() = panel_extent();
            }
            panel_->hide();
        }
        break;
    case Part::status_bar:
        status_bar_->setVisible(visible);
        break;
    case Part::editor:
        break;
    }
    emit visibility_changed(part, visible);
}

bool Workbench::is_maximized(Part part) const {
    return part == Part::panel && panel_maximized_;
}

void Workbench::set_maximized(Part part, bool maximized) {
    if (part != Part::panel || panel_maximized_ == maximized) {
        return;
    }
    if (maximized) {
        const bool was_shown = panel_->isVisible();
        set_visible(Part::panel, true);
        if (was_shown) {
            panel_size() = panel_extent();
        }
        panel_maximized_ = true;
        editor_host_->hide();
    } else {
        panel_maximized_ = false;
        editor_host_->show();
        apply_sizes();
    }
    emit maximized_changed(part, maximized);
}

void Workbench::toggle(Part part) {
    set_visible(part, !is_visible(part));
}

int Workbench::size(Part part) const {
    switch (part) {
    case Part::title_bar:
        return title_bar_ ? title_bar_->height() : 0;
    case Part::activity_bar:
        return activity_bar_->isHidden() ? 0 : activity_bar_->width();
    case Part::sidebar:
        return sidebar_->isVisible() ? sidebar_->width() : sidebar_size_;
    case Part::editor:
        return editor_host_->width();
    case Part::panel:
        return panel_->isVisible() ? panel_extent() : panel_size();
    case Part::status_bar:
        return status_bar_ ? status_bar_->height() : 0;
    }
    return 0;
}

void Workbench::set_size(Part part, int pixels) {
    if (part == Part::sidebar) {
        sidebar_size_ = std::max(pixels, sidebar_minimum());
        apply_sizes();
        emit sizes_changed();
    } else if (part == Part::panel) {
        set_panel_size_at(panel_position_, pixels);
        emit sizes_changed();
    }
}

int Workbench::panel_size_at(PanelPosition position) const {
    if (position == panel_position_ && panel_->isVisible() && !panel_maximized_) {
        return panel_extent();
    }
    return panel_sizes_[static_cast<std::size_t>(position)];
}

void Workbench::set_panel_size_at(PanelPosition position, int pixels) {
    const bool beside = position == PanelPosition::left || position == PanelPosition::right;
    panel_sizes_[static_cast<std::size_t>(position)] =
        std::max(pixels, zoomed(beside ? panel_minimum_width : panel_minimum_height));
    if (position == panel_position_) {
        apply_sizes();
    }
}

Side Workbench::sidebar_side() const {
    return sidebar_side_;
}

void Workbench::set_sidebar_side(Side side) {
    if (side == sidebar_side_) {
        return;
    }
    if (sidebar_->isVisible()) {
        sidebar_size_ = sidebar_->width();
    }
    sidebar_side_ = side;
    arrange();
    emit sidebar_side_changed(side);
}

PanelPosition Workbench::panel_position() const {
    return panel_position_;
}

void Workbench::set_panel_position(PanelPosition position) {
    if (position == panel_position_) {
        return;
    }
    if (panel_->isVisible() && !panel_maximized_) {
        panel_size() = panel_extent();
    }
    panel_position_ = position;
    arrange();
    emit panel_position_changed(position);
}

bool Workbench::panel_is_beside_editor() const {
    return panel_position_ == PanelPosition::left || panel_position_ == PanelPosition::right;
}

int Workbench::panel_extent() const {
    return panel_is_beside_editor() ? panel_->width() : panel_->height();
}

int& Workbench::panel_size() {
    return panel_sizes_[static_cast<std::size_t>(panel_position_)];
}

int Workbench::panel_size() const {
    return panel_sizes_[static_cast<std::size_t>(panel_position_)];
}

void Workbench::arrange() {
    const bool sidebar_last = sidebar_side_ == Side::right;
    across_->insertWidget(sidebar_last ? 1 : 0, sidebar_);
    across_->setStretchFactor(across_->indexOf(sidebar_), 0);
    across_->setStretchFactor(across_->indexOf(down_), 1);
    row_layout_->removeWidget(activity_bar_);
    row_layout_->insertWidget(sidebar_last ? 1 : 0, activity_bar_);
    // The line is on the side that faces the sidebar.
    activity_bar_->set_border("activityBar.border", sidebar_last ? Qt::LeftEdge : Qt::RightEdge);

    const bool beside = panel_is_beside_editor();
    const bool panel_first =
        panel_position_ == PanelPosition::top || panel_position_ == PanelPosition::left;
    down_->setOrientation(beside ? Qt::Horizontal : Qt::Vertical);
    down_->insertWidget(panel_first ? 0 : 1, panel_);
    down_->setStretchFactor(down_->indexOf(panel_), 0);
    down_->setStretchFactor(down_->indexOf(editor_host_), 1);
    apply_minimums();
    apply_sizes();
}

void Workbench::apply_sizes() {
    if (!sidebar_->isHidden()) {
        const int total = across_->width();
        const int rest = std::max(1, total - sidebar_size_);
        across_->setSizes(sidebar_side_ == Side::left ? QList<int>{sidebar_size_, rest}
                                                      : QList<int>{rest, sidebar_size_});
    }
    if (!panel_->isHidden() && !panel_maximized_) {
        const int total = panel_is_beside_editor() ? down_->width() : down_->height();
        const int rest = std::max(1, total - panel_size());
        const bool panel_first =
            panel_position_ == PanelPosition::top || panel_position_ == PanelPosition::left;
        down_->setSizes(panel_first ? QList<int>{panel_size(), rest}
                                    : QList<int>{rest, panel_size()});
    }
}

void Workbench::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    apply_sizes();
}

} // namespace anvil::ui
