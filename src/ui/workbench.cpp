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

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter(this).fillRect(rect(), colors_.color(background_));
    }

private:
    const WorkbenchColors& colors_;
    std::string_view background_;
};

void Workbench::set_zoom(const Zoom* zoom) {
    if (zoom_) {
        disconnect(zoom_, &Zoom::changed, this, nullptr);
    }
    zoom_ = zoom;
    const auto apply = [this] {
        activity_bar_->setFixedWidth(scaled_by(zoom_, activity_bar_width));
        apply_sizes();
    };
    if (zoom_) {
        connect(zoom_, &Zoom::changed, this, apply);
    }
    apply();
}

Workbench::Workbench(const WorkbenchColors& colors, QWidget* parent)
    : QWidget(parent), colors_(colors) {
    activity_bar_ = new PartWidget(colors_, "activityBar.background", this);
    activity_bar_->setFixedWidth(activity_bar_width);
    activity_bar_->hide();

    sidebar_ = new PartWidget(colors_, "sideBar.background", this);
    sidebar_->setMinimumWidth(sidebar_minimum_width);
    sidebar_->hide();
    panel_ = new PartWidget(colors_, "panel.background", this);
    panel_->setMinimumHeight(panel_minimum_height);
    panel_->hide();

    editor_host_ = new QWidget(this);
    editor_host_->setMinimumSize(editor_minimum_width, editor_minimum_height);
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
            sidebar_size_ = sidebar_->width();
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
            } else {
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
        const bool was_shown = !panel_->isHidden();
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
        return sidebar_->isHidden() ? sidebar_size_ : sidebar_->width();
    case Part::editor:
        return editor_host_->width();
    case Part::panel:
        return panel_->isHidden() ? panel_size() : panel_extent();
    case Part::status_bar:
        return status_bar_ ? status_bar_->height() : 0;
    }
    return 0;
}

void Workbench::set_size(Part part, int pixels) {
    if (part == Part::sidebar) {
        sidebar_size_ = std::max(pixels, sidebar_minimum_width);
    } else if (part == Part::panel) {
        panel_size() =
            std::max(pixels, panel_is_beside_editor() ? panel_minimum_width : panel_minimum_height);
    } else {
        return;
    }
    apply_sizes();
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

    const bool beside = panel_is_beside_editor();
    const bool panel_first =
        panel_position_ == PanelPosition::top || panel_position_ == PanelPosition::left;
    down_->setOrientation(beside ? Qt::Horizontal : Qt::Vertical);
    down_->insertWidget(panel_first ? 0 : 1, panel_);
    down_->setStretchFactor(down_->indexOf(panel_), 0);
    down_->setStretchFactor(down_->indexOf(editor_host_), 1);
    panel_->setMinimumSize(beside ? panel_minimum_width : 0, beside ? 0 : panel_minimum_height);

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
