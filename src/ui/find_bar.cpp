#include "anvil/ui/find_bar.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPointer>
#include <QSignalBlocker>
#include <QThreadPool>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>

namespace anvil::ui {
namespace {

constexpr const char* error_style = "QLineEdit { border: 1px solid #d13438; }";
// VS Code's limit on a selection that seeds the search (SEARCH_STRING_MAX_LENGTH), in UTF-16 units.
constexpr std::size_t max_seed_length = 524288;

QToolButton* make_toggle(const QString& text, const QString& tooltip, QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setText(text);
    button->setToolTip(tooltip);
    button->setCheckable(true);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::TabFocus);
    return button;
}

QToolButton* make_action(const QString& text, const QString& tooltip, QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setText(text);
    button->setToolTip(tooltip);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::TabFocus);
    return button;
}

core::Selection to_selection(const core::Match& match) {
    return {match.begin, match.end};
}

} // namespace

FindBar::FindBar(EditorView* view, QWidget* parent) : QWidget(parent), view_(view) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 4, 6, 4);
    layout->setSpacing(4);

    auto* find_row = new QHBoxLayout;
    replace_toggle_ = make_toggle(QString::fromUtf8("▸"), tr("Toggle Replace"), this);
    find_edit_ = new QLineEdit(this);
    find_edit_->setPlaceholderText(tr("Find"));
    // A longer selection can seed the field, and the default limit would cut it silently.
    find_edit_->setMaxLength(static_cast<int>(max_seed_length));
    find_edit_->setClearButtonEnabled(true);
    find_edit_->installEventFilter(this);
    view_->installEventFilter(this);
    case_button_ = make_toggle(tr("Aa"), tr("Match Case"), this);
    word_button_ = make_toggle(tr("ab"), tr("Match Whole Word"), this);
    regex_button_ = make_toggle(tr(".*"), tr("Use Regular Expression"), this);
    status_label_ = new QLabel(this);
    status_label_->setMinimumWidth(status_label_->fontMetrics().horizontalAdvance(
        QStringLiteral("20000+ of 20000+ (wrapped)")));
    auto* previous = make_action(QString::fromUtf8("↑"), tr("Previous Match"), this);
    auto* next = make_action(QString::fromUtf8("↓"), tr("Next Match"), this);
    auto* close = make_action(QString::fromUtf8("✕"), tr("Close"), this);
    find_row->addWidget(replace_toggle_);
    find_row->addWidget(find_edit_, 1);
    find_row->addWidget(case_button_);
    find_row->addWidget(word_button_);
    find_row->addWidget(regex_button_);
    find_row->addWidget(status_label_);
    find_row->addWidget(previous);
    find_row->addWidget(next);
    find_row->addWidget(close);
    layout->addLayout(find_row);

    replace_row_ = new QWidget(this);
    auto* replace_layout = new QHBoxLayout(replace_row_);
    replace_layout->setContentsMargins(0, 0, 0, 0);
    replace_edit_ = new QLineEdit(replace_row_);
    replace_edit_->setPlaceholderText(tr("Replace"));
    replace_edit_->installEventFilter(this);
    auto* replace = make_action(tr("Replace"), tr("Replace the Match"), replace_row_);
    auto* replace_every = make_action(tr("Replace All"), tr("Replace All Matches"), replace_row_);
    replace_layout->addSpacing(replace_toggle_->sizeHint().width() + find_row->spacing());
    replace_layout->addWidget(replace_edit_, 1);
    replace_layout->addWidget(replace);
    replace_layout->addWidget(replace_every);
    layout->addWidget(replace_row_);
    replace_row_->hide();
    hide();

    connect(replace_toggle_, &QToolButton::toggled, this, [this](bool shown) {
        replace_row_->setVisible(shown);
        replace_toggle_->setText(QString::fromUtf8(shown ? "▾" : "▸"));
    });
    connect(find_edit_, &QLineEdit::textChanged, this, &FindBar::query_changed);
    for (QToolButton* button : {case_button_, word_button_, regex_button_}) {
        connect(button, &QToolButton::toggled, this, &FindBar::query_changed);
    }
    connect(next, &QToolButton::clicked, this, &FindBar::find_next);
    connect(previous, &QToolButton::clicked, this, &FindBar::find_previous);
    connect(close, &QToolButton::clicked, this, &FindBar::close_bar);
    connect(replace, &QToolButton::clicked, this, &FindBar::replace_current);
    connect(replace_every, &QToolButton::clicked, this, &FindBar::replace_all);
    connect(view_, &EditorView::content_changed, this, [this] {
        if (!isHidden()) {
            refresh(false);
        }
    });
    connect(view_, &EditorView::selections_changed, this, [this] {
        if (!isHidden()) {
            wrapped_ = false;
            update_status();
        }
    });
}

FindBar::~FindBar() {
    stop_.request_stop();
}

QAbstractButton* FindBar::match_case_button() {
    return case_button_;
}
QAbstractButton* FindBar::whole_word_button() {
    return word_button_;
}
QAbstractButton* FindBar::regex_button() {
    return regex_button_;
}

void FindBar::open(bool with_replace) {
    // With the focus already in the bar, what the user typed there is not replaced.
    const bool focus_in_bar = isAncestorOf(QApplication::focusWidget());
    show();
    if (with_replace) {
        replace_toggle_->setChecked(true);
    }
    bool seeded = false;
    if (!focus_in_bar) {
        if (const auto seed = seed_from_editor()) {
            {
                const QSignalBlocker blocker(find_edit_);
                find_edit_->setText(*seed);
            }
            // The selection stays where it is, so that Enter goes to the next occurrence.
            apply_query(false);
            seeded = true;
        }
    }
    find_edit_->setFocus();
    find_edit_->selectAll();
    if (!seeded) {
        refresh(false);
    }
}

std::optional<QString> FindBar::seed_from_editor() const {
    const auto& document = view_->document();
    if (!document) {
        return std::nullopt;
    }
    const core::Text text = document->text();
    const core::Selection selection = document->selections().primary();
    std::size_t begin = selection.start();
    std::size_t end = selection.end();
    if (selection.empty()) {
        const auto word = core::word_at(text, selection.active);
        if (!word) {
            return std::nullopt;
        }
        begin = word->start();
        end = word->end();
    } else if (text.position_at(begin).line != text.position_at(end).line ||
               text.utf16_offset(end) - text.utf16_offset(begin) >= max_seed_length) {
        return std::nullopt;
    }
    std::string seed = text.slice(begin, end - begin);
    if (regex_button_->isChecked()) {
        seed = core::escape_regex(seed);
    }
    return QString::fromStdString(seed);
}

void FindBar::close_bar() {
    cancel_search();
    hide();
    matches_.clear();
    view_->set_highlights({});
    view_->setFocus();
}

QString FindBar::status() const {
    return status_label_->text();
}

bool FindBar::has_error() const {
    return !query_error_.isEmpty();
}

core::Query FindBar::query() const {
    return {.pattern = find_edit_->text().toStdString(),
            .match_case = case_button_->isChecked(),
            .whole_word = word_button_->isChecked(),
            .regex = regex_button_->isChecked()};
}

void FindBar::query_changed() {
    apply_query(true);
}

void FindBar::apply_query(bool navigate) {
    search_.reset();
    query_error_.clear();
    if (!find_edit_->text().isEmpty()) {
        auto created = core::Search::create(query());
        if (created) {
            search_ = std::move(*created);
        } else {
            query_error_ = QString::fromStdString(created.error().message);
        }
    }
    set_error_state(!query_error_.isEmpty());
    wrapped_ = false;
    refresh(navigate);
}

void FindBar::set_error_state(bool error) {
    find_edit_->setStyleSheet(error ? QString::fromLatin1(error_style) : QString());
    find_edit_->setToolTip(error ? query_error_ : QString());
}

void FindBar::cancel_search() {
    stop_.request_stop();
    stop_ = std::stop_source();
    ++generation_;
    searching_ = false;
    navigate_when_done_.reset();
}

FindBar::Results FindBar::search_snapshot(Results results, const core::Search& search,
                                          const core::Text& text,
                                          std::optional<std::size_t> navigate_from,
                                          const std::stop_token& stop) {
    if (navigate_from) {
        auto target = search.find_next(text, *navigate_from, true);
        if (target) {
            results.target = *target;
        } else {
            results.error = std::move(target.error());
        }
    }
    if (!results.error) {
        auto list = search.find_all(text, 0, text.size(), match_limit, stop);
        if (list) {
            results.list = std::move(*list);
        } else {
            results.error = std::move(list.error());
        }
    }
    return results;
}

void FindBar::refresh(bool navigate) {
    cancel_search();
    matches_.clear();
    truncated_ = false;
    search_error_.clear();
    results_document_ = nullptr;
    view_->set_highlights({});
    const auto& document = view_->document();
    if (!search_ || !document) {
        update_status();
        return;
    }

    const core::Text text = document->text();
    std::optional<std::size_t> from;
    if (navigate) {
        from = document->selections().primary().start();
    }
    Results identity;
    identity.generation = generation_;
    identity.document = document.get();
    identity.version = document->version();

    if (text.size() <= synchronous_limit_bytes) {
        apply(search_snapshot(identity, *search_, text, from, {}));
        return;
    }

    searching_ = true;
    update_status();
    // The task may outlive this bar. It hands its result to the application object, which
    // outlives every widget, and the bar is looked up there on the GUI thread, where it cannot be
    // destroyed in between.
    QThreadPool::globalInstance()->start([self = QPointer<FindBar>(this), search = *search_, text,
                                          from, identity, token = stop_.get_token()] {
        Results found = search_snapshot(identity, search, text, from, token);
        if (QCoreApplication* application = QCoreApplication::instance()) {
            QMetaObject::invokeMethod(
                application,
                [self, found = std::move(found)]() mutable {
                    if (self) {
                        self->apply(std::move(found));
                    }
                },
                Qt::QueuedConnection);
        }
    });
}

void FindBar::apply(Results results) {
    const auto& document = view_->document();
    if (isHidden() || results.generation != generation_ || !document ||
        document.get() != results.document || document->version() != results.version) {
        return;
    }
    searching_ = false;
    if (results.error) {
        search_error_ = QString::fromStdString(results.error->message);
        update_status();
        return;
    }
    matches_ = std::move(results.list.matches);
    truncated_ = results.list.truncated;
    results_document_ = results.document;
    results_version_ = results.version;
    view_->set_highlights(matches_);
    if (results.target) {
        select(results.target->match, results.target->wrapped);
    }
    update_status();
    if (const auto direction = std::exchange(navigate_when_done_, std::nullopt)) {
        navigate(*direction);
    }
}

bool FindBar::results_are_current() const {
    const auto& document = view_->document();
    return document && document.get() == results_document_ &&
           document->version() == results_version_;
}

void FindBar::find_next() {
    navigate(Direction::forward);
}

void FindBar::find_previous() {
    navigate(Direction::backward);
}

void FindBar::navigate(Direction direction) {
    const auto& document = view_->document();
    if (!search_ || !document) {
        return;
    }
    if (searching_) {
        navigate_when_done_ = direction;
        return;
    }
    const core::Selection selection = document->selections().primary();
    const std::size_t from = direction == Direction::forward ? selection.end() : selection.start();

    // A complete list is the same answer as searching again, without reading the text.
    if (results_are_current() && !truncated_) {
        if (matches_.empty()) {
            return;
        }
        const auto first_at_or_after = std::lower_bound(
            matches_.begin(), matches_.end(), from,
            [](const core::Match& match, std::size_t offset) { return match.begin < offset; });
        if (direction == Direction::forward) {
            auto next = first_at_or_after;
            if (next != matches_.end() && is_selected_empty_match(*next, selection)) {
                ++next;
            }
            const bool wrapped = next == matches_.end();
            select(wrapped ? matches_.front() : *next, wrapped);
        } else {
            const bool wrapped = first_at_or_after == matches_.begin();
            select(wrapped ? matches_.back() : *(first_at_or_after - 1), wrapped);
        }
        return;
    }

    const core::Text text = document->text();
    auto hit = direction == Direction::forward ? search_->find_next(text, from, true)
                                               : search_->find_previous(text, from, true);
    if (hit && *hit && direction == Direction::forward &&
        is_selected_empty_match((*hit)->match, selection)) {
        std::size_t after = from + 1;
        while (after < text.size() && !text.is_char_boundary(after)) {
            ++after;
        }
        hit = search_->find_next(text, after, true);
    }
    if (!hit) {
        search_error_ = QString::fromStdString(hit.error().message);
        update_status();
    } else if (*hit) {
        select((*hit)->match, (*hit)->wrapped);
    }
}

// An empty match is a caret, so searching on from the caret would find it again.
bool FindBar::is_selected_empty_match(const core::Match& match, const core::Selection& selection) {
    return selection.empty() && match.begin == match.end && match.begin == selection.start();
}

void FindBar::select(const core::Match& match, bool wrapped) {
    view_->select_range(to_selection(match));
    // The selection change has reset the flag.
    wrapped_ = wrapped;
    update_status();
}

void FindBar::replace_current() {
    const auto& document = view_->document();
    if (!search_ || !document) {
        return;
    }
    const core::Selection selection = document->selections().primary();
    auto replacement =
        search_->replacement_for(document->text(), {selection.start(), selection.end()},
                                 replace_edit_->text().toStdString());
    if (!replacement) {
        search_error_ = QString::fromStdString(replacement.error().message);
        update_status();
        return;
    }
    if (*replacement) {
        const std::size_t caret = selection.start() + (*replacement)->size();
        view_->replace_ranges({{selection.start(), selection.end(), std::move(**replacement)}},
                              core::Selection::caret(caret));
    }
    find_next();
}

void FindBar::replace_all() {
    const auto& document = view_->document();
    if (!search_ || !document) {
        return;
    }
    auto edits = search_->replace_all(document->text(), replace_edit_->text().toStdString());
    if (!edits) {
        search_error_ = QString::fromStdString(edits.error().message);
        update_status();
        return;
    }
    view_->replace_ranges(std::move(*edits));
}

void FindBar::update_status() {
    QString text;
    if (!query_error_.isEmpty()) {
        text = tr("Invalid pattern");
    } else if (!search_error_.isEmpty()) {
        text = search_error_;
    } else if (searching_) {
        text = tr("Searching…");
    } else if (search_) {
        if (matches_.empty()) {
            text = tr("No results");
        } else {
            const QString total = truncated_ ? QStringLiteral("%1+").arg(matches_.size())
                                             : QString::number(matches_.size());
            QString index = QStringLiteral("?");
            if (const auto& document = view_->document()) {
                const core::Selection selection = document->selections().primary();
                const core::Match selected{selection.start(), selection.end()};
                const auto found =
                    std::lower_bound(matches_.begin(), matches_.end(), selected.begin,
                                     [](const core::Match& match, std::size_t offset) {
                                         return match.begin < offset;
                                     });
                if (found != matches_.end() && *found == selected) {
                    index = QString::number(found - matches_.begin() + 1);
                }
            }
            text = tr("%1 of %2").arg(index, total);
            if (wrapped_) {
                text += tr(" (wrapped)");
            }
        }
    }
    status_label_->setText(text);
    status_label_->setToolTip(!query_error_.isEmpty() ? query_error_ : search_error_);
}

bool FindBar::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() != QEvent::KeyPress) {
        return QWidget::eventFilter(watched, event);
    }
    const auto* key = static_cast<QKeyEvent*>(event);
    if (watched == view_) {
        // The editor drops extra cursors on Escape; while the bar is open, the key closes the bar
        // instead, unless an input method composition is using it.
        if (key->key() != Qt::Key_Escape || isHidden() || view_->composing()) {
            return QWidget::eventFilter(watched, event);
        }
        close_bar();
        return true;
    }
    if (key->key() == Qt::Key_Escape) {
        close_bar();
        return true;
    }
    const bool enter = key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter;
    if (!enter) {
        return QWidget::eventFilter(watched, event);
    }
    if (watched == find_edit_) {
        if (key->modifiers() & Qt::ShiftModifier) {
            find_previous();
        } else {
            find_next();
        }
        return true;
    }
    if (watched == replace_edit_) {
        if (key->modifiers() & Qt::ControlModifier) {
            replace_all();
        } else {
            replace_current();
        }
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace anvil::ui
