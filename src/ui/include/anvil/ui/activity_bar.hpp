#pragma once

#include "anvil/ui/workbench.hpp"
#include "anvil/ui/workbench_colors.hpp"

#include <QIcon>
#include <QString>
#include <QWidget>
#include <string>
#include <vector>

class QToolButton;
class QVBoxLayout;

namespace anvil::ui {

// The column of icon buttons at the left edge or the right edge: one for each view container of
// the sidebar. The button of the active container shows an indicator on the outer edge and the icon
// in the foreground color; the others use the inactive color until the mouse is over them.
class ActivityBar : public QWidget {
    Q_OBJECT

public:
    static constexpr int button_size = 48;
    static constexpr int icon_size = 24;

    struct Entry {
        std::string id;
        QString title;
        QIcon icon;
    };

    ActivityBar(const WorkbenchColors& colors, QWidget* parent);

    // Replaces the buttons.
    void set_entries(const std::vector<Entry>& entries);
    // The edge of the window the bar is at, where the indicator of the active button is drawn.
    void set_side(Side side);
    Side side() const {
        return side_;
    }
    // Shows the button of this container as the active one, or none for an empty id.
    void set_active(const std::string& id);

    std::size_t button_count() const {
        return buttons_.size();
    }
    // The button for the container, or null.
    QToolButton* button(const std::string& id) const;
    // The ids in the order the buttons are.
    std::vector<std::string> ids() const;

signals:
    // The user clicked the button of this container.
    void activated(const QString& id);

private:
    class Button;

    const WorkbenchColors& colors_;
    QVBoxLayout* layout_ = nullptr;
    std::vector<Button*> buttons_;
    Side side_ = Side::left;
};

} // namespace anvil::ui
