#pragma once

#include "anvil/core/document.hpp"
#include "anvil/core/editing.hpp"
#include "anvil/core/editor_settings.hpp"
#include "anvil/core/search.hpp"
#include "anvil/ui/workbench_colors.hpp"

#include <QAbstractScrollArea>
#include <QElapsedTimer>
#include <QString>
#include <QTimer>
#include <memory>
#include <optional>
#include <vector>

namespace anvil::ui {

// Shows and edits a core::Document: painted text with line numbers, carets and selections,
// scrolling, and keyboard, mouse and input method handling. All editing behavior lives in
// core::EditController; the view translates events into controller commands and paints the result.
//
// Only the visible lines are laid out, so documents of any size scroll smoothly. Lines longer than
// `max_line_characters` are shown cut off.
class EditorView : public QAbstractScrollArea {
    Q_OBJECT

public:
    static constexpr int max_line_characters = 5000;

    explicit EditorView(QWidget* parent = nullptr);
    ~EditorView() override;

    // Takes the font, tab size and tab key behavior from the settings. A font family that is not
    // installed falls back to the system's fixed-width font. The text, caret and scroll position
    // stay as they are, so this can run while the user edits.
    void apply_settings(const core::EditorSettings& settings);

    // Takes the colors of the editor from the workbench colors, which must outlive the view, and
    // repaints when they change; null returns to the palette of the widget. The keys read are
    // `editor.background`, `editor.foreground`, `editor.selectionBackground`,
    // `editor.lineHighlightBackground`, `editorLineNumber.foreground`,
    // `editorLineNumber.activeForeground`, `editorCursor.foreground`, `editorGutter.background`,
    // `editor.findMatchBackground` and `editor.findMatchHighlightBackground`. Text under a
    // selection keeps its color when the theme sets the selection's background, since that color
    // may be translucent; otherwise it takes the palette's highlighted text color.
    void set_colors(const WorkbenchColors* colors);

    // Shows `document`, or nothing when null. The document is shared with its other users.
    void set_document(std::shared_ptr<core::Document> document);
    const std::shared_ptr<core::Document>& document() const {
        return document_;
    }

    // Editing commands, also reachable through menus.
    void undo();
    void redo();
    void cut();
    void copy();
    void paste();
    void select_all();
    // Moves every caret, or extends every selection when `extend` is set.
    void move(core::Motion motion, bool extend = false);
    // Deletes the selection, or the character before or after each caret.
    void delete_backward();
    void delete_forward();

    // Call after something other than this view changed the document or its selections.
    void document_changed_elsewhere();

    // Makes `range` the only selection and scrolls to show it.
    void select_range(core::Selection range);
    // Applies the edits as one undo step, as the user's own change: the window is told through
    // `text_changed`. See EditController::replace_ranges.
    void replace_ranges(std::vector<core::Edit> edits,
                        std::optional<core::Selection> selection_after = std::nullopt);

    // Shades the given byte ranges, which must be ordered and must not overlap, in a color that
    // differs from the selection. Empty ranges are not shown.
    void set_highlights(std::vector<core::Match> highlights);

    // The caret rectangle of byte `offset`, in viewport coordinates.
    QRect cursor_rect(std::size_t offset) const;
    // The byte offset nearest to a point in viewport coordinates.
    std::size_t offset_at(const QPoint& point) const;

    // True while an input method composition is shown, which owns the keys it is using.
    bool composing() const {
        return !preedit_.isEmpty();
    }

    int line_height() const {
        return line_height_;
    }
    int char_width() const {
        return char_width_;
    }

signals:
    // The user changed the text: typing, deleting, pasting, undo or redo.
    void text_changed(const anvil::core::DocumentChange& change);
    // The selections changed, whether or not the text did.
    void selections_changed();
    // The text shown may differ from before: the user or someone else edited it, or another
    // document is shown.
    void content_changed();

protected:
    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    void changeEvent(QEvent* event) override;

private:
    struct LineLayout;

    std::unique_ptr<LineLayout> layout_line(std::size_t line) const;
    int gutter_width() const;
    int text_origin_x() const;
    void apply(const core::EditOutcome& outcome);
    void move_by_page(bool down, bool extend);
    void update_metrics();
    void update_scroll_bars();
    void ensure_cursor_visible();
    void restart_caret_blink();
    void insert_tab();

    struct Colors;
    Colors current_colors() const;

    const WorkbenchColors* colors_ = nullptr;
    QMetaObject::Connection colors_connection_;
    std::shared_ptr<core::Document> document_;
    std::unique_ptr<core::EditController> controller_;
    int line_height_ = 16;
    int char_width_ = 8;
    std::size_t tab_size_ = 4;
    bool insert_spaces_ = true;
    int max_line_width_ = 0;
    bool caret_visible_ = true;
    bool selecting_with_mouse_ = false;
    QTimer caret_timer_;
    QElapsedTimer since_double_click_;
    QString preedit_;
    std::vector<core::Match> highlights_;
};

} // namespace anvil::ui
