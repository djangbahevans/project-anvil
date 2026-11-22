#include "anvil/ui/sidebar.hpp"

#include "anvil/ui/activity_bar.hpp"

#include <QHBoxLayout>
#include <QPainter>
#include <QVBoxLayout>

namespace anvil::ui {
namespace {

constexpr int title_height = 35;
constexpr int title_padding = 20;

} // namespace

// The title above the container: its name in capitals, and room for actions on the right.
class Sidebar::Title : public QWidget {
public:
    Title(const WorkbenchColors& colors, QWidget* parent) : QWidget(parent), colors_(colors) {
        setFixedHeight(title_height);
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(title_padding, 0, 8, 0);
        layout->addStretch(1);
        actions_ = new QWidget(this);
        auto* actions_layout = new QHBoxLayout(actions_);
        actions_layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(actions_);
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
        font.setPixelSize(11);
        font.setBold(true);
        painter.setFont(font);
        painter.setPen(colors_.color("sideBarTitle.foreground"));
        painter.drawText(QRect(title_padding, 0, width() - title_padding, height()),
                         Qt::AlignVCenter | Qt::AlignLeft, text_);
    }

private:
    const WorkbenchColors& colors_;
    QString text_;
    QWidget* actions_ = nullptr;
};

Sidebar::Sidebar(Workbench& workbench, ViewContainerRegistry& registry,
                 const WorkbenchColors& colors, QObject* parent)
    : QObject(parent), workbench_(workbench), registry_(registry) {
    auto* activity_layout = new QVBoxLayout(workbench_.activity_bar());
    activity_layout->setContentsMargins(0, 0, 0, 0);
    activity_bar_ = new ActivityBar(colors, workbench_.activity_bar());
    activity_layout->addWidget(activity_bar_);

    auto* content = new QVBoxLayout(workbench_.sidebar());
    content->setContentsMargins(0, 0, 0, 0);
    content->setSpacing(0);
    title_ = new Title(colors, workbench_.sidebar());
    title_actions_ = title_->actions();
    stack_ = std::make_unique<ViewContainerStack>(registry_, ViewContainerLocation::sidebar,
                                                  workbench_.sidebar());
    content->addWidget(title_);
    content->addWidget(stack_->widget(), 1);

    connect(activity_bar_, &ActivityBar::activated, this,
            [this](const QString& id) { choose(id.toStdString()); });
    connect(&registry_, &ViewContainerRegistry::changed, this, [this] { refresh(); });
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
    workbench_.set_visible(Part::activity_bar, !entries.empty());

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
