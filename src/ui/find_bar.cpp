#include "anvil/ui/find_bar.hpp"

#include "anvil/ui/codicons.hpp"
#include "anvil/ui/scroll_bar.hpp"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QFontInfo>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPainter>
#include <QPointer>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QThreadPool>
#include <QToolButton>
#include <algorithm>
#include <string_view>
#include <utility>

namespace anvil::ui {
namespace {

// VS Code's measures at zoom level 0, from the stylesheet of its find widget.
constexpr int border_px = 1;
constexpr int widget_radius_px = 8;
constexpr int top_gap_px = 4;
constexpr int left_gap_px = 4;
constexpr int shadow_px = 8;
constexpr int toggle_width_px = 18;
constexpr int toggle_left_px = 5;
constexpr int toggle_margin_px = 4;
constexpr int field_left_px = 27;
constexpr int close_area_px = 30;
constexpr int row_gap_px = 3;
constexpr int field_padding_px = 6;
constexpr int field_radius_px = 4;
constexpr int option_size_px = 20;
constexpr int option_gap_px = 2;
constexpr int option_radius_px = 3;
constexpr int button_size_px = 22;
constexpr int button_gap_px = 3;
constexpr int button_radius_px = 5;
constexpr int close_right_px = 5;
constexpr int close_top_px = 6;
constexpr int count_width_px = 69;
constexpr int glyph_px = 16;
// Below these the count and then the arrows make room for the field, and the options for the text.
constexpr int field_width_with_count_px = 120;
constexpr int field_width_with_arrows_px = 80;
// VS Code's limit on a selection that seeds the search (SEARCH_STRING_MAX_LENGTH), in UTF-16 units.
constexpr std::size_t max_seed_length = 524288;

// VS Code's shortcut for a find option: Alt and a letter, and Cmd+Alt and the letter on macOS.
QKeySequence option_shortcut(Qt::Key key) {
#ifdef Q_OS_MACOS
    return QKeySequence(Qt::CTRL | Qt::ALT | key);
#else
    return QKeySequence(Qt::ALT | key);
#endif
}

// A checkable action for a find option. Its tooltip names its shortcut, and follows it when the
// shortcut changes. The action is active in the whole window while the button it is shown by is
// visible, so the shortcut works from the editor while the bar is open and not while it is closed.
QAction* make_option_action(QObject* parent, const QString& text, const QString& description,
                            Qt::Key key) {
    auto* action = new QAction(text, parent);
    action->setCheckable(true);
    action->setShortcut(option_shortcut(key));
    action->setShortcutContext(Qt::WindowShortcut);
    const auto update_tooltip = [action, description] {
        const QString shortcut = action->shortcut().toString(QKeySequence::NativeText);
        const QString tooltip =
            shortcut.isEmpty() ? description : QStringLiteral("%1 (%2)").arg(description, shortcut);
        if (action->toolTip() != tooltip) {
            action->setToolTip(tooltip);
        }
    };
    QObject::connect(action, &QAction::changed, action, update_tooltip);
    update_tooltip();
    return action;
}

core::Selection to_selection(const core::Match& match) {
    return {match.begin, match.end};
}

} // namespace

// A button that shows a Codicon: an option inside a field, which has its own colors while it is
// on, or a button beside the fields.
class FindBar::Button : public QToolButton {
public:
    enum class Kind { button, option };

    Button(FindBar& bar, Kind kind, std::string_view icon, const QString& name, QWidget* parent)
        : QToolButton(parent), bar_(bar), kind_(kind), icon_(icon) {
        setAccessibleName(name);
        setToolTip(name);
        setFocusPolicy(Qt::TabFocus);
        setCursor(Qt::ArrowCursor);
        setAttribute(Qt::WA_Hover);
    }

    void set_icon(std::string_view icon) {
        icon_ = icon;
        update();
    }
    std::string_view icon_name() const {
        return icon_;
    }

protected:
    void paintEvent(QPaintEvent*) override {
        const WorkbenchColors& colors = *bar_.colors_;
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const bool option = kind_ == Kind::option;
        const double radius = bar_.zoomed(option ? option_radius_px : button_radius_px);
        const QRectF area = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        const bool on = option && isChecked();
        const char* hover = option ? "inputOption.hoverBackground" : "toolbar.hoverBackground";
        painter.setPen(Qt::NoPen);
        if (on) {
            painter.setPen(colors.color("inputOption.activeBorder"));
            painter.setBrush(colors.color("inputOption.activeBackground"));
            painter.drawRoundedRect(area, radius, radius);
        } else if (isEnabled() && underMouse() && colors.is_themed(hover)) {
            painter.setBrush(colors.color(hover));
            painter.drawRoundedRect(area, radius, radius);
        }
        if (hasFocus()) {
            painter.setPen(colors.color("focusBorder"));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(area, radius, radius);
        }
        const auto glyph = codicon(icon_);
        const QString family = codicon_font_family();
        if (!glyph || family.isEmpty()) {
            return;
        }
        QColor color = colors.color(on ? "inputOption.activeForeground" : "icon.foreground");
        if (!isEnabled()) {
            color.setAlphaF(color.alphaF() * 0.4F);
        }
        QFont font(family);
        font.setPixelSize(bar_.zoomed(glyph_px));
        // Smoothing by subpixel gives the thin strokes colored edges.
        font.setStyleStrategy(QFont::NoSubpixelAntialias);
        painter.setFont(font);
        painter.setPen(color);
        painter.drawText(rect(), Qt::AlignCenter, QString(QChar(*glyph)));
    }

private:
    FindBar& bar_;
    Kind kind_;
    std::string_view icon_;
};

// The box of an input field, which holds the line edit and its options.
class FindBar::Box : public QWidget {
public:
    Box(FindBar& bar, QWidget* parent) : QWidget(parent), bar_(bar) {}

    void set_field(QLineEdit* field) {
        field_ = field;
        field_->installEventFilter(this);
    }
    void set_error(bool error) {
        error_ = error;
        update();
    }
    // The color of the line around the box.
    QColor border() const {
        return bar_.colors_->color(error_                         ? "inputValidation.errorBorder"
                                   : field_ && field_->hasFocus() ? "focusBorder"
                                                                  : "input.border");
    }

protected:
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut) {
            update();
        }
        return false;
    }
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const double radius = bar_.zoomed(field_radius_px);
        painter.setPen(border());
        painter.setBrush(bar_.colors_->color("input.background"));
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    }

private:
    FindBar& bar_;
    QLineEdit* field_ = nullptr;
    bool error_ = false;
};

// The soft edge around the widget, in `widget.shadow`; none where the theme has no such color.
class FindBar::Shadow : public QWidget {
public:
    Shadow(FindBar& bar, QWidget* parent) : QWidget(parent), bar_(bar) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        if (!bar_.colors_->is_themed("widget.shadow")) {
            return;
        }
        QColor color = bar_.colors_->color("widget.shadow");
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setBrush(Qt::NoBrush);
        const int size = bar_.zoomed(shadow_px);
        const double radius = bar_.zoomed(widget_radius_px);
        const int alpha = color.alpha();
        for (int out = 1; out <= size; ++out) {
            color.setAlpha(alpha * (size - out + 1) / (2 * size));
            painter.setPen(color);
            const QRectF ring = QRectF(rect()).adjusted(size - out, -out, out - size, out - size);
            painter.drawRoundedRect(ring, radius + out, radius + out);
        }
    }

private:
    FindBar& bar_;
};

FindBar::FindBar(EditorView* view, QWidget* parent)
    : QWidget(parent), palette_colors_(palette()), view_(view) {
    colors_ = &palette_colors_;
    base_font_pixels_ = QFontInfo(font()).pixelSize();
    setCursor(Qt::ArrowCursor);
    setAccessibleName(tr("Find / Replace"));

    replace_toggle_ =
        new Button(*this, Button::Kind::button, "chevron-right", tr("Toggle Replace"), this);
    replace_toggle_->setCheckable(true);

    find_box_ = new Box(*this, this);
    find_edit_ = new QLineEdit(find_box_);
    find_edit_->setPlaceholderText(tr("Find"));
    // A longer selection can seed the field, and the default limit would cut it silently.
    find_edit_->setMaxLength(static_cast<int>(max_seed_length));
    find_edit_->installEventFilter(this);
    find_box_->set_field(find_edit_);
    view_->installEventFilter(this);
    const auto option = [this](std::string_view icon, const QString& name, Qt::Key key) {
        auto* button = new Button(*this, Button::Kind::option, icon, name, find_box_);
        QAction* action = make_option_action(this, name, name, key);
        button->setDefaultAction(action);
        // Also the bar's, so that the key works while a narrow bar has no room for the button.
        addAction(action);
        return button;
    };
    case_button_ = option("case-sensitive", tr("Match Case"), Qt::Key_C);
    word_button_ = option("whole-word", tr("Match Whole Word"), Qt::Key_W);
    regex_button_ = option("regex", tr("Use Regular Expression"), Qt::Key_R);

    status_label_ = new QLabel(this);
    const auto button = [this](std::string_view icon, const QString& name) {
        return new Button(*this, Button::Kind::button, icon, name, this);
    };
    previous_ = button("arrow-up", tr("Previous Match"));
    next_ = button("arrow-down", tr("Next Match"));
    close_ = button("close", tr("Close"));

    replace_box_ = new Box(*this, this);
    replace_edit_ = new QLineEdit(replace_box_);
    replace_edit_->setPlaceholderText(tr("Replace"));
    replace_edit_->installEventFilter(this);
    replace_box_->set_field(replace_edit_);
    replace_ = button("replace", tr("Replace"));
    replace_all_ = button("replace-all", tr("Replace All"));
    for (QWidget* part : {static_cast<QWidget*>(replace_box_), static_cast<QWidget*>(replace_),
                          static_cast<QWidget*>(replace_all_)}) {
        part->hide();
    }

    // The order of VS Code.
    const std::initializer_list<QWidget*> order = {
        replace_toggle_, find_edit_, case_button_, word_button_, regex_button_, replace_edit_,
        previous_,       next_,      close_,       replace_,     replace_all_};
    for (auto next = order.begin() + 1; next != order.end(); ++next) {
        setTabOrder(*(next - 1), *next);
    }

    set_colors(nullptr);
    apply_zoom();
    update_status();
    hide();

    connect(replace_toggle_, &QToolButton::toggled, this, [this](bool shown) {
        replace_toggle_->set_icon(shown ? "chevron-down" : "chevron-right");
        replace_box_->setVisible(shown);
        replace_->setVisible(shown);
        replace_all_->setVisible(shown);
        place();
    });
    connect(find_edit_, &QLineEdit::textChanged, this, &FindBar::query_changed);
    for (QToolButton* toggle : {case_button_, word_button_, regex_button_}) {
        connect(toggle, &QToolButton::toggled, this, &FindBar::query_changed);
    }
    connect(next_, &QToolButton::clicked, this, &FindBar::find_next);
    connect(previous_, &QToolButton::clicked, this, &FindBar::find_previous);
    connect(close_, &QToolButton::clicked, this, &FindBar::close_bar);
    connect(replace_, &QToolButton::clicked, this, &FindBar::replace_current);
    connect(replace_all_, &QToolButton::clicked, this, &FindBar::replace_all);
    connect(view_, &EditorView::content_changed, this, [this] {
        if (!isHidden()) {
            refresh(false);
        }
    });
    connect(view_, &EditorView::selections_changed, this, [this] {
        if (!isHidden()) {
            update_status();
        }
    });
}

void FindBar::set_zoom(const Zoom* zoom) {
    if (zoom_) {
        disconnect(zoom_, &Zoom::changed, this, nullptr);
    }
    zoom_ = zoom;
    if (zoom_) {
        connect(zoom_, &Zoom::changed, this, &FindBar::apply_zoom);
    }
    apply_zoom();
}

void FindBar::set_colors(const WorkbenchColors* colors) {
    disconnect(colors_, &WorkbenchColors::changed, this, nullptr);
    colors_ = colors ? colors : &palette_colors_;
    connect(colors_, &WorkbenchColors::changed, this, &FindBar::apply_colors);
    apply_colors();
}

void FindBar::apply_colors() {
    for (QLineEdit* field : {find_edit_, replace_edit_}) {
        QPalette colors = field->palette();
        colors.setColor(QPalette::Base, Qt::transparent);
        colors.setColor(QPalette::Text, colors_->color("input.foreground"));
        colors.setColor(QPalette::PlaceholderText, colors_->color("input.placeholderForeground"));
        field->setPalette(colors);
        field->setStyleSheet(QStringLiteral("QLineEdit { background: transparent; border: none; "
                                            "padding: 0; color: %1; }")
                                 .arg(colors_->color("input.foreground").name(QColor::HexArgb)));
    }
    update_status();
    update();
    for (QWidget* part : findChildren<QWidget*>()) {
        part->update();
    }
    if (shadow_) {
        shadow_->update();
    }
}

void FindBar::apply_zoom() {
    QFont bar_font = font();
    bar_font.setPixelSize(zoomed(base_font_pixels_));
    setFont(bar_font);
    // The buttons and fields have fonts of their own that a parent's font does not reach.
    for (QWidget* child : findChildren<QWidget*>()) {
        child->setFont(bar_font);
    }
    count_width_ = 0;
    update_status();
    place();
}

QSize FindBar::sizeHint() const {
    return {zoomed(widget_width),
            zoomed(replace_toggle_->isChecked() ? replace_height : find_height)};
}

void FindBar::place() {
    QRect area(QPoint(), sizeHint());
    const QWidget* host = parentWidget();
    if (host && host->isAncestorOf(view_)) {
        const QRect view(view_->mapTo(host, QPoint()), view_->size());
        // Two widths of the scroll bar from the edge, as VS Code puts it.
        const int right = view.right() + 1 - zoomed(2 * ScrollBar::editor_vertical_width);
        const int room = right - view.left() - zoomed(left_gap_px);
        const int width = std::clamp(room, 0, area.width());
        area = QRect(right - width, view.top() + zoomed(top_gap_px), width, area.height());
    }
    setGeometry(area);
    arrange();
    place_shadow();
}

void FindBar::arrange() {
    const int border = zoomed(border_px);
    const int left = zoomed(field_left_px);
    const int right = width() - zoomed(close_area_px);
    const int row = zoomed(field_height);
    const int button = zoomed(button_size_px);
    const int gap = zoomed(button_gap_px);
    const int top = zoomed(top_gap_px);
    const int count = std::max(count_width_, zoomed(count_width_px));

    int arrows = 2 * (gap + button);
    const bool with_count =
        right - left - arrows - gap - count >= zoomed(field_width_with_count_px);
    const bool with_arrows = right - left - arrows >= zoomed(field_width_with_arrows_px);
    if (!with_arrows) {
        arrows = 0;
    }
    const int box = std::max(0, right - left - arrows - (with_count ? gap + count : 0));

    const int margin = border + zoomed(toggle_margin_px);
    replace_toggle_->setGeometry(border + zoomed(toggle_left_px), margin, zoomed(toggle_width_px),
                                 height() - 2 * margin);
    find_box_->setGeometry(left, top, box, row);
    const int option = zoomed(option_size_px);
    const int step = option + zoomed(option_gap_px);
    const bool with_options = box >= zoomed(field_width_with_count_px);
    int x = box - 3 * step;
    for (Button* toggle : {case_button_, word_button_, regex_button_}) {
        toggle->setGeometry(x, (row - option) / 2, option, option);
        toggle->setVisible(with_options);
        x += step;
    }
    const int padding = zoomed(field_padding_px);
    find_edit_->setGeometry(padding, 0,
                            std::max(0, box - padding - (with_options ? 3 * step : padding)), row);

    x = left + box;
    status_label_->setVisible(with_count);
    if (with_count) {
        status_label_->setGeometry(x + gap, top, count, row);
        x += gap + count;
    }
    for (Button* arrow : {previous_, next_}) {
        arrow->setVisible(with_arrows);
        arrow->setGeometry(x + gap, top + (row - button) / 2, button, button);
        x += gap + button;
    }
    close_->setGeometry(width() - zoomed(close_right_px) - button, zoomed(close_top_px), button,
                        button);

    const int second = top + row + zoomed(row_gap_px);
    replace_box_->setGeometry(left, second, box, row);
    replace_edit_->setGeometry(padding, 0, std::max(0, box - 2 * padding), row);
    x = left + box;
    for (Button* action : {replace_, replace_all_}) {
        action->setGeometry(x + gap, second + (row - button) / 2, button, button);
        x += gap + button;
    }
}

void FindBar::place_shadow() {
    QWidget* host = parentWidget();
    if (!host) {
        return;
    }
    if (!shadow_) {
        shadow_ = new Shadow(*this, host);
    }
    const int size = zoomed(shadow_px);
    shadow_->setGeometry(geometry().adjusted(-size, 0, size, size));
    shadow_->setVisible(!isHidden());
    if (!isHidden()) {
        shadow_->raise();
        raise();
    }
}

void FindBar::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const double radius = zoomed(widget_radius_px);
    painter.setPen(colors_->color("widget.border"));
    painter.setBrush(colors_->color("editorWidget.background"));
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
}

void FindBar::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    place();
}

void FindBar::hideEvent(QHideEvent* event) {
    QWidget::hideEvent(event);
    if (shadow_) {
        shadow_->hide();
    }
}

void FindBar::uncover(const core::Match& match) {
    const QWidget* host = parentWidget();
    if (isHidden() || !host || !host->isAncestorOf(view_)) {
        return;
    }
    // Where the match will be once a smooth scroll has arrived.
    const QPoint at(view_->horizontalScrollBar()->value(), view_->verticalScrollBar()->value());
    QRect found = view_->cursor_rect(match.begin)
                      .united(view_->cursor_rect(match.end))
                      .translated(at - view_->scroll_target());
    found.moveTopLeft(view_->viewport()->mapTo(host, found.topLeft()));
    if (found.intersects(geometry())) {
        view_->scroll_to(view_->scroll_target() - QPoint(0, geometry().bottom() + 1 - found.top()));
    }
}

FindBar::~FindBar() {
    stop_.request_stop();
    // It is a child of the bar's parent and reads the bar when it paints.
    delete shadow_.data();
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
QAbstractButton* FindBar::toggle_replace_button() {
    return replace_toggle_;
}
QAbstractButton* FindBar::previous_button() {
    return previous_;
}
QAbstractButton* FindBar::next_button() {
    return next_;
}
QAbstractButton* FindBar::close_button() {
    return close_;
}
QAbstractButton* FindBar::replace_button() {
    return replace_;
}
QAbstractButton* FindBar::replace_all_button() {
    return replace_all_;
}
QWidget* FindBar::find_box() const {
    return find_box_;
}
QWidget* FindBar::replace_box() const {
    return replace_box_;
}
QWidget* FindBar::count_label() const {
    return status_label_;
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
    refresh(navigate);
}

void FindBar::set_error_state(bool error) {
    find_box_->set_error(error);
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
        select(results.target->match);
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
            select(next == matches_.end() ? matches_.front() : *next);
        } else {
            select(first_at_or_after == matches_.begin() ? matches_.back()
                                                         : *(first_at_or_after - 1));
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
        select((*hit)->match);
    }
}

// An empty match is a caret, so searching on from the caret would find it again.
bool FindBar::is_selected_empty_match(const core::Match& match, const core::Selection& selection) {
    return selection.empty() && match.begin == match.end && match.begin == selection.start();
}

void FindBar::select(const core::Match& match) {
    view_->select_range(to_selection(match));
    uncover(match);
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
    } else if (matches_.empty()) {
        text = tr("No results");
    } else {
        const QString total = truncated_ ? QStringLiteral("%1+").arg(matches_.size())
                                         : QString::number(matches_.size());
        QString index = QStringLiteral("?");
        if (const auto& document = view_->document()) {
            const core::Selection selection = document->selections().primary();
            const core::Match selected{selection.start(), selection.end()};
            const auto found = std::lower_bound(
                matches_.begin(), matches_.end(), selected.begin,
                [](const core::Match& match, std::size_t offset) { return match.begin < offset; });
            if (found != matches_.end() && *found == selected) {
                index = QString::number(found - matches_.begin() + 1);
            }
        }
        text = tr("%1 of %2").arg(index, total);
    }
    status_label_->setText(text);
    status_label_->setToolTip(!query_error_.isEmpty() ? query_error_ : search_error_);

    const bool nothing = !find_edit_->text().isEmpty() && matches_.empty() && !searching_;
    QPalette colors = status_label_->palette();
    colors.setColor(QPalette::WindowText,
                    colors_->color(nothing ? "errorForeground" : "editorWidget.foreground"));
    status_label_->setPalette(colors);

    // As VS Code enables them. A large document is stepped through once its search ends.
    const bool typed = !find_edit_->text().isEmpty();
    previous_->setEnabled(typed && (searching_ || !matches_.empty()));
    next_->setEnabled(typed && (searching_ || !matches_.empty()));
    replace_->setEnabled(typed);
    replace_all_->setEnabled(typed);

    const int width = status_label_->fontMetrics().horizontalAdvance(text) + zoomed(option_gap_px);
    if (width > std::max(count_width_, zoomed(count_width_px))) {
        count_width_ = width;
        arrange();
    }
}

bool FindBar::eventFilter(QObject* watched, QEvent* event) {
    if (watched == view_ && (event->type() == QEvent::Move || event->type() == QEvent::Resize) &&
        !isHidden()) {
        place();
    }
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
    // Tab goes from one field to the other while both show, as in VS Code.
    const bool replacing = replace_toggle_->isChecked();
    if (key->key() == Qt::Key_Tab && watched == find_edit_ && replacing) {
        replace_edit_->setFocus(Qt::TabFocusReason);
        return true;
    }
    if (key->key() == Qt::Key_Backtab && watched == replace_edit_) {
        find_edit_->setFocus(Qt::BacktabFocusReason);
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
