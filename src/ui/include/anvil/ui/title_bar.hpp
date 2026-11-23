#pragma once

#include "anvil/ui/workbench_colors.hpp"

#include <QPointer>
#include <QString>
#include <QWidget>

class QAbstractButton;
class QHBoxLayout;

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

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    class ControlButton;

    void style_menu_bar();

    const WorkbenchColors& colors_;
    QString title_;
    bool active_ = true;
    QHBoxLayout* layout_ = nullptr;
    QAbstractButton* minimize_ = nullptr;
    QAbstractButton* maximize_ = nullptr;
    QAbstractButton* close_ = nullptr;
    QWidget* icon_ = nullptr;
    QPointer<QWidget> menu_bar_;
};

} // namespace anvil::ui
