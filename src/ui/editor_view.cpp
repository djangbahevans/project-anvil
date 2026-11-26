#include "anvil/ui/editor_view.hpp"

#include "anvil/core/zoom.hpp"
#include "anvil/ui/zoom.hpp"

#include <QApplication>
#include <QClipboard>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QJsonArray>
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

QColor to_color(const core::Rgba& color) {
    return QColor(color.red, color.green, color.blue, color.alpha);
}

constexpr int text_padding_px = 6;
constexpr int gutter_padding_px = 8;
const QColor match_highlight(255, 190, 0, 90);
// VS Code draws a control character in white on this red, whatever the theme.
const QColor control_background(150, 0, 0);

// Marks clipboard text that was copied from a lone caret and stands for its line, so that a paste
// inserts it above the line of the caret. It travels with the text and is gone as soon as anything
// else is copied.
constexpr const char* line_marker_format = "application/x-anvil-editor-data";
constexpr const char* line_marker_key = "isFromEmptySelection";
constexpr const char* pieces_key = "multicursorText";

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

std::vector<std::string> copied_pieces(const QMimeData* data) {
    std::vector<std::string> pieces;
    if (data == nullptr || !data->hasFormat(line_marker_format)) {
        return pieces;
    }
    const QJsonValue value =
        QJsonDocument::fromJson(data->data(line_marker_format)).object().value(pieces_key);
    if (!value.isArray()) {
        return pieces;
    }
    for (const QJsonValue piece : value.toArray()) {
        if (!piece.isString()) {
            return {};
        }
        pieces.push_back(piece.toString().toStdString());
    }
    return pieces;
}

} // namespace

struct EditorView::LineLayout {
    QString text;
    QTextLayout layout;
    // The UTF-16 columns of the control characters, which `text` shows as symbols.
    std::vector<int> control_columns;
    // The digest of the highlighter's spans that the formats were made from, when there are any.
    std::optional<std::size_t> spans;

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
    clear_layouts();
    const int first_line = vertical / line_height_;
    const int first_column = horizontal / char_width_;
    const int previous_line_height = line_height_;
    const int previous_char_width = char_width_;

    tab_size_ = static_cast<std::size_t>(settings.tab_size);
    insert_spaces_ = settings.insert_spaces;
    render_whitespace_ = settings.render_whitespace;
    render_control_characters_ = settings.render_control_characters;
    multi_cursor_modifier_ = settings.multi_cursor_modifier;
    column_selection_ = settings.column_selection;
    text_padding_ = core::scale_for_zoom(text_padding_px, settings.zoom_level);
    gutter_padding_ = core::scale_for_zoom(gutter_padding_px, settings.zoom_level);
    scale_scroll_bars(*this, settings.zoom_level);
    const QFont wanted = font_for(settings.font_family,
                                  core::scale_for_zoom(settings.font_size, settings.zoom_level));
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
    clear_layouts();
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

void EditorView::select_ranges(std::vector<core::Selection> ranges, std::size_t primary) {
    if (controller_) {
        apply(controller_->select_ranges(std::move(ranges), primary));
        ensure_cursor_visible();
    }
}

void EditorView::replace_ranges(std::vector<core::Edit> edits,
                                std::optional<core::Selection> selection_after) {
    if (controller_) {
        apply(controller_->replace_ranges(std::move(edits), selection_after));
    }
}

void EditorView::set_highlighter(std::shared_ptr<const core::Highlighter> highlighter) {
    highlighter_ = std::move(highlighter);
    clear_layouts();
    painted_highlighter_version_ = highlighter_ ? highlighter_->version() : 0;
    viewport()->update();
}

void EditorView::refresh_highlighting() {
    if (!highlighter_ || highlighter_->version() == painted_highlighter_version_) {
        return;
    }
    painted_highlighter_version_ = highlighter_->version();
    if (!document_ || line_height_ <= 0) {
        viewport()->update();
        return;
    }
    const std::size_t line_count = document_->text().line_count();
    const std::size_t first = static_cast<std::size_t>(verticalScrollBar()->value() / line_height_);
    const std::size_t last = std::min(
        line_count, static_cast<std::size_t>((verticalScrollBar()->value() + viewport()->height()) /
                                             line_height_) +
                        1);
    QRegion changed;
    const auto repaint = [&](std::size_t line) {
        const int y = static_cast<int>(line) * line_height_ - verticalScrollBar()->value();
        changed += QRect(0, y, viewport()->width(), line_height_);
    };
    // Only the lines in view are asked for spans; a layout made from spans elsewhere is dropped
    // and made again when it comes into view.
    for (auto layout = layouts_.begin(); layout != layouts_.end();) {
        const auto& [line, cached] = *layout;
        const bool in_view = line >= first && line < last;
        if (cached.layout->spans && (!in_view || *cached.layout->spans != span_digest(line))) {
            if (in_view) {
                repaint(line);
            }
            layout = layouts_.erase(layout);
        } else {
            ++layout;
        }
    }
    for (std::size_t line = first; line < last; ++line) {
        if (!layouts_.contains(line)) {
            repaint(line);
        }
    }
    if (!changed.isEmpty()) {
        viewport()->update(changed);
    }
}

std::size_t EditorView::span_digest(std::size_t line) const {
    std::size_t digest = 0;
    for (const core::HighlightSpan& span : highlighter_->spans(line)) {
        digest = digest * 1000003 ^ (span.begin * 31 + span.end);
        digest = digest * 1000003 ^ std::hash<std::string_view>{}(span.scope);
    }
    return digest;
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
    return digits * char_width_ + 2 * gutter_padding_;
}

int EditorView::text_origin_x() const {
    return gutter_width() + text_padding_ - horizontalScrollBar()->value();
}

QList<QTextLayout::FormatRange> EditorView::syntax_formats(const core::TokenTheme& token_theme,
                                                           std::size_t line, qsizetype size) const {
    QList<QTextLayout::FormatRange> formats;
    const std::string_view root = highlighter_->root_scope();
    for (const core::HighlightSpan& span : highlighter_->spans(line)) {
        const qsizetype begin = static_cast<qsizetype>(span.begin);
        const qsizetype end = std::min(static_cast<qsizetype>(span.end), size);
        if (begin >= end) {
            continue;
        }
        const std::string_view stack[] = {root, span.scope};
        const core::TokenStyle style = token_theme.style_for(stack);
        QTextCharFormat format;
        if (style.foreground) {
            format.setForeground(to_color(*style.foreground));
        }
        if (style.background) {
            format.setBackground(to_color(*style.background));
        }
        if (const std::uint8_t bits = style.font_style.value_or(0); bits != 0) {
            format.setFontWeight((bits & core::FontStyle::bold) ? QFont::Bold : QFont::Normal);
            format.setFontItalic((bits & core::FontStyle::italic) != 0);
            format.setFontUnderline((bits & core::FontStyle::underline) != 0);
            format.setFontStrikeOut((bits & core::FontStyle::strikethrough) != 0);
        }
        if (format.properties().isEmpty()) {
            continue;
        }
        formats.push_back({static_cast<int>(begin), static_cast<int>(end - begin), format});
    }
    return formats;
}

std::shared_ptr<const EditorView::LineLayout> EditorView::layout_line(std::size_t line) const {
    if (layout_version_ != document_->version()) {
        layouts_.clear();
        layout_version_ = document_->version();
    }
    const auto found = layouts_.find(line);
    if (found != layouts_.end()) {
        found->second.used = ++layout_clock_;
        return found->second.layout;
    }
    auto layout = build_layout(line);
    const std::size_t capacity = layout_capacity();
    if (layouts_.size() >= capacity) {
        std::vector<std::uint64_t> stamps;
        stamps.reserve(layouts_.size());
        for (const auto& entry : layouts_) {
            stamps.push_back(entry.second.used);
        }
        const auto quarter = stamps.begin() + static_cast<std::ptrdiff_t>(stamps.size() / 4);
        std::nth_element(stamps.begin(), quarter, stamps.end());
        std::erase_if(layouts_, [&](const auto& entry) { return entry.second.used < *quarter; });
    }
    layouts_[line] = {layout, ++layout_clock_};
    return layout;
}

std::size_t EditorView::layout_capacity() const {
    const std::size_t in_view = static_cast<std::size_t>(viewport()->height() / line_height_ + 2);
    return std::max<std::size_t>(256, 2 * in_view);
}

void EditorView::clear_layouts() {
    layouts_.clear();
}

void EditorView::update_layouts(const core::DocumentChange& change) {
    if (layout_version_ + 1 != change.version) {
        layouts_.clear();
        layout_version_ = change.version;
        return;
    }
    layout_version_ = change.version;
    for (const core::TextChange& edit : change.changes) {
        const std::size_t first = edit.range.start.line;
        const std::size_t last = edit.range.end.line;
        const std::size_t inserted = static_cast<std::size_t>(std::ranges::count(edit.text, '\n'));
        std::erase_if(layouts_, [&](const auto& entry) {
            return entry.first >= first && entry.first <= last;
        });
        if (inserted == last - first) {
            continue;
        }
        std::vector<std::pair<std::size_t, CachedLayout>> moved;
        for (auto it = layouts_.begin(); it != layouts_.end();) {
            if (it->first > last) {
                moved.emplace_back(it->first - last + first + inserted, std::move(it->second));
                it = layouts_.erase(it);
            } else {
                ++it;
            }
        }
        layouts_.insert(std::make_move_iterator(moved.begin()),
                        std::make_move_iterator(moved.end()));
    }
}

std::shared_ptr<const EditorView::LineLayout> EditorView::build_layout(std::size_t line) const {
    ++layouts_built_;
    auto result = std::make_shared<LineLayout>();
    result->text = to_qstring(document_->text().line(line));
    if (result->text.size() > max_line_characters) {
        result->text.truncate(max_line_characters);
    }
    if (render_control_characters_) {
        for (int i = 0; i < result->text.size(); ++i) {
            const char16_t c = result->text[i].unicode();
            if ((c < 0x20 && c != u'\t') || c == 0x7f) {
                // The Control Pictures block has one symbol for each, as many columns as the
                // character.
                result->text[i] = QChar(c == 0x7f ? 0x2421 : 0x2400 + c);
                result->control_columns.push_back(i);
            }
        }
    }
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    option.setTabStopDistance(char_width_ * static_cast<qreal>(tab_size_));
    result->layout.setText(result->text);
    result->layout.setFont(font());
    result->layout.setTextOption(option);
    if (highlighter_ && colors_ && colors_->token_theme()) {
        // Set before the layout, since only then do font styles such as bold and italic apply.
        result->layout.setFormats(
            syntax_formats(*colors_->token_theme(), line, result->text.size()));
        result->spans = span_digest(line);
    }
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
    const int available = viewport()->width() - gutter_width() - 2 * text_padding_;
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
    const int available = viewport()->width() - gutter_width() - 2 * text_padding_;
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
        update_layouts(*outcome.change);
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
    if (copied.whole_line || !copied.pieces.empty()) {
        QJsonObject metadata{{line_marker_key, copied.whole_line}};
        if (!copied.pieces.empty()) {
            QJsonArray pieces;
            for (const std::string& piece : copied.pieces) {
                pieces.append(to_qstring(piece));
            }
            metadata.insert(pieces_key, pieces);
        }
        data->setData(line_marker_format, QJsonDocument(metadata).toJson(QJsonDocument::Compact));
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
    apply(controller_->delete_cut());
}

void EditorView::paste() {
    if (controller_) {
        const QClipboard* clipboard = QGuiApplication::clipboard();
        apply(controller_->paste(clipboard->text().toUtf8().toStdString(),
                                 is_marked_as_line(clipboard->mimeData()),
                                 copied_pieces(clipboard->mimeData())));
    }
}

EditorView::OccurrenceSession* EditorView::occurrence_session(const OccurrenceSource& source) {
    if (occurrence_session_ && (document_->selections() != occurrence_session_->selections ||
                                (occurrence_session_->uses_find_options &&
                                 (occurrence_session_->match_case != source.match_case ||
                                  occurrence_session_->whole_word != source.whole_word)))) {
        occurrence_session_.reset();
    }
    return occurrence_session_ ? &*occurrence_session_ : nullptr;
}

std::optional<EditorView::OccurrenceSession>
EditorView::begin_occurrence_session(const OccurrenceSource& source) const {
    core::Query query;
    std::optional<core::Selection> first;
    bool uses_find_options = true;
    if (!source.find_text.empty()) {
        query = {.pattern = source.find_text,
                 .match_case = source.match_case,
                 .whole_word = source.whole_word};
    } else {
        auto occurrence = controller_->occurrence_query(source.match_case, source.whole_word);
        if (!occurrence) {
            return std::nullopt;
        }
        query = std::move(occurrence->query);
        first = occurrence->first;
        const auto& selections = document_->selections();
        uses_find_options = !(selections.all().size() == 1 && selections.primary().empty());
    }
    auto search = core::Search::create(query);
    if (!search) {
        return std::nullopt;
    }
    return OccurrenceSession{.first = first,
                             .search = std::move(*search),
                             .selections = document_->selections(),
                             .uses_find_options = uses_find_options,
                             .match_case = source.match_case,
                             .whole_word = source.whole_word};
}

void EditorView::add_next_occurrence(const OccurrenceSource& source) {
    add_occurrence(source, true);
}

void EditorView::add_previous_occurrence(const OccurrenceSource& source) {
    add_occurrence(source, false);
}

void EditorView::add_cursors_vertically(bool down) {
    if (controller_) {
        apply(controller_->add_cursors_vertically(down));
    }
}

void EditorView::add_cursors_to_line_ends() {
    if (controller_) {
        apply(controller_->add_cursors_to_line_ends());
    }
}

void EditorView::jump_to_bracket() {
    if (controller_) {
        apply(controller_->jump_to_bracket());
        ensure_cursor_visible();
    }
}

void EditorView::comment_lines(core::LineCommentType type, const core::CommentOptions& options) {
    if (controller_) {
        apply(controller_->comment_lines(type, options));
    }
}

void EditorView::toggle_block_comment(const core::CommentOptions& options) {
    if (controller_) {
        apply(controller_->toggle_block_comment(options));
    }
}

void EditorView::expand_selection() {
    if (controller_) {
        apply(controller_->expand_selection());
    }
}

void EditorView::shrink_selection() {
    if (controller_) {
        apply(controller_->shrink_selection());
    }
}

void EditorView::add_occurrence(const OccurrenceSource& source, bool next) {
    if (!controller_) {
        return;
    }
    OccurrenceSession* session = occurrence_session(source);
    if (!session) {
        if (document_->selections().all().size() > 1 &&
            !controller_->selections_share_text(source.match_case)) {
            // Several selections of different text have no single thing to look for: each caret
            // grows to its word, and the next press searches from there.
            apply(controller_->expand_carets_to_words());
            return;
        }
        occurrence_session_ = begin_occurrence_session(source);
        if (!occurrence_session_) {
            return;
        }
        session = &*occurrence_session_;
    }
    if (session->first) {
        const core::Selection first = *session->first;
        session->first.reset();
        apply(controller_->add_selection(first));
    } else {
        apply(next ? controller_->add_next_occurrence(session->search)
                   : controller_->add_previous_occurrence(session->search));
    }
    session->selections = document_->selections();
}

bool EditorView::select_all_occurrences(const OccurrenceSource& source, std::size_t limit) {
    if (!controller_) {
        return false;
    }
    const OccurrenceSession* session = occurrence_session(source);
    std::optional<OccurrenceSession> started;
    if (!session) {
        started = begin_occurrence_session(source);
        if (!started) {
            return false;
        }
        session = &*started;
    }
    const bool truncated = select_all_matches(session->search, limit);
    occurrence_session_.reset();
    return truncated;
}

bool EditorView::select_all_matches(const core::Search& search, std::size_t limit) {
    if (!controller_) {
        return false;
    }
    const auto result = controller_->select_all_occurrences(search, limit);
    apply(result.outcome);
    return result.truncated;
}

void EditorView::select_all() {
    if (controller_) {
        apply(controller_->select_all());
    }
}

void EditorView::move_lines(bool down) {
    if (controller_) {
        apply(controller_->move_lines(down));
    }
}

void EditorView::copy_lines(bool down) {
    if (controller_) {
        apply(controller_->copy_lines(down));
    }
}

void EditorView::duplicate_selection() {
    if (controller_) {
        apply(controller_->duplicate_selection());
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
        {QKeySequence::MoveToStartOfLine, core::Motion::home, false},
        {QKeySequence::MoveToEndOfLine, core::Motion::line_end, false},
        {QKeySequence::MoveToStartOfDocument, core::Motion::document_start, false},
        {QKeySequence::MoveToEndOfDocument, core::Motion::document_end, false},
        {QKeySequence::MoveToNextWord, core::Motion::word_right, false},
        {QKeySequence::MoveToPreviousWord, core::Motion::word_left, false},
        {QKeySequence::SelectNextChar, core::Motion::right, true},
        {QKeySequence::SelectPreviousChar, core::Motion::left, true},
        {QKeySequence::SelectNextLine, core::Motion::down, true},
        {QKeySequence::SelectPreviousLine, core::Motion::up, true},
        {QKeySequence::SelectStartOfLine, core::Motion::home, true},
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

QPoint EditorView::text_point(const QPoint& viewport_point) const {
    return {viewport_point.x() - text_origin_x(),
            std::max(0, viewport_point.y() + verticalScrollBar()->value())};
}

bool EditorView::has_multi_cursor_modifier(Qt::KeyboardModifiers modifiers) const {
    // Qt reports the Command key of macOS as the control modifier.
    return modifiers.testFlag(multi_cursor_modifier_ == core::MultiCursorModifier::alt
                                  ? Qt::AltModifier
                                  : Qt::ControlModifier);
}

bool EditorView::has_other_modifier(Qt::KeyboardModifiers modifiers) const {
    const Qt::KeyboardModifier multi = multi_cursor_modifier_ == core::MultiCursorModifier::alt
                                           ? Qt::AltModifier
                                           : Qt::ControlModifier;
    return (modifiers & (Qt::AltModifier | Qt::ControlModifier | Qt::MetaModifier) &
            ~Qt::KeyboardModifiers(multi)) != 0;
}

void EditorView::select_column_to(const QPoint& viewport_point) {
    const core::Text& text = document_->text();
    const QPoint point = text_point(viewport_point);
    const std::size_t last_line = text.line_count() - 1;
    const auto line_at = [&](int y) {
        return std::min<std::size_t>(static_cast<std::size_t>(std::max(0, y) / line_height_),
                                     last_line);
    };
    const std::size_t from = line_at(drag_origin_.y());
    const std::size_t to = line_at(point.y());
    std::vector<core::Selection> ranges;
    for (std::size_t line = std::min(from, to); line <= std::max(from, to); ++line) {
        const auto layout = layout_line(line);
        ranges.push_back({text.offset_at({line, layout->column_at(drag_origin_.x())}),
                          text.offset_at({line, layout->column_at(point.x())})});
    }
    apply(controller_->select_ranges(std::move(ranges), to - std::min(from, to)));
}

void EditorView::mousePressEvent(QMouseEvent* event) {
    if (!controller_ || event->button() != Qt::LeftButton) {
        QAbstractScrollArea::mousePressEvent(event);
        return;
    }
    setFocus();
    const QPoint point = event->position().toPoint();
    const std::size_t offset = offset_at(point);
    if (since_double_click_.isValid() &&
        since_double_click_.elapsed() < QApplication::doubleClickInterval()) {
        since_double_click_.invalidate();
        apply(controller_->select_line_at(offset)); // third click
        return;
    }
    const Qt::KeyboardModifiers modifiers = event->modifiers();
    const bool shift = modifiers.testFlag(Qt::ShiftModifier);
    drag_origin_ = text_point(point);
    mouse_drag_ = MouseDrag::extend;
    // As in VS Code, a rectangle that Shift asks for grows from where the selection began, so that
    // clicking with Shift+Alt extends it, and dragging goes on from there.
    const auto grow_rectangle = [&] {
        drag_origin_ = text_point(cursor_rect(document_->selections().primary().anchor).topLeft());
        mouse_drag_ = MouseDrag::column;
        select_column_to(point);
    };
    if (has_multi_cursor_modifier(modifiers)) {
        if (has_other_modifier(modifiers)) {
            mouse_drag_ = MouseDrag::none;
        } else if (shift) {
            grow_rectangle();
        } else {
            apply(controller_->add_caret(offset));
            mouse_drag_ = MouseDrag::extend_added_cursor;
        }
    } else if (shift && (column_selection_ || modifiers.testFlag(Qt::AltModifier))) {
        grow_rectangle();
    } else {
        apply(controller_->set_caret(offset, shift));
    }
}

void EditorView::mouseMoveEvent(QMouseEvent* event) {
    if (!controller_ || mouse_drag_ == MouseDrag::none) {
        return;
    }
    const QPoint point = event->position().toPoint();
    const bool column =
        mouse_drag_ == MouseDrag::column ||
        (mouse_drag_ == MouseDrag::extend &&
         (column_selection_ || (multi_cursor_modifier_ == core::MultiCursorModifier::ctrl_cmd &&
                                event->modifiers().testFlag(Qt::AltModifier))));
    if (column) {
        select_column_to(point);
    } else {
        apply(controller_->set_caret(offset_at(point), true));
    }
}

void EditorView::mouseReleaseEvent(QMouseEvent* event) {
    mouse_drag_ = MouseDrag::none;
    QAbstractScrollArea::mouseReleaseEvent(event);
}

void EditorView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (!controller_ || event->button() != Qt::LeftButton) {
        return;
    }
    apply(controller_->select_word_at(offset_at(event->position().toPoint())));
    since_double_click_.start();
    mouse_drag_ = MouseDrag::none;
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
    occurrence_session_.reset();
    caret_timer_.stop();
    caret_visible_ = true;
    viewport()->update();
}

void EditorView::changeEvent(QEvent* event) {
    QAbstractScrollArea::changeEvent(event);
    if (event->type() == QEvent::FontChange) {
        clear_layouts();
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
    QColor whitespace;
};

void EditorView::set_colors(const WorkbenchColors* colors) {
    QObject::disconnect(colors_connection_);
    colors_ = colors;
    if (colors_) {
        colors_connection_ = connect(colors_, &WorkbenchColors::changed, this, [this] {
            clear_layouts();
            viewport()->update();
        });
    }
    clear_layouts();
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
                match_highlight,
                palette.color(QPalette::PlaceholderText)};
    }
    const QColor foreground = colors_->color("editor.foreground");
    return {colors_->color("editor.background"),
            foreground,
            colors_->color("editor.selectionBackground"),
            colors_->is_themed("editor.selectionBackground")
                ? QColor()
                : palette().color(QPalette::HighlightedText),
            colors_->color("editor.lineHighlightBackground"),
            colors_->color("editorLineNumber.foreground"),
            colors_->color("editorLineNumber.activeForeground"),
            colors_->color("editorCursor.foreground"),
            colors_->color("editorGutter.background"),
            colors_->color("editor.findMatchBackground"),
            colors_->color("editor.findMatchHighlightBackground"),
            colors_->color("editorWhitespace.foreground")};
}

void EditorView::draw_whitespace(QPainter& painter, const LineLayout& layout,
                                 std::span<const core::ColumnRange> selected, int origin_x, int y,
                                 const QColor& color) const {
    const std::u16string_view line(reinterpret_cast<const char16_t*>(layout.text.utf16()),
                                   static_cast<std::size_t>(layout.text.size()));
    const std::vector<std::size_t> columns =
        core::whitespace_columns(line, render_whitespace_, selected);
    // The columns are in the order of their x, so the first visible one is found by bisection. The
    // marks of the whole line are found first and Qt takes time in proportion to the column to give
    // its x, so the paint of a very long line costs time in proportion to the line, not to the part
    // that is shown.
    const int left = gutter_width();
    const auto first =
        std::lower_bound(columns.begin(), columns.end(), left, [&](std::size_t column, int edge) {
            return origin_x + static_cast<int>(layout.x_of(column + 1)) <= edge;
        });
    painter.setPen(color);
    for (auto column = first; column != columns.end(); ++column) {
        const int x = origin_x + static_cast<int>(layout.x_of(*column));
        if (x >= viewport()->width()) {
            break;
        }
        const QChar mark = line[*column] == u'\t' ? QChar(0x2192) : QChar(0xB7);
        painter.drawText(QRect(x, y, char_width_, line_height_), Qt::AlignCenter, QString(mark));
    }
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
        for (const int column : layout->control_columns) {
            QTextCharFormat format;
            format.setBackground(control_background);
            format.setForeground(Qt::white);
            formats.push_back({column, 1, format});
        }
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
                if (selection_foreground.isValid()) {
                    format.setForeground(selection_foreground);
                }
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
        if (render_whitespace_ != core::RenderWhitespace::none) {
            std::vector<core::ColumnRange> selected;
            if (render_whitespace_ == core::RenderWhitespace::selection) {
                for (const core::Selection& selection : selections) {
                    const std::size_t from = std::max(selection.start(), line_begin);
                    const std::size_t to = std::min(selection.end(), line_end);
                    if (from < to) {
                        selected.push_back({text.utf16_offset(from) - line_units,
                                            text.utf16_offset(to) - line_units});
                    }
                }
            }
            draw_whitespace(painter, *layout, selected, origin_x, y, colors.whitespace);
        }
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
        painter.drawText(QRect(0, y, gutter - gutter_padding_, line_height_),
                         Qt::AlignRight | Qt::AlignVCenter, QString::number(line + 1));
    }

    if (widest != max_line_width_) {
        const_cast<EditorView*>(this)->max_line_width_ = widest;
        const_cast<EditorView*>(this)->update_scroll_bars();
    }
}

} // namespace anvil::ui
