#pragma once

#include "anvil/ui/painted_accessibility.hpp"
#include "anvil/ui/workbench_colors.hpp"

#include <QColor>
#include <QRect>
#include <QString>
#include <QTimer>
#include <QWidget>
#include <optional>
#include <string>
#include <vector>

namespace anvil::ui {

// A color of a status bar item: a workbench color key, or a fixed color when no key is given.
// Neither set means the color of the status bar itself.
struct StatusBarColor {
    // Resolved when the item is painted, so that a theme change reaches the item.
    std::string key = {};
    QColor color = {};

    bool operator==(const StatusBarColor&) const = default;
};

struct StatusBarItem {
    QString text = {};
    QString tooltip = {};
    StatusBarColor foreground = {};
    StatusBarColor background = {};
    // The command a click runs. Only an item with a command reacts to the mouse.
    std::string command = {};
    // Within a side, an item with a higher priority is further to the left; items with the same
    // priority keep the order in which they were added.
    int priority = 0;

    bool operator==(const StatusBarItem&) const = default;
};

enum class StatusBarSide { left, right };

// The strip along the bottom edge of the window: VS Code's status bar. It holds a model of items in
// a left and a right group, each with a text and optionally colors and a command, and paints them
// itself so that a change of one item costs one repaint and never a layout of widgets.
//
// A transient message replaces the left group until it times out or is cleared; the right group
// stays.
class StatusBar : public QWidget, public AccessibleItems {
    Q_OBJECT

public:
    static constexpr int height_pixels = 22;
    static constexpr int font_pixels = 12;
    // Space around the text inside an item, and between the edge of the bar or a neighbor and the
    // item. The highlight of an item covers the padding but not the margin.
    static constexpr int item_padding = 5;
    static constexpr int item_margin = 3;

    // The colors, which must outlive the widget, come from the `statusBar` and `statusBarItem`
    // keys. The status bar repaints when they change.
    StatusBar(const WorkbenchColors& colors, QWidget* parent = nullptr);

    // Adds the item, or replaces the one with this id, which moves to `side` if it was on the
    // other one. Setting an item to what it already is does no work.
    void set_item(const std::string& id, StatusBarSide side, StatusBarItem item);
    // Returns false when there is no such item.
    bool remove_item(const std::string& id);
    bool has_item(const std::string& id) const;
    const StatusBarItem* item(const std::string& id) const;
    // The ids of one side in the order they are shown, left to right.
    std::vector<std::string> item_ids(StatusBarSide side) const;
    // The area the item highlights, in the coordinates of the status bar; empty when there is no
    // such item or while a message hides it.
    QRect item_rect(const std::string& id) const;
    // The item with a command that is under the mouse.
    const std::string& hovered_item() const {
        return hovered_;
    }

    // Shows `text` in place of the left group, for `timeout_ms` milliseconds, or until it is
    // replaced or cleared when that is zero.
    void show_message(const QString& text, int timeout_ms = 0);
    void clear_message();
    const QString& message() const {
        return message_;
    }

    // The items as assistive technology reads them, in display order, left group then right group,
    // without those that a message hides: static text, or a button when the item has a command,
    // named by its text and described by its tooltip. A button can be pressed, which asks for its
    // command like a click does.
    std::vector<AccessibleItem> accessible_items() const override;
    void do_accessible_action(const QString& key, const QString& action) override;

signals:
    // The user clicked an item that has this command.
    void command_requested(const QString& command);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    bool event(QEvent* event) override;

private:
    struct Entry {
        std::string id;
        StatusBarSide side;
        StatusBarItem item;
        // Computed by the layout, which runs when the model or the width changed.
        mutable QRect rect;
    };

    std::vector<const Entry*> ordered(StatusBarSide side) const;
    void ensure_layout() const;
    const Entry* entry_at(const QPoint& position) const;
    bool shown(const Entry& entry) const;
    QColor resolve(const StatusBarColor& color, std::string_view fallback_key) const;
    void set_hovered(const std::string& id);
    // The position of the item among the accessible items, or -1.
    int accessible_index(const std::string& id) const;

    const WorkbenchColors& colors_;
    std::vector<Entry> entries_;
    mutable bool layout_valid_ = false;
    QString message_;
    QTimer message_timer_;
    std::string hovered_;
    std::string pressed_;
};

} // namespace anvil::ui
