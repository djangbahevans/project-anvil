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

QFont title_font(QFont font) {
    font.setPixelSize(11);
    return font;
}

} // namespace

// A tab of the header: the container's title in capitals, with a line under the active one.
class Panel::Tab : public QToolButton {
public:
    Tab(const WorkbenchColors& colors, std::string id, const QString& title, QWidget* parent)
        : QToolButton(parent), colors_(colors), id_(std::move(id)), title_(title.toUpper()) {
        setCheckable(true);
        setAutoRaise(true);
        setFocusPolicy(Qt::TabFocus);
        setText(title);
        setToolTip(title);
        setAccessibleName(title);
        setFixedHeight(header_height);
        setFixedWidth(QFontMetrics(title_font(font())).horizontalAdvance(title_) + 2 * tab_padding);
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
        painter.setFont(title_font(font()));
        painter.setPen(colors_.color(active || underMouse() || hasFocus()
                                         ? "panelTitle.activeForeground"
                                         : "panelTitle.inactiveForeground"));
        painter.drawText(rect(), Qt::AlignCenter, title_);
        if (active) {
            painter.fillRect(QRect(tab_padding, height() - 1, width() - 2 * tab_padding, 1),
                             colors_.color("panelTitle.activeBorder"));
        }
    }

private:
    const WorkbenchColors& colors_;
    std::string id_;
    QString title_;
};

// A small icon button on the right of the header, drawn with lines.
class Panel::ActionButton : public QToolButton {
public:
    enum class Icon { close, chevron_up, chevron_down };

    ActionButton(const WorkbenchColors& colors, QWidget* parent)
        : QToolButton(parent), colors_(colors) {
        setAutoRaise(true);
        setFocusPolicy(Qt::TabFocus);
        setFixedSize(action_size, action_size);
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
        pen.setWidthF(1.4);
        pen.setCapStyle(Qt::RoundCap);
        painter.setPen(pen);
        const QPointF c(width() / 2.0, height() / 2.0);
        switch (icon_) {
        case Icon::close:
            painter.drawLine(c + QPointF(-4, -4), c + QPointF(4, 4));
            painter.drawLine(c + QPointF(-4, 4), c + QPointF(4, -4));
            break;
        case Icon::chevron_up:
            painter.drawPolyline(
                QPolygonF({c + QPointF(-5, 2.5), c + QPointF(0, -2.5), c + QPointF(5, 2.5)}));
            break;
        case Icon::chevron_down:
            painter.drawPolyline(
                QPolygonF({c + QPointF(-5, -2.5), c + QPointF(0, 2.5), c + QPointF(5, -2.5)}));
            break;
        }
    }

private:
    const WorkbenchColors& colors_;
    Icon icon_ = Icon::close;
};

class Panel::Header : public QWidget {
public:
    explicit Header(QWidget* parent) : QWidget(parent) {
        setFixedHeight(header_height);
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(tab_padding, 0, 4, 0);
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
    }

    void add_tab(QWidget* tab) {
        tabs_->addWidget(tab);
    }
    void add_action(QWidget* action) {
        actions_->addWidget(action);
    }

private:
    QHBoxLayout* tabs_ = nullptr;
    QHBoxLayout* actions_ = nullptr;
};

Panel::Panel(Workbench& workbench, ViewContainerRegistry& registry, const WorkbenchColors& colors,
             QObject* parent)
    : QObject(parent), workbench_(workbench), registry_(registry), colors_(colors) {
    auto* content = new QVBoxLayout(workbench_.panel());
    content->setContentsMargins(0, 0, 0, 0);
    content->setSpacing(0);
    header_ = new Header(workbench_.panel());
    stack_ = std::make_unique<ViewContainerStack>(registry_, ViewContainerLocation::panel,
                                                  workbench_.panel());
    content->addWidget(header_);
    content->addWidget(stack_->widget(), 1);

    auto* maximize = new ActionButton(colors_, header_);
    auto* close = new ActionButton(colors_, header_);
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
        auto* tab = new Tab(colors_, container->id, container->title, header_);
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
