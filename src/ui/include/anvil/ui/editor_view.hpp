#pragma once

#include "anvil/core/document.hpp"
#include "anvil/core/editing.hpp"
#include "anvil/core/editor_settings.hpp"
#include "anvil/core/highlighter.hpp"
#include "anvil/core/search.hpp"
#include "anvil/core/whitespace.hpp"
#include "anvil/ui/workbench_colors.hpp"

#include <QAbstractScrollArea>
#include <QElapsedTimer>
#include <QString>
#include <QTextLayout>
#include <QTimer>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
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
    // See core::EditController: the lines of the selections go up or down, are copied above or
    // below, or the selections are copied after themselves.
    void move_lines(bool down);
    void copy_lines(bool down);
    void duplicate_selection();
    // Moves every caret, or extends every selection when `extend` is set.
    void move(core::Motion motion, bool extend = false);
    // Deletes the selection, or the character before or after each caret.
    void delete_backward();
    void delete_forward();

    // What the find bar says about the search, for the occurrence commands.
    struct OccurrenceSource {
        // The pattern of the find bar when the bar owns the search (it is shown, has the focus
        // and has a pattern), which is then searched as plain text. Empty when the selection
        // owns the search.
        std::string find_text;
        // The find bar's options.
        bool match_case = false;
        bool whole_word = false;
    };
    // "Add selection to next find match" (Ctrl+D): a lone caret selects the word at it, and each
    // further call adds the next match of the same text. The search stays what the first call made
    // it, whole words and case included, until the selections change by other means.
    void add_next_occurrence(const OccurrenceSource& source);
    // The same, towards the start of the text: the first press selects the word at a caret, the
    // next ones add the match before the last one added, wrapping around from the end.
    void add_previous_occurrence(const OccurrenceSource& source);
    // Adds a cursor above or below each selection (see core::EditController).
    void add_cursors_vertically(bool down);
    void add_cursors_to_line_ends();
    // Moves every caret as Go to Bracket does (see core::jump_to_bracket).
    void jump_to_bracket();
    // Comments or uncomments the lines of every selection, or its text with a block comment (see
    // core::EditController::comment_lines and toggle_block_comment).
    void comment_lines(core::LineCommentType type, const core::CommentOptions& options);
    void toggle_block_comment(const core::CommentOptions& options);
    // Grows every selection to the next range around it, and goes back one (see
    // core::EditController::expand_selection).
    void expand_selection();
    void shrink_selection();
    // "Select all occurrences of find match" (Ctrl+Shift+L). Returns true when more than `limit`
    // matches exist and only `limit` were selected.
    bool select_all_occurrences(const OccurrenceSource& source, std::size_t limit);
    // Selects every match of `search`, up to `limit`, as the find bar's Alt+Enter does. Returns
    // true when there are more.
    bool select_all_matches(const core::Search& search, std::size_t limit);

    // Call after something other than this view changed the document or its selections.
    void document_changed_elsewhere();

    // Makes `range` the only selection and scrolls to show it.
    void select_range(core::Selection range);
    // Replaces the selections; `primary` indexes `ranges`.
    void select_ranges(std::vector<core::Selection> ranges, std::size_t primary);
    // Applies the edits as one undo step, as the user's own change: the window is told through
    // `text_changed`. See EditController::replace_ranges.
    void replace_ranges(std::vector<core::Edit> edits,
                        std::optional<core::Selection> selection_after = std::nullopt);

    // Shades the given byte ranges, which must be ordered and must not overlap, in a color that
    // differs from the selection. Empty ranges are not shown.
    void set_highlights(std::vector<core::Match> highlights);

    // Shows the syntax of the document in the colors and font styles that the theme gives the
    // scopes of the highlighter's spans (`WorkbenchColors::token_theme`). Null shows plain text.
    // Only the lines in view are asked for spans. The view paints again when `refresh_highlighting`
    // finds that the highlighter's version has changed, or at once for a new highlighter.
    void set_highlighter(std::shared_ptr<const core::Highlighter> highlighter);
    const std::shared_ptr<const core::Highlighter>& highlighter() const {
        return highlighter_;
    }
    // Paints again the lines in view whose spans differ from those last painted, if the
    // highlighter's version has changed since: when spans arrive for a few lines, the others are
    // left alone.
    void refresh_highlighting();

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
    // Tab is a key of the editor, not a request to move the focus to the next widget.
    bool focusNextPrevChild(bool next) override;

private:
    struct LineLayout;
    // The point of the text, in the coordinates of `drag_origin_`, that the viewport point shows.
    QPoint text_point(const QPoint& viewport_point) const;
    // Selects the rectangle between `drag_origin_` and the pointer, one selection on each line,
    // clamped to the end of lines that are shorter; the line of the pointer has the primary one.
    void select_column_to(const QPoint& viewport_point);
    // Whether `modifiers` hold the modifier that adds a cursor, and one of the others.
    bool has_multi_cursor_modifier(Qt::KeyboardModifiers modifiers) const;
    bool has_other_modifier(Qt::KeyboardModifiers modifiers) const;
    // Draws the marks for the spaces and tabs of the line that `render_whitespace_` asks for.
    // A number that differs when the spans of the highlighter on `line` differ.
    std::size_t span_digest(std::size_t line) const;
    // The format of each span of the highlighter on `line`, whose text has `size` UTF-16 units.
    QList<QTextLayout::FormatRange> syntax_formats(const core::TokenTheme& token_theme,
                                                   std::size_t line, qsizetype size) const;
    void draw_whitespace(QPainter& painter, const LineLayout& layout,
                         std::span<const core::ColumnRange> selected, int origin_x, int y,
                         const QColor& color) const;

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

    // The search that Ctrl+D started and keeps while the selections are the ones it made.
    struct OccurrenceSession {
        std::optional<core::Selection> first;
        core::Search search;
        core::SelectionSet selections;
        bool uses_find_options = false;
        bool match_case = false;
        bool whole_word = false;
    };
    // The session if it still describes the selections and the find options, otherwise none.
    OccurrenceSession* occurrence_session(const OccurrenceSource& source);
    void add_occurrence(const OccurrenceSource& source, bool next);
    std::optional<OccurrenceSession> begin_occurrence_session(const OccurrenceSource& source) const;

    const WorkbenchColors* colors_ = nullptr;
    QMetaObject::Connection colors_connection_;
    std::shared_ptr<core::Document> document_;
    std::unique_ptr<core::EditController> controller_;
    int line_height_ = 16;
    int char_width_ = 8;
    std::size_t tab_size_ = 4;
    bool insert_spaces_ = true;
    core::RenderWhitespace render_whitespace_ = core::RenderWhitespace::selection;
    bool render_control_characters_ = true;
    int max_line_width_ = 0;
    bool caret_visible_ = true;
    // What moving the mouse with the button down does: nothing, extends the selection the press
    // made (and becomes a column selection when the mouse asks for one), or selects a rectangle
    // between where the press was and the pointer.
    enum class MouseDrag { none, extend, extend_added_cursor, column };
    MouseDrag mouse_drag_ = MouseDrag::none;
    // Where a press was, in text coordinates (x from the start of the text, y from the top of the
    // document), so that scrolling during a drag does not move it.
    QPoint drag_origin_;
    core::MultiCursorModifier multi_cursor_modifier_ = core::MultiCursorModifier::alt;
    bool column_selection_ = false;
    QTimer caret_timer_;
    QElapsedTimer since_double_click_;
    QString preedit_;
    std::vector<core::Match> highlights_;
    std::shared_ptr<const core::Highlighter> highlighter_;
    std::uint64_t painted_highlighter_version_ = 0;
    // A digest of the spans that each line was last laid out with, which tells
    // `refresh_highlighting` what changed. Lines far from the view are dropped as it runs.
    mutable std::unordered_map<std::size_t, std::size_t> laid_out_spans_;
    std::optional<OccurrenceSession> occurrence_session_;
};

} // namespace anvil::ui
