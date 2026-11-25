#include "anvil/ui/panel.hpp"

#include <QHBoxLayout>
#include <QPainter>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

namespace anvil::ui {
namespace {

constexpr int tab_padding = 10;
constexpr int action_size = 28;
constexpr int header_right_padding = 4;
constexpr int title_font_pixels = 11;
constexpr double action_pen_width = 1.4;

QFont title_font(QFont font, const Zoom* zoom) {
    font.setPixelSize(scaled_by(zoom, title_font_pixels));
    return font;
}

} // namespace

// A tab of the header: the container's title in capitals, with a line under the active one.
class Panel::Tab : public QToolButton {
public:
    Tab(const WorkbenchColors& colors, const Zoom* zoom, std::string id, const QString& title,
        QWidget* parent)
        : QToolButton(parent), colors_(colors), zoom_(zoom), id_(std::move(id)),
          title_(title.toUpper()) {
        setCheckable(true);
        setAutoRaise(true);
        setFocusPolicy(Qt::TabFocus);
        setText(title);
        setToolTip(title);
        setAccessibleName(title);
        apply_zoom();
    }

    void apply_zoom() {
        setFixedHeight(scaled_by(zoom_, header_height));
        setFixedWidth(QFontMetrics(title_font(font(), zoom_)).horizontalAdvance(title_) +
                      2 * scaled_by(zoom_, tab_padding));
        update();
    }

    const std::string& id() const {
        return id_;
    }
    const QString& title() const {
        return title_;
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        const bool active = isChecked();
        painter.setFont(title_font(font(), zoom_));
        painter.setPen(colors_.color(active || underMouse() || hasFocus()
                                         ? "panelTitle.activeForeground"
                                         : "panelTitle.inactiveForeground"));
        painter.drawText(rect(), Qt::AlignCenter, title_);
        if (active) {
            const int padding = scaled_by(zoom_, tab_padding);
            painter.fillRect(QRect(padding, height() - 1, width() - 2 * padding, 1),
                             colors_.color("panelTitle.activeBorder"));
        }
    }

private:
    const WorkbenchColors& colors_;
    const Zoom* zoom_;
    std::string id_;
    QString title_;
};

// A small icon button on the right of the header, drawn with lines.
class Panel::ActionButton : public QToolButton {
public:
    enum class Icon { close, chevron_up, chevron_down };

    ActionButton(const WorkbenchColors& colors, const Zoom* zoom, QWidget* parent)
        : QToolButton(parent), colors_(colors), zoom_(zoom) {
        setAutoRaise(true);
        setFocusPolicy(Qt::TabFocus);
        apply_zoom();
    }

    void apply_zoom() {
        setFixedSize(scaled_by(zoom_, action_size), scaled_by(zoom_, action_size));
        update();
    }

    void set_icon(Icon icon) {
        icon_ = icon;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QPen pen(colors_.color("panelTitle.activeForeground"));
        const double factor = zoom_ ? zoom_->factor() : 1.0;
        pen.setWidthF(action_pen_width * factor);
        pen.setCapStyle(Qt::RoundCap);
        painter.setPen(pen);
        const QPointF c(width() / 2.0, height() / 2.0);
        // The glyphs are drawn in units of the level 0 size.
        const auto at = [&](double x, double y) { return c + QPointF(x * factor, y * factor); };
        switch (icon_) {
        case Icon::close:
            painter.drawLine(at(-4, -4), at(4, 4));
            painter.drawLine(at(-4, 4), at(4, -4));
            break;
        case Icon::chevron_up:
            painter.drawPolyline(QPolygonF({at(-5, 2.5), at(0, -2.5), at(5, 2.5)}));
            break;
        case Icon::chevron_down:
            painter.drawPolyline(QPolygonF({at(-5, -2.5), at(0, 2.5), at(5, -2.5)}));
            break;
        }
    }

private:
    const WorkbenchColors& colors_;
    const Zoom* zoom_;
    Icon icon_ = Icon::close;
};

class Panel::Header : public QWidget {
public:
    Header(const Zoom* zoom, QWidget* parent) : QWidget(parent), zoom_(zoom) {
        layout_ = new QHBoxLayout(this);
        auto* layout = layout_;
        layout->setSpacing(0);
        tabs_ = new QHBoxLayout;
        tabs_->setContentsMargins(0, 0, 0, 0);
        tabs_->setSpacing(0);
        layout->addLayout(tabs_);
        layout->addStretch(1);
        actions_ = new QHBoxLayout;
        actions_->setContentsMargins(0, 0, 0, 0);
        actions_->setSpacing(0);
        layout->addLayout(actions_);
        apply_zoom();
    }

    void apply_zoom() {
        setFixedHeight(scaled_by(zoom_, header_height));
        layout_->setContentsMargins(scaled_by(zoom_, tab_padding), 0,
                                    scaled_by(zoom_, header_right_padding), 0);
    }

    void add_tab(QWidget* tab) {
        tabs_->addWidget(tab);
    }
    void add_action(QWidget* action) {
        actions_->addWidget(action);
    }

private:
    const Zoom* zoom_;
    QHBoxLayout* layout_ = nullptr;
    QHBoxLayout* tabs_ = nullptr;
    QHBoxLayout* actions_ = nullptr;
};

Panel::Panel(Workbench& workbench, ViewContainerRegistry& registry, const WorkbenchColors& colors,
             QObject* parent)
    : QObject(parent), workbench_(workbench), registry_(registry), colors_(colors) {
    auto* content = new QVBoxLayout(workbench_.panel());
    content->setContentsMargins(0, 0, 0, 0);
    content->setSpacing(0);
    header_ = new Header(workbench_.zoom(), workbench_.panel());
    stack_ = std::make_unique<ViewContainerStack>(registry_, ViewContainerLocation::panel,
                                                  workbench_.panel());
    content->addWidget(header_);
    content->addWidget(stack_->widget(), 1);

    auto* maximize = new ActionButton(colors_, workbench_.zoom(), header_);
    auto* close = new ActionButton(colors_, workbench_.zoom(), header_);
    close->set_icon(ActionButton::Icon::close);
    close->setToolTip(tr("Close Panel"));
    close->setAccessibleName(tr("Close Panel"));
    header_->add_action(maximize);
    header_->add_action(close);
    maximize_ = maximize;
    close_ = close;
    connect(close_, &QToolButton::clicked, this,
            [this] { workbench_.set_visible(Part::panel, false); });
    connect(maximize_, &QToolButton::clicked, this, [this] {
        workbench_.set_maximized(Part::panel, !workbench_.is_maximized(Part::panel));
    });
    update_maximize_button();

    connect(&registry_, &ViewContainerRegistry::changed, this, [this] { refresh(); });
    connect(&workbench_, &Workbench::visibility_changed, this, [this](Part part, bool visible) {
        if (part != Part::panel) {
            return;
        }
        if (visible && active_.empty()) {
            if (const auto containers = registry_.containers(ViewContainerLocation::panel);
                !containers.empty()) {
                activate(containers.front()->id);
            }
        }
    });
    connect(&workbench_, &Workbench::maximized_changed, this, [this](Part part, bool) {
        if (part == Part::panel) {
            update_maximize_button();
        }
    });
    if (workbench_.zoom()) {
        connect(workbench_.zoom(), &Zoom::changed, this, [this] {
            header_->apply_zoom();
            static_cast<ActionButton*>(maximize_)->apply_zoom();
            static_cast<ActionButton*>(close_)->apply_zoom();
            for (Tab* tab : tabs_) {
                tab->apply_zoom();
            }
        });
    }
    connect(&colors_, &WorkbenchColors::changed, this, [this] {
        maximize_->update();
        close_->update();
        for (Tab* tab : tabs_) {
            tab->update();
        }
    });
    refresh();
}

Panel::~Panel() = default;

int Panel::header_size() const {
    return header_->height();
}

void Panel::activate(std::string_view id) {
    const ViewContainer* container = stack_->show(id);
    if (!container) {
        return;
    }
    const bool changed = active_ != container->id;
    active_ = container->id;
    workbench_.set_visible(Part::panel, true);
    update_tabs();
    if (changed) {
        emit active_container_changed(QString::fromStdString(active_));
    }
}

QWidget* Panel::container_widget(std::string_view id) const {
    return stack_->container_widget(id);
}

QToolButton* Panel::tab(const std::string& id) const {
    const auto found = std::ranges::find(tabs_, id, &Tab::id);
    return found == tabs_.end() ? nullptr : *found;
}

QString Panel::tab_title(const std::string& id) const {
    const auto found = std::ranges::find(tabs_, id, &Tab::id);
    return found == tabs_.end() ? QString() : (*found)->title();
}

std::vector<std::string> Panel::tab_ids() const {
    std::vector<std::string> ids;
    for (const Tab* tab : tabs_) {
        ids.push_back(tab->id());
    }
    return ids;
}

void Panel::update_tabs() {
    for (Tab* tab : tabs_) {
        tab->setChecked(tab->id() == active_);
    }
}

void Panel::update_maximize_button() {
    const bool maximized = workbench_.is_maximized(Part::panel);
    static_cast<ActionButton*>(maximize_)->set_icon(maximized ? ActionButton::Icon::chevron_down
                                                              : ActionButton::Icon::chevron_up);
    const QString text = maximized ? tr("Restore Panel Size") : tr("Maximize Panel Size");
    maximize_->setToolTip(text);
    maximize_->setAccessibleName(text);
}

void Panel::refresh() {
    const auto containers = registry_.containers(ViewContainerLocation::panel);

    for (Tab* tab : tabs_) {
        delete tab;
    }
    tabs_.clear();
    for (const ViewContainer* container : containers) {
        auto* tab = new Tab(colors_, workbench_.zoom(), container->id, container->title, header_);
        header_->add_tab(tab);
        tabs_.push_back(tab);
        connect(tab, &QToolButton::clicked, this, [this, id = container->id] { activate(id); });
    }

    stack_->prune();

    const ViewContainer* active = registry_.find(active_);
    if (!active_.empty() && (!active || active->location != ViewContainerLocation::panel)) {
        active_.clear();
        emit active_container_changed({});
        if (containers.empty()) {
            workbench_.set_visible(Part::panel, false);
        } else if (workbench_.is_visible(Part::panel)) {
            activate(containers.front()->id);
        }
    }
    update_tabs();
}

} // namespace anvil::ui
