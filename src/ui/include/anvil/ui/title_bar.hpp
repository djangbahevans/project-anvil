#pragma once

#include "anvil/ui/workbench_colors.hpp"

#include <QPointer>
#include <QString>
#include <QWidget>

class QAbstractButton;
class QHBoxLayout;
class QResizeEvent;

namespace anvil::ui {

// The top row of a window that has no frame of its own (ADR 0015): the application icon, the menu
// bar, an empty area that shows the window title and moves the window, and the minimize, maximize
// and close buttons. It decides nothing about the window; each action is a signal that the window
// answers.
class TitleBar : public QWidget {
    Q_OBJECT

public:
    static constexpr int bar_height = 35;
    static constexpr int button_width = 46;

    // The colors, which must outlive the widget, come from the `titleBar` and `toolbar` keys.
    explicit TitleBar(const WorkbenchColors& colors, QWidget* parent = nullptr);

    // The title is drawn in the middle of the area between the menu bar and the buttons.
    void set_title(const QString& title);
    const QString& title() const {
        return title_;
    }

    // The widget that follows the icon, which the title bar lays out and does not own.
    void set_menu_bar(QWidget* menu_bar);

    // Whether the window is the one that has the focus; an inactive window uses the inactive
    // colors.
    void set_active(bool active);
    // Shows the restore icon in place of the maximize icon.
    void set_maximized(bool maximized);

    // The command center is a box in the middle of the bar that shows `text`, the name of the
    // workspace, and asks for the command palette when it is clicked. It replaces the title, which
    // is drawn only while there is no command center, and is left out while the bar is too narrow
    // for it.
    void set_command_center_visible(bool visible);
    bool command_center_visible() const {
        return command_center_wanted_;
    }
    void set_command_center_text(const QString& text);
    QAbstractButton* command_center() const {
        return command_center_;
    }

    // The two buttons before the window buttons that show and hide the primary side bar and the
    // panel. Each shows by its icon whether its part is shown, and asks for the change.
    void set_layout_controls_visible(bool visible);
    bool layout_controls_visible() const {
        return layout_controls_wanted_;
    }
    void set_sidebar_shown(bool shown);
    void set_panel_shown(bool shown);
    QAbstractButton* sidebar_button() const {
        return sidebar_button_;
    }
    QAbstractButton* panel_button() const {
        return panel_button_;
    }

    QAbstractButton* minimize_button() const {
        return minimize_;
    }
    QAbstractButton* maximize_button() const {
        return maximize_;
    }
    QAbstractButton* close_button() const {
        return close_;
    }

signals:
    // The left button went down on the empty area: the window should follow the pointer.
    void move_requested();
    // A double click on the empty area.
    void toggle_maximized_requested();
    void minimize_requested();
    void close_requested();
    void command_center_requested();
    void toggle_sidebar_requested();
    void toggle_panel_requested();

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    class ControlButton;
    class LayoutButton;
    class CommandCenter;

    void style_menu_bar();
    void place_command_center();
    // Where the buttons on the right begin.
    int buttons_left() const;

    const WorkbenchColors& colors_;
    QString title_;
    bool active_ = true;
    QHBoxLayout* layout_ = nullptr;
    QAbstractButton* minimize_ = nullptr;
    QAbstractButton* maximize_ = nullptr;
    QAbstractButton* close_ = nullptr;
    QAbstractButton* sidebar_button_ = nullptr;
    QAbstractButton* panel_button_ = nullptr;
    QAbstractButton* command_center_ = nullptr;
    QWidget* layout_spacer_ = nullptr;
    bool command_center_wanted_ = false;
    bool layout_controls_wanted_ = false;
    QWidget* icon_ = nullptr;
    QPointer<QWidget> menu_bar_;
};

} // namespace anvil::ui
