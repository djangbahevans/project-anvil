#pragma once

#include "anvil/ui/view_container_registry.hpp"
#include "anvil/ui/view_container_stack.hpp"
#include "anvil/ui/workbench.hpp"
#include "anvil/ui/workbench_colors.hpp"

#include <QObject>
#include <memory>
#include <string>
#include <vector>

class QToolButton;

namespace anvil::ui {

// The bottom panel: a header with a tab for each view container registered for the panel, and a
// button to maximize the panel and one to close it, above the container the user chose.
//
// Choosing a tab shows that container. Closing hides the panel and keeps the container, so showing
// the panel again, from the View menu for instance, shows it; with none chosen yet it shows the
// first. Maximizing makes the panel fill the editor area, and restoring returns it to its size. A
// container's widget is made the first time it is shown, as in the sidebar.
class Panel : public QObject {
    Q_OBJECT

public:
    // The height of the header at zoom level 0.
    static constexpr int header_height = 35;

    // The header and the containers are filled in the workbench's panel. All the arguments must
    // outlive this object.
    Panel(Workbench& workbench, ViewContainerRegistry& registry, const WorkbenchColors& colors,
          QObject* parent = nullptr);
    ~Panel() override;

    // The id of the container that is shown, or was shown last; empty when none has been.
    const std::string& active_container() const {
        return active_;
    }
    // Shows the panel with the container. Nothing happens for an id that is not registered for the
    // panel.
    void activate(std::string_view id);
    // Makes the container the one that the panel shows when it is shown, without showing the
    // panel or making the container's widget while it is hidden. Nothing happens for an id that
    // is not registered for the panel.
    void set_active_container(std::string_view id);
    // Forgets the active container, so that the panel shows the first one when it is next shown.
    // For a hidden panel.
    void reset_active_container();

    // The widget of the container, or null before it was first shown.
    QWidget* container_widget(std::string_view id) const;

    // The tab of the container, or null; the ids of the tabs in the order they are shown.
    QToolButton* tab(const std::string& id) const;
    std::vector<std::string> tab_ids() const;
    // The title a tab shows, in capitals.
    QString tab_title(const std::string& id) const;
    // The height of the header at the zoom now.
    int header_size() const;
    QToolButton* maximize_button() const {
        return maximize_;
    }
    QToolButton* close_button() const {
        return close_;
    }

signals:
    // The active container changed. Empty when there is none left.
    void active_container_changed(const QString& id);

private:
    class Header;
    class Tab;
    class ActionButton;

    void refresh();
    void update_tabs();
    void update_maximize_button();

    Workbench& workbench_;
    ViewContainerRegistry& registry_;
    const WorkbenchColors& colors_;
    Header* header_ = nullptr;
    QToolButton* maximize_ = nullptr;
    QToolButton* close_ = nullptr;
    std::unique_ptr<ViewContainerStack> stack_;
    std::vector<Tab*> tabs_;
    std::string active_;
};

} // namespace anvil::ui
