#include "anvil/ui/editor_view.hpp"

#include <QApplication>
#include <QClipboard>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QTextLayout>
#include <QTextOption>
#include <algorithm>

namespace anvil::ui {
namespace {

constexpr int text_padding = 6;
constexpr int gutter_padding = 8;
const QColor match_highlight(255, 190, 0, 90);

// Marks clipboard text that was copied from a lone caret and stands for its line, so that a paste
// inserts it above the line of the caret. It travels with the text and is gone as soon as anything
// else is copied.
constexpr const char* line_marker_format = "application/x-anvil-editor-data";
constexpr const char* line_marker_key = "isFromEmptySelection";

QString to_qstring(const std::string& utf8) {
    return QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size()));
}

// The families of a CSS-style font list: comma separated, each optionally in quotes.
QStringList font_families(const QString& list) {
    QStringList families;
    for (QString name : list.split(',')) {
        name = name.trimmed();
        if (name.size() >= 2 && (name.front() == '\'' || name.front() == '"') &&
            name.back() == name.front()) {
            name = name.sliced(1, name.size() - 2).trimmed();
        }
        if (!name.isEmpty()) {
            families << name;
        }
    }
    return families;
}

// The first installed family of the list, with "monospace" standing for the system's fixed-width
// font, which is also what an uninstalled list falls back to.
QFont font_for(const std::string& family_list, int pixel_size) {
    const QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    QFont font = fixed;
    const QStringList installed = QFontDatabase::families();
    for (const QString& family : font_families(to_qstring(family_list))) {
        if (family.compare(QLatin1String("monospace"), Qt::CaseInsensitive) == 0) {
            break;
        }
        const auto match =
            std::find_if(installed.begin(), installed.end(), [&](const QString& name) {
                return name.compare(family, Qt::CaseInsensitive) == 0;
            });
        if (match != installed.end()) {
            font.setFamily(*match);
            break;
        }
    }
    font.setPixelSize(pixel_size);
    return font;
}

// The column that the end of `prefix` is shown at, where a tab advances to the next tab stop.
std::size_t visual_column(std::string_view prefix, std::size_t tab_size) {
    std::size_t column = 0;
    for (const char c : prefix) {
        if (c == '\t') {
            column += tab_size - column % tab_size;
        } else if ((static_cast<unsigned char>(c) & 0xC0u) != 0x80u) {
            ++column;
        }
    }
    return column;
}

bool is_marked_as_line(const QMimeData* data) {
    return data != nullptr && data->hasFormat(line_marker_format) &&
           QJsonDocument::fromJson(data->data(line_marker_format))
               .object()
               .value(line_marker_key)
               .toBool();
}

} // namespace

struct EditorView::LineLayout {
    QString text;
    QTextLayout layout;

    // The number of UTF-16 code units shown.
    std::size_t size() const {
        return static_cast<std::size_t>(text.size());
    }
    // The x coordinate of a UTF-16 column, relative to the start of the line.
    qreal x_of(std::size_t column) const {
        const QTextLine line = layout.lineAt(0);
        return line.isValid() ? line.cursorToX(static_cast<int>(std::min(column, size()))) : 0;
    }
    // The UTF-16 column nearest to an x coordinate.
    std::size_t column_at(qreal x) const {
        const QTextLine line = layout.lineAt(0);
        return line.isValid() ? static_cast<std::size_t>(std::max(0, line.xToCursor(x))) : 0;
    }
};

EditorView::EditorView(QWidget* parent) : QAbstractScrollArea(parent) {
    apply_settings(core::EditorSettings{});

    viewport()->setCursor(Qt::IBeamCursor);
    viewport()->setAttribute(Qt::WA_InputMethodEnabled);
    setAttribute(Qt::WA_InputMethodEnabled);
    setFocusPolicy(Qt::StrongFocus);
    setFrameShape(QFrame::NoFrame);
    verticalScrollBar()->setSingleStep(line_height_);
    horizontalScrollBar()->setSingleStep(char_width_ * 4);

    connect(verticalScrollBar(), &QScrollBar::valueChanged, viewport(),
            qOverload<>(&QWidget::update));
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, viewport(),
            qOverload<>(&QWidget::update));
    caret_timer_.setInterval(std::max(QApplication::cursorFlashTime() / 2, 100));
    connect(&caret_timer_, &QTimer::timeout, this, [this] {
        caret_visible_ = !caret_visible_;
        viewport()->update();
    });
}

EditorView::~EditorView() = default;

void EditorView::update_metrics() {
    const QFontMetrics metrics(font());
    line_height_ = metrics.lineSpacing();
    char_width_ = std::max(1, metrics.horizontalAdvance(QLatin1Char('M')));
}

void EditorView::apply_settings(const core::EditorSettings& settings) {
    // Scroll positions are in pixels, so they are converted to lines and columns and back to keep
    // the same text in view when the metrics change.
    const int vertical = verticalScrollBar()->value();
    const int horizontal = horizontalScrollBar()->value();
    const int first_line = vertical / line_height_;
    const int first_column = horizontal / char_width_;
    const int previous_line_height = line_height_;
    const int previous_char_width = char_width_;

    tab_size_ = static_cast<std::size_t>(settings.tab_size);
    insert_spaces_ = settings.insert_spaces;
    const QFont wanted = font_for(settings.font_family, settings.font_size);
    if (wanted != font()) {
        setFont(wanted);
    }
    update_metrics();
    // The widest line is only measured while painting, so until the next paint the old width,
    // scaled to the new font, keeps the horizontal range from collapsing to nothing.
    max_line_width_ = max_line_width_ * char_width_ / previous_char_width;
    verticalScrollBar()->setSingleStep(line_height_);
    horizontalScrollBar()->setSingleStep(char_width_ * 4);
    update_scroll_bars();
    // A reload that leaves the metrics alone, such as a change of color theme, keeps the exact
    // position instead of snapping to whole lines.
    const bool same_metrics =
        line_height_ == previous_line_height && char_width_ == previous_char_width;
    verticalScrollBar()->setValue(same_metrics ? vertical : first_line * line_height_);
    horizontalScrollBar()->setValue(same_metrics ? horizontal : first_column * char_width_);
    viewport()->update();
}

void EditorView::set_document(std::shared_ptr<core::Document> document) {
    document_ = std::move(document);
    controller_ = document_ ? std::make_unique<core::EditController>(*document_) : nullptr;
    max_line_width_ = 0;
    highlights_.clear();
    verticalScrollBar()->setValue(0);
    horizontalScrollBar()->setValue(0);
    update_scroll_bars();
    viewport()->update();
    emit content_changed();
}

void EditorView::document_changed_elsewhere() {
    update_scroll_bars();
    viewport()->update();
    emit content_changed();
}

void EditorView::select_range(core::Selection range) {
    if (controller_) {
        apply(controller_->select(range));
        ensure_cursor_visible();
    }
}

void EditorView::replace_ranges(std::vector<core::Edit> edits,
                                std::optional<core::Selection> selection_after) {
    if (controller_) {
        apply(controller_->replace_ranges(std::move(edits), selection_after));
    }
}

void EditorView::set_highlights(std::vector<core::Match> highlights) {
    highlights_ = std::move(highlights);
    viewport()->update();
}

int EditorView::gutter_width() const {
    if (!document_) {
        return 0;
    }
    const int digits =
        std::max(3, static_cast<int>(QString::number(document_->text().line_count()).size()));
    return digits * char_width_ + 2 * gutter_padding;
}

int EditorView::text_origin_x() const {
    return gutter_width() + text_padding - horizontalScrollBar()->value();
}

std::unique_ptr<EditorView::LineLayout> EditorView::layout_line(std::size_t line) const {
    auto result = std::make_unique<LineLayout>();
    result->text = to_qstring(document_->text().line(line));
    if (result->text.size() > max_line_characters) {
        result->text.truncate(max_line_characters);
    }
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    option.setTabStopDistance(char_width_ * static_cast<qreal>(tab_size_));
    result->layout.setText(result->text);
    result->layout.setFont(font());
    result->layout.setTextOption(option);
    result->layout.beginLayout();
    QTextLine text_line = result->layout.createLine();
    if (text_line.isValid()) {
        text_line.setLineWidth(1 << 24);
        text_line.setPosition(QPointF(0, 0));
    }
    result->layout.endLayout();
    return result;
}

QRect EditorView::cursor_rect(std::size_t offset) const {
    if (!document_) {
        return {};
    }
    const core::Text& text = document_->text();
    const core::Position position = text.position_at(offset);
    const auto layout = layout_line(position.line);
    const int x = text_origin_x() + static_cast<int>(layout->x_of(position.column));
    const int y = static_cast<int>(position.line) * line_height_ - verticalScrollBar()->value();
    return QRect(x, y, 2, line_height_);
}

std::size_t EditorView::offset_at(const QPoint& point) const {
    if (!document_) {
        return 0;
    }
    const core::Text& text = document_->text();
    const int y = std::max(0, point.y() + verticalScrollBar()->value());
    const std::size_t line =
        std::min<std::size_t>(static_cast<std::size_t>(y / line_height_), text.line_count() - 1);
    const auto layout = layout_line(line);
    const std::size_t column = layout->column_at(point.x() - text_origin_x());
    return text.offset_at({line, column});
}

void EditorView::update_scroll_bars() {
    const int lines = document_ ? static_cast<int>(document_->text().line_count()) : 0;
    verticalScrollBar()->setRange(
        0, std::max(0, lines * line_height_ - viewport()->height() + line_height_));
    verticalScrollBar()->setPageStep(viewport()->height());
    const int available = viewport()->width() - gutter_width() - 2 * text_padding;
    horizontalScrollBar()->setRange(0, std::max(0, max_line_width_ - available));
    horizontalScrollBar()->setPageStep(available);
}

void EditorView::ensure_cursor_visible() {
    if (!document_) {
        return;
    }
    const std::size_t active = document_->selections().primary().active;
    const core::Position position = document_->text().position_at(active);
    const int top = static_cast<int>(position.line) * line_height_;
    QScrollBar* vertical = verticalScrollBar();
    if (top < vertical->value()) {
        vertical->setValue(top);
    } else if (top + line_height_ > vertical->value() + viewport()->height()) {
        vertical->setValue(top + line_height_ - viewport()->height());
    }

    const auto layout = layout_line(position.line);
    const int x = static_cast<int>(layout->x_of(position.column));
    const int available = viewport()->width() - gutter_width() - 2 * text_padding;
    QScrollBar* horizontal = horizontalScrollBar();
    if (x < horizontal->value()) {
        horizontal->setValue(x);
    } else if (x + 2 > horizontal->value() + available) {
        horizontal->setValue(x + 2 - available);
    }
}

void EditorView::restart_caret_blink() {
    caret_visible_ = true;
    if (hasFocus() && QApplication::cursorFlashTime() > 0) {
        caret_timer_.start();
    }
}

void EditorView::apply(const core::EditOutcome& outcome) {
    if (!outcome.changed_anything()) {
        return;
    }
    if (outcome.change) {
        update_scroll_bars();
    }
    ensure_cursor_visible();
    restart_caret_blink();
    viewport()->update();
    if (outcome.change) {
        emit text_changed(*outcome.change);
        emit content_changed();
    }
    if (outcome.selections_changed) {
        emit selections_changed();
    }
}

void EditorView::move(core::Motion motion, bool extend) {
    if (controller_) {
        apply(controller_->move(motion, extend));
    }
}

void EditorView::delete_backward() {
    if (controller_) {
        apply(controller_->delete_backward());
    }
}

void EditorView::delete_forward() {
    if (controller_) {
        apply(controller_->delete_forward());
    }
}

void EditorView::move_by_page(bool down, bool extend) {
    if (!controller_) {
        return;
    }
    const int lines = std::max(1, viewport()->height() / line_height_ - 1);
    for (int i = 0; i < lines; ++i) {
        apply(controller_->move(down ? core::Motion::down : core::Motion::up, extend));
    }
}

void EditorView::undo() {
    if (controller_) {
        apply(controller_->undo());
    }
}

void EditorView::redo() {
    if (controller_) {
        apply(controller_->redo());
    }
}

void EditorView::copy() {
    if (!controller_) {
        return;
    }
    const core::CopiedText copied = controller_->copied_text();
    if (copied.text.empty()) {
        return;
    }
    auto* data = new QMimeData;
    data->setText(to_qstring(copied.text));
    if (copied.whole_line) {
        data->setData(
            line_marker_format,
            QJsonDocument(QJsonObject{{line_marker_key, true}}).toJson(QJsonDocument::Compact));
    }
    QGuiApplication::clipboard()->setMimeData(data);
}

void EditorView::cut() {
    if (!controller_) {
        return;
    }
    const core::CopiedText copied = controller_->copied_text();
    if (copied.text.empty()) {
        return;
    }
    copy();
    apply(copied.whole_line ? controller_->delete_caret_line() : controller_->delete_selection());
}

void EditorView::paste() {
    if (controller_) {
        const QClipboard* clipboard = QGuiApplication::clipboard();
        apply(controller_->paste(clipboard->text().toUtf8().toStdString(),
                                 is_marked_as_line(clipboard->mimeData())));
    }
}

void EditorView::select_all() {
    if (controller_) {
        apply(controller_->select_all());
    }
}

void EditorView::insert_tab() {
    if (!insert_spaces_) {
        apply(controller_->type("\t"));
        return;
    }
    // Spaces up to the next tab stop of the primary caret.
    const core::Text& text = document_->text();
    const std::size_t caret = document_->selections().primary().active;
    const std::size_t line_start = text.line_start(text.position_at(caret).line);
    const std::size_t column = visual_column(text.slice(line_start, caret - line_start), tab_size_);
    apply(controller_->type(std::string(tab_size_ - column % tab_size_, ' ')));
}

bool EditorView::focusNextPrevChild(bool) {
    return false;
}

void EditorView::keyPressEvent(QKeyEvent* event) {
    if (!controller_) {
        QAbstractScrollArea::keyPressEvent(event);
        return;
    }
    struct Binding {
        QKeySequence::StandardKey key;
        core::Motion motion;
        bool extend;
    };
    static const Binding motions[] = {
        {QKeySequence::MoveToNextChar, core::Motion::right, false},
        {QKeySequence::MoveToPreviousChar, core::Motion::left, false},
        {QKeySequence::MoveToNextLine, core::Motion::down, false},
        {QKeySequence::MoveToPreviousLine, core::Motion::up, false},
        {QKeySequence::MoveToStartOfLine, core::Motion::line_start, false},
        {QKeySequence::MoveToEndOfLine, core::Motion::line_end, false},
        {QKeySequence::MoveToStartOfDocument, core::Motion::document_start, false},
        {QKeySequence::MoveToEndOfDocument, core::Motion::document_end, false},
        {QKeySequence::MoveToNextWord, core::Motion::word_right, false},
        {QKeySequence::MoveToPreviousWord, core::Motion::word_left, false},
        {QKeySequence::SelectNextChar, core::Motion::right, true},
        {QKeySequence::SelectPreviousChar, core::Motion::left, true},
        {QKeySequence::SelectNextLine, core::Motion::down, true},
        {QKeySequence::SelectPreviousLine, core::Motion::up, true},
        {QKeySequence::SelectStartOfLine, core::Motion::line_start, true},
        {QKeySequence::SelectEndOfLine, core::Motion::line_end, true},
        {QKeySequence::SelectStartOfDocument, core::Motion::document_start, true},
        {QKeySequence::SelectEndOfDocument, core::Motion::document_end, true},
        {QKeySequence::SelectNextWord, core::Motion::word_right, true},
        {QKeySequence::SelectPreviousWord, core::Motion::word_left, true},
    };
    for (const Binding& binding : motions) {
        if (event->matches(binding.key)) {
            move(binding.motion, binding.extend);
            return;
        }
    }

    if (event->matches(QKeySequence::MoveToNextPage) ||
        event->matches(QKeySequence::SelectNextPage)) {
        move_by_page(true, event->matches(QKeySequence::SelectNextPage));
    } else if (event->matches(QKeySequence::MoveToPreviousPage) ||
               event->matches(QKeySequence::SelectPreviousPage)) {
        move_by_page(false, event->matches(QKeySequence::SelectPreviousPage));
    } else if (event->matches(QKeySequence::Undo)) {
        undo();
    } else if (event->matches(QKeySequence::Redo)) {
        redo();
    } else if (event->matches(QKeySequence::Cut)) {
        cut();
    } else if (event->matches(QKeySequence::Copy)) {
        copy();
    } else if (event->matches(QKeySequence::Paste)) {
        paste();
    } else if (event->matches(QKeySequence::SelectAll)) {
        select_all();
    } else if (event->matches(QKeySequence::Delete)) {
        delete_forward();
    } else if (event->key() == Qt::Key_Backspace) {
        delete_backward();
    } else if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        apply(controller_->insert_newline());
    } else if (event->key() == Qt::Key_Tab && event->modifiers() == Qt::NoModifier) {
        insert_tab();
    } else if (event->key() == Qt::Key_Backtab) {
        // Left to the commands that will outdent; unhandled, it would move the focus out.
    } else if (event->key() == Qt::Key_Escape) {
        apply(controller_->keep_primary_selection());
    } else {
        const QString text = event->text();
        const bool has_command_modifier =
            event->modifiers() & (Qt::ControlModifier | Qt::MetaModifier);
        // The first code point, not the first UTF-16 unit: half of a surrogate pair is not
        // printable.
        if (!text.isEmpty() && QChar::isPrint(text.toUcs4().first()) && !has_command_modifier) {
            apply(controller_->type(text.toUtf8().toStdString()));
        } else {
            QAbstractScrollArea::keyPressEvent(event);
        }
        return;
    }
    event->accept();
}

void EditorView::mousePressEvent(QMouseEvent* event) {
    if (!controller_ || event->button() != Qt::LeftButton) {
        QAbstractScrollArea::mousePressEvent(event);
        return;
    }
    setFocus();
    const std::size_t offset = offset_at(event->position().toPoint());
    if (since_double_click_.isValid() &&
        since_double_click_.elapsed() < QApplication::doubleClickInterval()) {
        since_double_click_.invalidate();
        apply(controller_->select_line_at(offset)); // third click
        return;
    }
    if (event->modifiers() & Qt::AltModifier) {
        apply(controller_->add_caret(offset));
    } else {
        apply(controller_->set_caret(offset, event->modifiers() & Qt::ShiftModifier));
    }
    selecting_with_mouse_ = true;
}

void EditorView::mouseMoveEvent(QMouseEvent* event) {
    if (controller_ && selecting_with_mouse_) {
        apply(controller_->set_caret(offset_at(event->position().toPoint()), true));
    }
}

void EditorView::mouseReleaseEvent(QMouseEvent* event) {
    selecting_with_mouse_ = false;
    QAbstractScrollArea::mouseReleaseEvent(event);
}

void EditorView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (!controller_ || event->button() != Qt::LeftButton) {
        return;
    }
    apply(controller_->select_word_at(offset_at(event->position().toPoint())));
    since_double_click_.start();
    selecting_with_mouse_ = false;
}

void EditorView::inputMethodEvent(QInputMethodEvent* event) {
    if (controller_ && !event->commitString().isEmpty()) {
        apply(controller_->type(event->commitString().toUtf8().toStdString()));
    }
    preedit_ = event->preeditString();
    viewport()->update();
    event->accept();
}

QVariant EditorView::inputMethodQuery(Qt::InputMethodQuery query) const {
    if (!document_) {
        return QAbstractScrollArea::inputMethodQuery(query);
    }
    const std::size_t active = document_->selections().primary().active;
    switch (query) {
    case Qt::ImCursorRectangle:
        return cursor_rect(active);
    case Qt::ImFont:
        return font();
    case Qt::ImCursorPosition:
        return static_cast<int>(document_->text().position_at(active).column);
    case Qt::ImSurroundingText:
        return to_qstring(document_->text().line(document_->text().position_at(active).line));
    default:
        return QAbstractScrollArea::inputMethodQuery(query);
    }
}

void EditorView::resizeEvent(QResizeEvent* event) {
    QAbstractScrollArea::resizeEvent(event);
    update_scroll_bars();
}

void EditorView::focusInEvent(QFocusEvent* event) {
    QAbstractScrollArea::focusInEvent(event);
    restart_caret_blink();
    viewport()->update();
}

void EditorView::focusOutEvent(QFocusEvent* event) {
    QAbstractScrollArea::focusOutEvent(event);
    caret_timer_.stop();
    caret_visible_ = true;
    viewport()->update();
}

void EditorView::changeEvent(QEvent* event) {
    QAbstractScrollArea::changeEvent(event);
    if (event->type() == QEvent::FontChange) {
        update_metrics();
        verticalScrollBar()->setSingleStep(line_height_);
        horizontalScrollBar()->setSingleStep(char_width_ * 4);
        update_scroll_bars();
    }
}

struct EditorView::Colors {
    QColor background;
    QColor foreground;
    QColor selection_background;
    QColor selection_foreground;
    QColor line_highlight;
    QColor line_number;
    QColor line_number_active;
    QColor cursor;
    QColor gutter_background;
    QColor find_match;
    QColor find_highlight;
};

void EditorView::set_colors(const WorkbenchColors* colors) {
    QObject::disconnect(colors_connection_);
    colors_ = colors;
    if (colors_) {
        colors_connection_ =
            connect(colors_, &WorkbenchColors::changed, this, [this] { viewport()->update(); });
    }
    viewport()->update();
}

EditorView::Colors EditorView::current_colors() const {
    if (!colors_) {
        const QPalette& palette = this->palette();
        return {palette.color(QPalette::Base),
                palette.color(QPalette::Text),
                palette.color(QPalette::Highlight),
                palette.color(QPalette::HighlightedText),
                palette.color(QPalette::AlternateBase),
                palette.color(QPalette::PlaceholderText),
                palette.color(QPalette::Text),
                palette.color(QPalette::Text),
                palette.color(QPalette::Window),
                match_highlight,
                match_highlight};
    }
    const QColor foreground = colors_->color("editor.foreground");
    return {colors_->color("editor.background"),
            foreground,
            colors_->color("editor.selectionBackground"),
            colors_->is_themed("editor.selectionBackground")
                ? foreground
                : palette().color(QPalette::HighlightedText),
            colors_->color("editor.lineHighlightBackground"),
            colors_->color("editorLineNumber.foreground"),
            colors_->color("editorLineNumber.activeForeground"),
            colors_->color("editorCursor.foreground"),
            colors_->color("editorGutter.background"),
            colors_->color("editor.findMatchBackground"),
            colors_->color("editor.findMatchHighlightBackground")};
}

void EditorView::paintEvent(QPaintEvent*) {
    QPainter painter(viewport());
    const Colors colors = current_colors();
    painter.fillRect(viewport()->rect(), colors.background);
    if (!document_) {
        return;
    }

    const core::Text& text = document_->text();
    const auto& selections = document_->selections().all();
    const int scroll_y = verticalScrollBar()->value();
    const std::size_t first = static_cast<std::size_t>(scroll_y / line_height_);
    const std::size_t last =
        std::min(text.line_count(),
                 static_cast<std::size_t>((scroll_y + viewport()->height()) / line_height_) + 1);
    const std::size_t active_line = text.position_at(document_->selections().primary().active).line;
    const int origin_x = text_origin_x();
    const QColor selection_background = colors.selection_background;
    const QColor selection_foreground = colors.selection_foreground;
    const core::Selection primary = document_->selections().primary();
    int widest = max_line_width_;

    painter.setFont(font());
    for (std::size_t line = first; line < last; ++line) {
        const int y = static_cast<int>(line) * line_height_ - scroll_y;
        const QRect row(0, y, viewport()->width(), line_height_);
        if (line == active_line && selections.size() == 1 && selections[0].empty()) {
            painter.fillRect(row, colors.line_highlight);
        }

        const auto layout = layout_line(line);
        const std::size_t line_begin = text.line_start(line);
        const std::size_t line_end = text.line_end(line);
        const std::size_t line_units = text.utf16_offset(line_begin);

        QList<QTextLayout::FormatRange> formats;
        // Highlights go first so that a selection over a highlighted match is drawn on top.
        auto highlight = std::lower_bound(
            highlights_.begin(), highlights_.end(), line_begin,
            [](const core::Match& match, std::size_t offset) { return match.end <= offset; });
        for (; highlight != highlights_.end() && highlight->begin <= line_end; ++highlight) {
            const std::size_t from = std::max(highlight->begin, line_begin);
            const std::size_t to = std::min(highlight->end, line_end);
            if (from < to) {
                QTextCharFormat format;
                // The match the selection is on is the current one.
                const bool current =
                    highlight->begin == primary.start() && highlight->end == primary.end();
                format.setBackground(current ? colors.find_match : colors.find_highlight);
                const int start_column = static_cast<int>(text.utf16_offset(from) - line_units);
                const int end_column = static_cast<int>(text.utf16_offset(to) - line_units);
                formats.push_back(
                    {start_column,
                     std::min(end_column, static_cast<int>(layout->size())) - start_column,
                     format});
            }
        }
        for (const core::Selection& selection : selections) {
            const std::size_t from = std::max(selection.start(), line_begin);
            const std::size_t to = std::min(selection.end(), line_end);
            if (from < to) {
                QTextCharFormat format;
                format.setBackground(selection_background);
                format.setForeground(selection_foreground);
                const int start_column = static_cast<int>(text.utf16_offset(from) - line_units);
                const int end_column = static_cast<int>(text.utf16_offset(to) - line_units);
                formats.push_back(
                    {start_column,
                     std::min(end_column, static_cast<int>(layout->size())) - start_column,
                     format});
            }
            // A selection that continues past the line break shows a small block after the text.
            if (selection.start() <= line_end && selection.end() > line_end &&
                line + 1 < text.line_count()) {
                const int x = origin_x + static_cast<int>(layout->x_of(layout->size()));
                painter.fillRect(QRect(x, y, char_width_ / 2 + 2, line_height_),
                                 selection_background);
            }
        }

        painter.setPen(colors.foreground);
        layout->layout.draw(&painter, QPointF(origin_x, y), formats);
        widest = std::max(widest, static_cast<int>(layout->x_of(layout->size())) + char_width_);

        if (caret_visible_ && hasFocus()) {
            for (const core::Selection& selection : selections) {
                if (selection.active < line_begin || selection.active > line_end) {
                    continue;
                }
                const std::size_t column = text.utf16_offset(selection.active) - line_units;
                const int x = origin_x + static_cast<int>(layout->x_of(column));
                painter.fillRect(QRect(x, y, 2, line_height_), colors.cursor);
                if (!preedit_.isEmpty() &&
                    selection.active == document_->selections().primary().active) {
                    QFont underlined = font();
                    underlined.setUnderline(true);
                    painter.setFont(underlined);
                    painter.drawText(QPoint(x + 2, y + QFontMetrics(font()).ascent()), preedit_);
                    painter.setFont(font());
                }
            }
        }
    }

    // The gutter covers the horizontally scrolled text and carries the line numbers.
    const int gutter = gutter_width();
    painter.fillRect(QRect(0, 0, gutter, viewport()->height()), colors.gutter_background);
    for (std::size_t line = first; line < last; ++line) {
        const int y = static_cast<int>(line) * line_height_ - scroll_y;
        painter.setPen(line == active_line ? colors.line_number_active : colors.line_number);
        painter.drawText(QRect(0, y, gutter - gutter_padding, line_height_),
                         Qt::AlignRight | Qt::AlignVCenter, QString::number(line + 1));
    }

    if (widest != max_line_width_) {
        const_cast<EditorView*>(this)->max_line_width_ = widest;
        const_cast<EditorView*>(this)->update_scroll_bars();
    }
}

} // namespace anvil::ui
