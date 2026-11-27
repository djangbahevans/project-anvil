#pragma once

#include "anvil/core/search.hpp"
#include "anvil/ui/editor_view.hpp"
#include "anvil/ui/workbench_colors.hpp"
#include "anvil/ui/zoom.hpp"

#include <QPointer>
#include <QWidget>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <vector>

class QAbstractButton;
class QLabel;
class QLineEdit;
class QToolButton;

namespace anvil::ui {

// The find and replace widget, which floats at the top right of the editor as that of VS Code
// does, over the text and left of the scroll bar. It is placed over the view when both are in the
// same parent. It searches the document the editor shows as the
// pattern is typed, selects a match and shades all matches, counts them, and replaces them.
//
// Documents up to `synchronous_limit_bytes` are searched at once. A larger one is searched on a
// worker thread over a snapshot of its text, and the result is dropped when the document changes
// meanwhile, so a large document never blocks the window. At most `match_limit` matches are
// collected; the counter then reads "20000+". The search runs again whenever the text changes
// while the bar is open, and does nothing while the bar is closed. A match that the widget would
// cover is scrolled from under it where the editor can scroll.
class FindBar : public QWidget {
    Q_OBJECT

public:
    static constexpr std::size_t match_limit = 20000;
    static constexpr std::size_t synchronous_limit_bytes = 1024 * 1024;

    // The sizes of VS Code's widget at zoom level 0: its width where the editor has room for it,
    // its height without and with the replace field, and the height of a field.
    static constexpr int widget_width = 419;
    static constexpr int find_height = 33;
    static constexpr int replace_height = 62;
    static constexpr int field_height = 25;

    explicit FindBar(EditorView* view, QWidget* parent = nullptr);
    ~FindBar() override;

    // The zoom that the sizes of the bar are read through, which must outlive the bar; the bar
    // follows it when it changes. Null means level 0.
    void set_zoom(const Zoom* zoom);
    // The colors of the theme, which must outlive the bar; null takes them from the palette.
    void set_colors(const WorkbenchColors* colors);
    QSize sizeHint() const override;

    // Shows the bar and focuses the find field. With `with_replace` the replace field is shown.
    void open(bool with_replace = false);
    // Hides the bar, removes the highlights and returns the focus to the editor.
    void close_bar();

    // Select the next or previous match from the current selection, wrapping around the text.
    void find_next();
    void find_previous();
    // Replaces the selection when it is a match, then selects the next match.
    void replace_current();
    void replace_all();

    // The search for the pattern and options now in the bar; none while the pattern is empty or
    // does not compile.
    const std::optional<core::Search>& search() const {
        return search_;
    }

    // What the counter shows: "3 of 41", "No results", an error, or nothing.
    QString status() const;
    // True when the pattern is an invalid regular expression.
    bool has_error() const;
    // True while a search on a worker thread has not finished.
    bool is_searching() const {
        return searching_;
    }
    std::size_t match_count() const {
        return matches_.size();
    }

    QLineEdit* find_field() {
        return find_edit_;
    }
    QLineEdit* replace_field() {
        return replace_edit_;
    }
    QAbstractButton* match_case_button();
    QAbstractButton* whole_word_button();
    QAbstractButton* regex_button();
    QAbstractButton* toggle_replace_button();
    QAbstractButton* previous_button();
    QAbstractButton* next_button();
    QAbstractButton* close_button();
    QAbstractButton* replace_button();
    QAbstractButton* replace_all_button();
    // The boxes that hold the fields, and the count of matches.
    QWidget* find_box() const;
    QWidget* replace_box() const;
    QWidget* count_label() const;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    class Button;
    class Box;
    class Shadow;

    void apply_zoom();
    void apply_colors();
    int zoomed(int size) const {
        return scaled_by(zoom_, size);
    }
    // Puts the widget at the top right of the view, and its parts in it.
    void place();
    void arrange();
    void place_shadow();
    // Scrolls the editor so that the widget does not cover the match.
    void uncover(const core::Match& match);

    const Zoom* zoom_ = nullptr;
    int base_font_pixels_ = 0;
    WorkbenchColors palette_colors_;
    const WorkbenchColors* colors_ = nullptr;
    // The width of the count at level 0 and above: that of the widest it has shown.
    int count_width_ = 0;

    enum class Direction {
        forward,
        backward,
    };

    // What a search over a snapshot produces.
    struct Results {
        std::uint64_t generation = 0;
        const core::Document* document = nullptr;
        std::uint64_t version = 0;
        core::MatchList list;
        std::optional<core::Hit> target;
        std::optional<Error> error;
    };

    // Fills in the matches of `results`, whose identity fields are already set. With
    // `navigate_from`, also finds the match to select.
    static Results search_snapshot(Results results, const core::Search& search,
                                   const core::Text& text, std::optional<std::size_t> navigate_from,
                                   const std::stop_token& stop);

    core::Query query() const;
    void query_changed();
    // Builds the search from the field and the options and searches again; with `navigate`, also
    // selects the first match at or after the start of the selection.
    void apply_query(bool navigate);
    // The text that opening the bar puts in the field: the selection when it is on one line and
    // not too long, otherwise the word at the caret; nothing when there is neither.
    std::optional<QString> seed_from_editor() const;
    // Searches the whole document again. With `navigate`, also selects the first match at or after
    // the start of the selection.
    void refresh(bool navigate);
    void apply(Results results);
    void navigate(Direction direction);
    void select(const core::Match& match);
    // True when `match` is an empty match that the selection (a caret) already sits on.
    static bool is_selected_empty_match(const core::Match& match, const core::Selection& selection);
    bool results_are_current() const;
    void cancel_search();
    void update_status();
    void set_error_state(bool error);

    EditorView* view_;
    QLineEdit* find_edit_ = nullptr;
    QLineEdit* replace_edit_ = nullptr;
    Box* find_box_ = nullptr;
    Box* replace_box_ = nullptr;
    Button* replace_toggle_ = nullptr;
    Button* case_button_ = nullptr;
    Button* word_button_ = nullptr;
    Button* regex_button_ = nullptr;
    Button* previous_ = nullptr;
    Button* next_ = nullptr;
    Button* close_ = nullptr;
    Button* replace_ = nullptr;
    Button* replace_all_ = nullptr;
    QLabel* status_label_ = nullptr;
    // Under the widget, in its parent, so that it is not clipped by the widget.
    QPointer<QWidget> shadow_;

    std::optional<core::Search> search_;
    QString query_error_;
    QString search_error_;

    std::vector<core::Match> matches_;
    bool truncated_ = false;
    // The document and version that `matches_` describe.
    const core::Document* results_document_ = nullptr;
    std::uint64_t results_version_ = 0;

    bool searching_ = false;
    std::optional<Direction> navigate_when_done_;
    std::uint64_t generation_ = 0;
    std::stop_source stop_;
};

} // namespace anvil::ui
