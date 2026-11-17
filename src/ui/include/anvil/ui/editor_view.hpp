#pragma once

#include "anvil/core/document.hpp"
#include "anvil/core/editing.hpp"

#include <QAbstractScrollArea>
#include <QElapsedTimer>
#include <QString>
#include <QTimer>
#include <memory>

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

    // Call after something other than this view changed the document or its selections.
    void document_changed_elsewhere();

    // The caret rectangle of byte `offset`, in viewport coordinates.
    QRect cursor_rect(std::size_t offset) const;
    // The byte offset nearest to a point in viewport coordinates.
    std::size_t offset_at(const QPoint& point) const;

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
    void move(core::Motion motion, bool extend);
    void move_by_page(bool down, bool extend);
    void update_metrics();
    void update_scroll_bars();
    void ensure_cursor_visible();
    void restart_caret_blink();
    void insert_tab();

    std::shared_ptr<core::Document> document_;
    std::unique_ptr<core::EditController> controller_;
    int line_height_ = 16;
    int char_width_ = 8;
    int max_line_width_ = 0;
    bool caret_visible_ = true;
    bool selecting_with_mouse_ = false;
    QTimer caret_timer_;
    QElapsedTimer since_double_click_;
    QString preedit_;
};

} // namespace anvil::ui
