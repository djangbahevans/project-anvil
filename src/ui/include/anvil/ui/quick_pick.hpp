#pragma once

#include "anvil/core/fuzzy.hpp"
#include "anvil/ui/workbench_colors.hpp"

#include <QFrame>
#include <QPointer>
#include <QString>
#include <cstddef>
#include <string>
#include <vector>

class QLabel;
class QLineEdit;
class QListView;

namespace anvil::ui {

struct QuickPickItem {
    QString label;
    // Shown after the label in a dimmer color, and matched like the label.
    QString description;
    // A second line under the label; not matched.
    QString detail;
    // Right-aligned, for the key binding of a command.
    QString keybinding;
    // Shown dimmed; choosing it does nothing and the popup stays open.
    bool disabled = false;
    // Not an item but a divider: with no filter, a line above the next item and `label` at the
    // right end of its row. It is never shown as a row of its own, is never chosen, and is left
    // out while a filter is typed, which ranks the items of every group together.
    bool separator = false;
};

// A popup list with a filter field, over the top center of a window: VS Code's quick pick, used
// for the command palette and for extensions' pick lists. The widget filters and ranks the items
// itself, with the fuzzy matcher of the core, and reports the one the user chooses by its position
// in the list that was set.
//
// With no filter text the items show in the order they were given. Up and Down move through the
// list and wrap around; Page Up and Page Down move a page; Enter or a click chooses; Escape, or
// the focus leaving the popup, closes it without a choice.
class QuickPick : public QFrame {
    Q_OBJECT

public:
    static constexpr int row_height = 22;
    // The list shows this many rows at most, fewer in a window that is not high enough.
    static constexpr int max_visible_rows = 20;

    // `window` is the window the popup overlays and the parent of the widget. The colors, which
    // must outlive the popup, come from the `quickInput`, `quickInputList`, `input` and `widget`
    // keys of `colors`, and the popup repaints when they change.
    QuickPick(QWidget* window, const WorkbenchColors& colors);
    ~QuickPick() override;

    // Replaces the items; the filter is applied again.
    void set_items(std::vector<QuickPickItem> items);
    void set_placeholder(const QString& text);
    // Shown instead of the list when the filter matches nothing.
    void set_empty_message(const QString& text);
    // A prefix of the filter text that is not matched, such as the `>` of the command palette.
    void set_ignored_prefix(const QString& prefix);
    // Whether the filter text selects and ranks the items. Without it every item shows in the
    // order it was given, for a list that the caller makes from the text itself. On by default.
    void set_filtering(bool enabled);
    // The distance of the popup from the top of the window, so that it sits below the menu bar.
    void set_top_offset(int pixels);

    // Shows the popup with `text` in the filter field and the first item chosen, and takes the
    // keyboard focus. The focus goes back to where it was when the popup closes.
    void open(const QString& text = {});
    // Makes the item, by its position in the list that was set, the chosen row, when the filter
    // shows it and it is not a divider.
    void set_active_item(std::size_t item);
    // Closes the popup without a choice.
    void cancel();

    // The items that were set, in the order they were given.
    const std::vector<QuickPickItem>& items() const {
        return items_;
    }
    QLineEdit* filter_field() const {
        return filter_;
    }
    QListView* list() const {
        return list_;
    }

    // The items that match the filter, best first, as positions in the list that was set.
    std::vector<std::size_t> shown_items() const;
    // The ranges of the label of the item shown in `row` that matched the filter, in UTF-16 code
    // units.
    std::vector<core::MatchRange> label_highlights(std::size_t row) const;

signals:
    // The chosen row moved to the item at this position of the list that was set: by the arrow
    // keys, the mouse, or because typing changed what is shown.
    void active_changed(std::size_t item);
    // The user chose the item at this position of the list that was set. The popup is closed.
    void accepted(std::size_t item);
    // The popup closed without a choice.
    void cancelled();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    class Model;
    class Delegate;
    class Shadow;
    struct Cached {
        std::string label;
        std::string description;
    };

    void refilter();
    void reposition();
    void choose(const QModelIndex& index);
    void move_selection(int rows, bool wrap);
    void close_popup(bool restore_focus);
    void on_focus_changed(QWidget* now);
    void apply_colors();
    void sync_shadow();

    const WorkbenchColors& colors_;
    QWidget* header_ = nullptr;
    QLineEdit* filter_ = nullptr;
    QListView* list_ = nullptr;
    Shadow* shadow_ = nullptr;
    QLabel* empty_ = nullptr;
    Model* model_ = nullptr;
    std::vector<QuickPickItem> items_;
    std::vector<Cached> cached_;
    QString ignored_prefix_;
    int top_offset_ = 0;
    bool has_detail_ = false;
    bool filtering_ = true;
    QPointer<QWidget> previous_focus_;
};

} // namespace anvil::ui
