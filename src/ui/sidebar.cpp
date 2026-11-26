#include "anvil/ui/sidebar.hpp"

#include "anvil/ui/activity_bar.hpp"

#include <QHBoxLayout>
#include <QPainter>
#include <QVBoxLayout>

namespace anvil::ui {
namespace {

constexpr int title_height_pixels = 35;
constexpr int title_padding = 20;
constexpr int title_right_padding = 8;
constexpr int title_font_pixels = 11;

} // namespace

// The title above the container: its name in capitals, and room for actions on the right.
class Sidebar::Title : public QWidget {
public:
    Title(const WorkbenchColors& colors, const Zoom* zoom, QWidget* parent)
        : QWidget(parent), colors_(colors), zoom_(zoom) {
        layout_ = new QHBoxLayout(this);
        layout_->addStretch(1);
        actions_ = new QWidget(this);
        auto* actions_layout = new QHBoxLayout(actions_);
        actions_layout->setContentsMargins(0, 0, 0, 0);
        layout_->addWidget(actions_);
        apply_zoom();
    }

    int height_now() const {
        return scaled_by(zoom_, title_height_pixels);
    }
    void apply_zoom() {
        setFixedHeight(height_now());
        layout_->setContentsMargins(scaled_by(zoom_, title_padding), 0,
                                    scaled_by(zoom_, title_right_padding), 0);
        update();
    }

    void set_text(const QString& text) {
        text_ = text.toUpper();
        update();
    }
    const QString& text() const {
        return text_;
    }
    QWidget* actions() const {
        return actions_;
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        QFont font = painter.font();
        font.setPixelSize(scaled_by(zoom_, title_font_pixels));
        font.setBold(true);
        painter.setFont(font);
        painter.setPen(colors_.color("sideBarTitle.foreground"));
        const int padding = scaled_by(zoom_, title_padding);
        painter.drawText(QRect(padding, 0, width() - padding, height()),
                         Qt::AlignVCenter | Qt::AlignLeft, text_);
    }

private:
    const WorkbenchColors& colors_;
    const Zoom* zoom_;
    QHBoxLayout* layout_ = nullptr;
    QString text_;
    QWidget* actions_ = nullptr;
};

int Sidebar::title_height() const {
    return title_->height_now();
}

Sidebar::Sidebar(Workbench& workbench, ViewContainerRegistry& registry,
                 const WorkbenchColors& colors, QObject* parent)
    : QObject(parent), workbench_(workbench), registry_(registry) {
    auto* activity_layout = new QVBoxLayout(workbench_.activity_bar());
    activity_layout->setContentsMargins(0, 0, 0, 0);
    activity_bar_ = new ActivityBar(colors, workbench_.activity_bar());
    activity_bar_->set_zoom(workbench_.zoom());
    activity_layout->addWidget(activity_bar_);

    auto* content = new QVBoxLayout(workbench_.sidebar());
    content->setContentsMargins(0, 0, 0, 0);
    content->setSpacing(0);
    title_ = new Title(colors, workbench_.zoom(), workbench_.sidebar());
    if (workbench_.zoom()) {
        connect(workbench_.zoom(), &Zoom::changed, title_, [this] { title_->apply_zoom(); });
    }
    title_actions_ = title_->actions();
    stack_ = std::make_unique<ViewContainerStack>(registry_, ViewContainerLocation::sidebar,
                                                  workbench_.sidebar());
    content->addWidget(title_);
    content->addWidget(stack_->widget(), 1);

    connect(activity_bar_, &ActivityBar::activated, this,
            [this](const QString& id) { choose(id.toStdString()); });
    connect(&registry_, &ViewContainerRegistry::changed, this, [this] { refresh(); });
    activity_bar_->set_side(workbench_.sidebar_side());
    connect(&workbench_, &Workbench::sidebar_side_changed, this,
            [this](Side side) { activity_bar_->set_side(side); });
    connect(&workbench_, &Workbench::visibility_changed, this, [this](Part part, bool visible) {
        if (part != Part::sidebar) {
            return;
        }
        if (visible && active_.empty()) {
            if (const auto containers = registry_.containers(ViewContainerLocation::sidebar);
                !containers.empty()) {
                activate(containers.front()->id);
            }
        }
        update_buttons();
    });
    refresh();
}

Sidebar::~Sidebar() = default;

void Sidebar::activate(std::string_view id) {
    const ViewContainer* container = stack_->show(id);
    if (!container) {
        return;
    }
    title_->set_text(container->title);

    const bool changed = active_ != container->id;
    active_ = container->id;
    workbench_.set_visible(Part::sidebar, true);
    update_buttons();
    if (changed) {
        emit active_container_changed(QString::fromStdString(active_));
    }
}

QString Sidebar::title() const {
    return title_->text();
}

QWidget* Sidebar::container_widget(std::string_view id) const {
    return stack_->container_widget(id);
}

void Sidebar::choose(const std::string& id) {
    if (id == active_ && workbench_.is_visible(Part::sidebar)) {
        workbench_.set_visible(Part::sidebar, false);
        return;
    }
    activate(id);
}

void Sidebar::set_activity_bar_allowed(bool allowed) {
    if (allowed == activity_bar_allowed_) {
        return;
    }
    activity_bar_allowed_ = allowed;
    refresh();
}

void Sidebar::update_buttons() {
    activity_bar_->set_active(workbench_.is_visible(Part::sidebar) ? active_ : std::string());
}

void Sidebar::refresh() {
    const auto containers = registry_.containers(ViewContainerLocation::sidebar);

    std::vector<ActivityBar::Entry> entries;
    for (const ViewContainer* container : containers) {
        entries.push_back({container->id, container->title, container->icon});
    }
    activity_bar_->set_entries(entries);
    workbench_.set_visible(Part::activity_bar, activity_bar_allowed_ && !entries.empty());

    // A container that was removed, or moved to the panel, leaves with its widget.
    stack_->prune();

    const ViewContainer* active = registry_.find(active_);
    if (!active_.empty() && (!active || active->location != ViewContainerLocation::sidebar)) {
        active_.clear();
        emit active_container_changed({});
        if (entries.empty()) {
            workbench_.set_visible(Part::sidebar, false);
        } else if (workbench_.is_visible(Part::sidebar)) {
            activate(entries.front().id);
        }
    } else if (active) {
        title_->set_text(active->title);
    }
    update_buttons();
}

} // namespace anvil::ui
