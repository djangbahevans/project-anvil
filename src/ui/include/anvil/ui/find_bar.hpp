#pragma once

#include "anvil/core/search.hpp"
#include "anvil/ui/editor_view.hpp"

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

// The find and replace bar above the editor. It searches the document the editor shows as the
// pattern is typed, selects a match and shades all matches, counts them, and replaces them.
//
// Documents up to `synchronous_limit_bytes` are searched at once. A larger one is searched on a
// worker thread over a snapshot of its text, and the result is dropped when the document changes
// meanwhile, so a large document never blocks the window. At most `match_limit` matches are
// collected; the counter then reads "20000+". The search runs again whenever the text changes
// while the bar is open, and does nothing while the bar is closed.
class FindBar : public QWidget {
    Q_OBJECT

public:
    static constexpr std::size_t match_limit = 20000;
    static constexpr std::size_t synchronous_limit_bytes = 1024 * 1024;

    explicit FindBar(EditorView* view, QWidget* parent = nullptr);
    ~FindBar() override;

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

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
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

    static Results search_snapshot(const core::Search& search, const core::Text& text,
                                   std::optional<std::size_t> navigate_from,
                                   const std::stop_token& stop);

    core::Query query() const;
    void query_changed();
    // Searches the whole document again. With `navigate`, also selects the first match at or after
    // the start of the selection.
    void refresh(bool navigate);
    void apply(Results results);
    void navigate(Direction direction);
    void select(const core::Match& match, bool wrapped);
    bool results_are_current() const;
    void cancel_search();
    void update_status();
    void set_error_state(bool error);

    EditorView* view_;
    QLineEdit* find_edit_ = nullptr;
    QLineEdit* replace_edit_ = nullptr;
    QToolButton* replace_toggle_ = nullptr;
    QToolButton* case_button_ = nullptr;
    QToolButton* word_button_ = nullptr;
    QToolButton* regex_button_ = nullptr;
    QLabel* status_label_ = nullptr;
    QWidget* replace_row_ = nullptr;

    std::optional<core::Search> search_;
    QString query_error_;
    QString search_error_;

    std::vector<core::Match> matches_;
    bool truncated_ = false;
    // The document and version that `matches_` describe.
    const core::Document* results_document_ = nullptr;
    std::uint64_t results_version_ = 0;
    bool wrapped_ = false;

    bool searching_ = false;
    std::optional<Direction> navigate_when_done_;
    std::uint64_t generation_ = 0;
    std::stop_source stop_;
};

} // namespace anvil::ui
