#include "anvil/ui/workbench.hpp"

#include <QEnterEvent>
#include <QHBoxLayout>
#include <QPainter>
#include <QSplitter>
#include <QSplitterHandle>
#include <QVBoxLayout>
#include <algorithm>

namespace anvil::ui {
namespace {

constexpr int sash_width = 4;

} // namespace

std::string_view part_id(Part part) {
    switch (part) {
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
    for (const Part part :
         {Part::activity_bar, Part::sidebar, Part::editor, Part::panel, Part::status_bar}) {
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

namespace {

// The handle between two parts: a 4 px hit area that shows a 1 px line of the border color, and the
// whole of it in the sash color while it is hovered or dragged.
class Sash : public QSplitterHandle {
public:
    Sash(Qt::Orientation orientation, const WorkbenchColors& colors, std::string_view border,
         QSplitter* parent)
        : QSplitterHandle(orientation, parent), colors_(colors), border_(border) {}

protected:
    void enterEvent(QEnterEvent* event) override {
        QSplitterHandle::enterEvent(event);
        hovered_ = true;
        update();
    }
    void leaveEvent(QEvent* event) override {
        QSplitterHandle::leaveEvent(event);
        hovered_ = false;
        update();
    }
    void mousePressEvent(QMouseEvent* event) override {
        QSplitterHandle::mousePressEvent(event);
        pressed_ = true;
        update();
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        QSplitterHandle::mouseReleaseEvent(event);
        pressed_ = false;
        update();
    }
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        if (hovered_ || pressed_) {
            painter.fillRect(rect(), colors_.color("sash.hoverBorder"));
            return;
        }
        const QColor line = colors_.color(border_);
        const int middle = orientation() == Qt::Horizontal ? width() / 2 : height() / 2;
        if (orientation() == Qt::Horizontal) {
            painter.fillRect(QRect(middle, 0, 1, height()), line);
        } else {
            painter.fillRect(QRect(0, middle, width(), 1), line);
        }
    }

private:
    const WorkbenchColors& colors_;
    std::string_view border_;
    bool hovered_ = false;
    bool pressed_ = false;
};

} // namespace

class Workbench::Splitter : public QSplitter {
public:
    Splitter(Qt::Orientation orientation, const WorkbenchColors& colors, std::string_view border,
             QWidget* parent)
        : QSplitter(orientation, parent), colors_(colors), border_(border) {
        setHandleWidth(sash_width);
        setChildrenCollapsible(false);
    }

protected:
    QSplitterHandle* createHandle() override {
        return new Sash(orientation(), colors_, border_, this);
    }

private:
    const WorkbenchColors& colors_;
    std::string_view border_;
};

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
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(activity_bar_);
    layout->addWidget(across_, 1);

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
    switch (part) {
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
                panel_size_ = panel_->height();
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
            panel_size_ = panel_->height();
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
    case Part::activity_bar:
        return activity_bar_->isHidden() ? 0 : activity_bar_width;
    case Part::sidebar:
        return sidebar_->isHidden() ? sidebar_size_ : sidebar_->width();
    case Part::editor:
        return editor_host_->width();
    case Part::panel:
        return panel_->isHidden() ? panel_size_ : panel_->height();
    case Part::status_bar:
        return status_bar_ ? status_bar_->height() : 0;
    }
    return 0;
}

void Workbench::set_size(Part part, int pixels) {
    if (part == Part::sidebar) {
        sidebar_size_ = std::max(pixels, sidebar_minimum_width);
    } else if (part == Part::panel) {
        panel_size_ = std::max(pixels, panel_minimum_height);
    } else {
        return;
    }
    apply_sizes();
}

void Workbench::apply_sizes() {
    if (!sidebar_->isHidden()) {
        const int total = across_->width();
        across_->setSizes({sidebar_size_, std::max(1, total - sidebar_size_)});
    }
    if (!panel_->isHidden() && !panel_maximized_) {
        const int total = down_->height();
        down_->setSizes({std::max(1, total - panel_size_), panel_size_});
    }
}

void Workbench::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    apply_sizes();
}

} // namespace anvil::ui
