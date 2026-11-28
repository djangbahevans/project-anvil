#include "anvil/ui/editor_view.hpp"

#include "anvil/core/visible_column.hpp"
#include "anvil/core/zoom.hpp"
#include "anvil/ui/line_layout.hpp"
#include "anvil/ui/scroll_bar.hpp"
#include "anvil/ui/wheel_input.hpp"
#include "anvil/ui/zoom.hpp"

#include <QApplication>
#include <QClipboard>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QTextLayout>
#include <QTextOption>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace anvil::ui {
namespace {

QColor to_color(const core::Rgba& color) {
    return QColor(color.red, color.green, color.blue, color.alpha);
}

constexpr int caret_width = 2;
constexpr int text_padding_px = 6;
constexpr int shadow_height_px = 6;
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

struct EditorView::ShownLine {
    LineLayout text;
    // The UTF-16 columns of the control characters, which `text` shows as symbols.
    std::vector<int> control_columns;
    // The digest of the highlighter's spans that the formats were made from, when there are any.
    std::optional<std::size_t> spans;

    // A line of one row that is laid out in several parts, whose widths are worth keeping.
    bool chunked() {
        return text.row_count() == 1 && text.row(0).chunk_count() > 1;
    }
};

// The square between the two scroll bars, in the color of the editor.
class EditorView::Corner : public QWidget {
public:
    explicit Corner(const EditorView& view) : view_(view) {}

protected:
    void paintEvent(QPaintEvent*) override;

private:
    const EditorView& view_;
};

EditorView::EditorView(QWidget* parent) : QAbstractScrollArea(parent) {
    setVerticalScrollBar(
        new ScrollBar(Qt::Vertical, ScrollBar::editor_vertical_width, "editor.background"));
    setHorizontalScrollBar(
        new ScrollBar(Qt::Horizontal, ScrollBar::editor_horizontal_height, "editor.background"));
    setCornerWidget(new Corner(*this));
    animator_ = std::make_unique<ScrollAnimator>(*this);
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
    QTextLayout sample(QStringLiteral("_gjpqy"), font());
    sample.beginLayout();
    const QTextLine line = sample.createLine();
    sample.endLayout();
    const qreal ink =
        line.ascent() + QFontMetricsF(font()).tightBoundingRect(sample.text()).bottom();
    // VS Code's line height, and for a font that needs more, room for a laid out line and for the
    // underscores that some fonts draw below it.
    const int wanted =
        static_cast<int>(std::lround(line_height_ratio_ * QFontInfo(font()).pixelSize()));
    line_height_ = std::max(wanted, static_cast<int>(std::ceil(std::max(line.height(), ink))));
    text_top_ = (line_height_ - static_cast<int>(std::ceil(line.height()))) / 2;
    char_width_ = std::max(1, metrics.horizontalAdvance(QLatin1Char('M')));
}

void EditorView::apply_settings(const core::EditorSettings& settings) {
    // Scroll positions are in pixels, so they are converted to places in the text and back to keep
    // the same text in view when the metrics or the rows change.
    animator_->finish();
    const int vertical = verticalScrollBar()->value();
    const int horizontal = horizontalScrollBar()->value();
    const TextPlace place = top_place();
    clear_layouts();
    rows_.reset();
    const int first_column = horizontal / char_width_;
    const int previous_line_height = line_height_;
    const int previous_char_width = char_width_;

    default_indentation_ = {static_cast<std::size_t>(settings.tab_size), settings.insert_spaces};
    detect_indentation_ = settings.detect_indentation;
    take_indentation();
    line_height_ratio_ = settings.line_height_ratio;
    render_whitespace_ = settings.render_whitespace;
    render_control_characters_ = settings.render_control_characters;
    multi_cursor_modifier_ = settings.multi_cursor_modifier;
    column_selection_ = settings.column_selection;
    wheel_ = settings.wheel;
    scroll_beyond_last_line_ = settings.scroll_beyond_last_line;
    scroll_beyond_last_column_ = settings.scroll_beyond_last_column;
    smooth_scrolling_ = settings.smooth_scrolling;
    text_padding_ = core::scale_for_zoom(text_padding_px, settings.zoom_level);
    shadow_height_ = core::scale_for_zoom(shadow_height_px, settings.zoom_level);
    gutter_padding_ = core::scale_for_zoom(gutter_padding_px, settings.zoom_level);
    for (QScrollBar* bar : {verticalScrollBar(), horizontalScrollBar()}) {
        static_cast<ScrollBar*>(bar)->set_zoom_level(settings.zoom_level);
    }
    const QFont wanted = font_for(settings.font_family,
                                  core::scale_for_zoom(settings.font_size, settings.zoom_level));
    if (wanted != font()) {
        setFont(wanted);
    }
    update_metrics();
    // The widest line is only measured while painting, so until the next paint the old width,
    // scaled to the new font, keeps the horizontal range from collapsing to nothing.
    max_line_width_ =
        wrapping_ == Wrapping::off ? max_line_width_ * char_width_ / previous_char_width : 0;
    verticalScrollBar()->setSingleStep(line_height_);
    horizontalScrollBar()->setSingleStep(char_width_ * 4);
    update_scroll_bars();
    // A reload that leaves the metrics alone, such as a change of color theme, keeps the exact
    // position instead of snapping to whole rows.
    const bool same_metrics =
        line_height_ == previous_line_height && char_width_ == previous_char_width;
    verticalScrollBar()->setValue(static_cast<int>(row_of_place(place)) * line_height_ +
                                  (same_metrics ? vertical % previous_line_height : 0));
    horizontalScrollBar()->setValue(same_metrics ? horizontal : first_column * char_width_);
    viewport()->update();
}

void EditorView::take_indentation() {
    const core::Indentation indentation =
        document_ ? document_->resolve_indentation(default_indentation_, detect_indentation_)
                  : default_indentation_;
    tab_size_ = indentation.tab_size;
    insert_spaces_ = indentation.insert_spaces;
    if (controller_) {
        controller_->set_tab_size(tab_size_);
    }
}

void EditorView::refresh_indentation() {
    take_indentation();
    clear_layouts();
    rows_.reset();
    viewport()->update();
}

void EditorView::set_document(std::shared_ptr<core::Document> document) {
    document_ = std::move(document);
    controller_ = document_ ? std::make_unique<core::EditController>(*document_) : nullptr;
    if (controller_) {
        controller_->set_row_source([this] { return rows(); });
    }
    take_indentation();
    max_line_width_ = 0;
    highlights_.clear();
    clear_layouts();
    rows_.reset();
    animator_->stop();
    verticalScrollBar()->setValue(0);
    horizontalScrollBar()->setValue(0);
    update_scroll_bars();
    viewport()->update();
    emit content_changed();
}

void EditorView::set_wrapping(Wrapping wrapping, std::size_t column) {
    column = std::max<std::size_t>(column, 1);
    if (wrapping == wrapping_ && column == wrap_column_) {
        return;
    }
    animator_->finish();
    const TextPlace place = top_place();
    set_wrap_mode(wrapping, column);
    horizontalScrollBar()->setValue(0);
    reflow(place, 0);
}

void EditorView::set_wrap_mode(Wrapping wrapping, std::size_t column) {
    wrapping_ = wrapping;
    wrap_column_ = std::max<std::size_t>(column, 1);
    // The scroll bar of wrapped text is always there: its coming and going would change the width
    // that the rows are made for.
    setVerticalScrollBarPolicy(wrapping == Wrapping::off ? Qt::ScrollBarAsNeeded
                                                         : Qt::ScrollBarAlwaysOn);
}

void EditorView::set_document(std::shared_ptr<core::Document> document, Wrapping wrapping,
                              std::size_t column) {
    // Layouts and rows of the document shown now are of no use to the next one.
    rows_.reset();
    clear_layouts();
    set_wrap_mode(wrapping, column);
    set_document(std::move(document));
}

const core::RowMap* EditorView::rows() const {
    sync_rows();
    return rows_ ? &*rows_ : nullptr;
}

std::optional<core::WrapOptions> EditorView::wrap_options() const {
    if (wrapping_ == Wrapping::off || !document_) {
        return std::nullopt;
    }
    const QFontMetricsF metrics(font());
    const qreal advance = std::max<qreal>(1, metrics.horizontalAdvance(QLatin1Char('M')));
    const auto fits = static_cast<std::size_t>(
        std::max<qreal>(1, std::floor((text_width() - caret_width) / advance)));
    const std::size_t column = wrapping_ == Wrapping::view     ? fits
                               : wrapping_ == Wrapping::column ? wrap_column_
                                                               : std::min(fits, wrap_column_);
    // U+4E2D stands for the full-width characters.
    const qreal full_width = metrics.horizontalAdvance(QChar(0x4E2D)) / advance;
    return core::WrapOptions{tab_size_, column, std::max<qreal>(1, full_width)};
}

void EditorView::sync_rows() const {
    if (!document_ || wrapping_ == Wrapping::off) {
        rows_.reset();
        return;
    }
    if (rows_ && rows_version_ == document_->version()) {
        return;
    }
    const core::WrapOptions options = rows_ ? rows_->options() : *wrap_options();
    rows_.emplace(document_->text(), options);
    rows_version_ = document_->version();
}

void EditorView::apply_rows(const core::DocumentChange& change) {
    if (rows_ && rows_version_ + 1 == change.version) {
        rows_->apply(change, document_->text());
        rows_version_ = change.version;
    }
}

void EditorView::rewrap() {
    const std::optional<core::WrapOptions> wanted = wrap_options();
    if (rows_ ? wanted == rows_->options() : !wanted) {
        return;
    }
    animator_->finish();
    reflow(top_place(), verticalScrollBar()->value() % line_height_);
}

void EditorView::reflow(const TextPlace& place, int pixels) {
    clear_layouts();
    rows_.reset();
    max_line_width_ = 0;
    update_scroll_bars();
    verticalScrollBar()->setValue(static_cast<int>(row_of_place(place)) * line_height_ + pixels);
    viewport()->update();
}

EditorView::TextPlace EditorView::top_place() const {
    if (!document_) {
        return {};
    }
    const auto row = static_cast<std::size_t>(verticalScrollBar()->value() / line_height_);
    if (!rows_) {
        return {row, 0};
    }
    // The rows can be those of an earlier text, whose lines the current one need not have.
    const std::size_t line = std::min(rows_->line_of(row), document_->text().line_count() - 1);
    const auto layout = layout_line(line);
    const std::size_t part =
        std::min(row - std::min(row, rows_->first_row(line)), layout->text.row_count() - 1);
    return {line, layout->text.begin(part)};
}

std::size_t EditorView::row_of_place(const TextPlace& place) const {
    if (!document_) {
        return 0;
    }
    const std::size_t line = std::min(place.line, document_->text().line_count() - 1);
    if (!rows_) {
        return line;
    }
    const auto layout = layout_line(line);
    return rows_->first_row(line) +
           layout->text.row_of(
               std::min(place.column, static_cast<std::size_t>(layout->text.text().size())));
}

std::size_t EditorView::row_count() const {
    if (!document_) {
        return 0;
    }
    return rows_ ? rows_->row_count() : document_->text().line_count();
}

std::size_t EditorView::first_row(std::size_t line) const {
    return rows_ ? rows_->first_row(line) : line;
}

std::size_t EditorView::line_of_row(std::size_t row) const {
    return rows_ ? rows_->line_of(row) : std::min(row, document_->text().line_count() - 1);
}

void EditorView::document_changed_elsewhere() {
    rewrap();
    update_scroll_bars();
    viewport()->update();
    emit content_changed();
}

void EditorView::document_changed_elsewhere(const core::DocumentChange& change) {
    update_layouts(change);
    apply_rows(change);
    document_changed_elsewhere();
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
    sync_rows();
    const int scroll = verticalScrollBar()->value();
    const std::size_t first = line_of_row(static_cast<std::size_t>(scroll / line_height_));
    const std::size_t last =
        line_of_row(static_cast<std::size_t>((scroll + viewport()->height()) / line_height_)) + 1;
    QRegion changed;
    const auto repaint = [&](std::size_t line) {
        const int y = static_cast<int>(first_row(line)) * line_height_ - scroll;
        const auto rows = static_cast<int>(rows_ ? rows_->rows_of(line) : 1);
        changed += QRect(0, y, viewport()->width(), rows * line_height_);
    };
    // Only the lines in view are asked for spans; a layout made from spans elsewhere is dropped
    // and made again when it comes into view.
    for (auto layout = layouts_.begin(); layout != layouts_.end();) {
        auto& [line, cached] = *layout;
        const bool in_view = line >= first && line < last;
        if (cached.stale || !cached.layout->spans ||
            (in_view && *cached.layout->spans == span_digest(line))) {
            ++layout;
            continue;
        }
        if (in_view) {
            repaint(line);
        }
        if (cached.layout->chunked()) {
            cached.stale = true;
            ++layout;
        } else {
            layout = layouts_.erase(layout);
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

int EditorView::text_width() const {
    return viewport()->width() - gutter_width() - 2 * text_padding_;
}

int EditorView::scroll_width(qreal end) const {
    // Wrapped rows that fit need no scrolling, not even for the room after them.
    const int width = static_cast<int>(end);
    return !wraps_at_view() || width > text_width() ? width : 0;
}

bool EditorView::wraps_at_view() const {
    return wrapping_ == Wrapping::view || wrapping_ == Wrapping::bounded;
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

std::shared_ptr<EditorView::ShownLine> EditorView::layout_line(std::size_t line) const {
    if (layout_version_ != document_->version()) {
        layouts_.clear();
        layout_version_ = document_->version();
    }
    const auto found = layouts_.find(line);
    if (found != layouts_.end() && !found->second.stale) {
        found->second.used = ++layout_clock_;
        return found->second.layout;
    }
    auto layout = build_layout(line);
    if (found != layouts_.end()) {
        if (layout->chunked()) {
            layout->text.row(0).keep_widths(found->second.layout->text.row(0),
                                            found->second.unchanged);
        }
        found->second = {layout, ++layout_clock_};
        return layout;
    }
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
        // A long line keeps the widths before the edit, so that the text there does not move.
        const auto edited = layouts_.find(first);
        if (edited != layouts_.end() && edited->second.layout->chunked()) {
            edited->second.stale = true;
            edited->second.unchanged = std::min(edited->second.unchanged, edit.range.start.column);
        } else {
            layouts_.erase(first);
        }
        std::erase_if(layouts_, [&](const auto& entry) {
            return entry.first > first && entry.first <= last;
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

std::shared_ptr<EditorView::ShownLine> EditorView::build_layout(std::size_t line) const {
    const std::string content = document_->text().line(line);
    QString text = to_qstring(content);
    std::vector<int> control_columns;
    if (render_control_characters_) {
        // Through a pointer: the line can be long, and each access by index is checked.
        QChar* const units = text.data();
        for (int i = 0; i < text.size(); ++i) {
            const char16_t c = units[i].unicode();
            if ((c < 0x20 && c != u'\t') || c == 0x7f) {
                // The Control Pictures block has one symbol for each, as many columns as the
                // character.
                units[i] = QChar(c == 0x7f ? 0x2421 : 0x2400 + c);
                control_columns.push_back(i);
            }
        }
    }
    QList<QTextLayout::FormatRange> formats;
    std::optional<std::size_t> spans;
    if (highlighter_ && colors_ && colors_->token_theme()) {
        formats = syntax_formats(*colors_->token_theme(), line, text.size());
        spans = span_digest(line);
    }
    const ChunkedLine::Style style{font(), char_width_ * static_cast<qreal>(tab_size_),
                                   QFontMetricsF(font()).horizontalAdvance(QLatin1Char('M'))};
    LineLayout::Wrap wrap;
    if (rows_) {
        const core::LineBreaks breaks = core::break_line(content, rows_->options());
        std::size_t byte = 0;
        std::size_t column = 0;
        for (std::size_t i = 0; i < breaks.starts.size(); ++i) {
            for (; byte < breaks.starts[i]; ++byte) {
                const auto unit = static_cast<unsigned char>(content[byte]);
                if ((unit & 0xC0u) != 0x80u) {
                    column += unit >= 0xF0 ? 2 : 1;
                }
            }
            wrap.breaks.push_back({column, breaks.columns[i]});
        }
        wrap.indent_end = breaks.indent > 0 ? content.find_first_not_of(" \t") : 0;
        wrap.tab_size = rows_->options().tab_size;
    }
    return std::make_shared<ShownLine>(ShownLine{
        LineLayout(std::move(text), std::move(formats), style, std::move(wrap), &layouts_built_),
        std::move(control_columns), spans});
}

QRect EditorView::cursor_rect(std::size_t offset) const {
    if (!document_) {
        return {};
    }
    sync_rows();
    const core::Position position = document_->text().position_at(offset);
    const auto layout = layout_line(position.line);
    const std::size_t part = caret_part(*layout, position.column, offset);
    const int x =
        text_origin_x() +
        static_cast<int>(layout->text.left(part) +
                         layout->text.row(part).x_of(position.column - layout->text.begin(part)));
    const std::size_t row = first_row(position.line) + part;
    const int y = static_cast<int>(row) * line_height_ - verticalScrollBar()->value();
    return QRect(x, y, caret_width, line_height_);
}

std::size_t EditorView::offset_at(const QPoint& point) const {
    return hit_at(point).offset;
}

EditorView::Hit EditorView::hit_at(const QPoint& point) const {
    if (!document_) {
        return {};
    }
    sync_rows();
    const int y = std::max(0, point.y() + verticalScrollBar()->value());
    const std::size_t row =
        std::min<std::size_t>(static_cast<std::size_t>(y / line_height_), row_count() - 1);
    return hit_at_row(row, point.x() - text_origin_x());
}

EditorView::Hit EditorView::hit_at_row(std::size_t row, qreal x) const {
    const std::size_t line = line_of_row(row);
    const auto layout = layout_line(line);
    const std::size_t part = row - first_row(line);
    const std::size_t column = layout->text.column_at(part, x);
    return {document_->text().offset_at({line, column}),
            part + 1 < layout->text.row_count() && column == layout->text.end(part)};
}

std::size_t EditorView::caret_part(const ShownLine& line, std::size_t column,
                                   std::size_t offset) const {
    const std::size_t part = line.text.row_of(column);
    const bool earlier =
        part > 0 && column == line.text.begin(part) && controller_ && controller_->ends_row(offset);
    return earlier ? part - 1 : part;
}

void EditorView::update_scroll_bars() {
    sync_rows();
    const int rows = static_cast<int>(row_count());
    const int bottom = scroll_beyond_last_line_ ? (rows - 1) * line_height_
                                                : rows * line_height_ - viewport()->height();
    verticalScrollBar()->setRange(0, std::max(0, bottom));
    verticalScrollBar()->setPageStep(viewport()->height());
    const int available = text_width();
    // VS Code counts no columns beyond the last one while lines wrap at the width of the view.
    // The caret after the longest line needs room in any case.
    const std::int64_t beyond =
        wraps_at_view() ? char_width_
                        : std::max<std::int64_t>(
                              std::int64_t{scroll_beyond_last_column_} * char_width_, caret_width);
    const std::int64_t end = max_line_width_ + beyond - available;
    horizontalScrollBar()->setRange(
        0, static_cast<int>(std::clamp<std::int64_t>(end, 0, std::numeric_limits<int>::max())));
    horizontalScrollBar()->setPageStep(available);
}

void EditorView::ensure_cursor_visible() {
    if (!document_) {
        return;
    }
    sync_rows();
    const std::size_t active = document_->selections().primary().active;
    const core::Position position = document_->text().position_at(active);
    const auto layout = layout_line(position.line);
    const std::size_t part = caret_part(*layout, position.column, active);
    const int top = static_cast<int>(first_row(position.line) + part) * line_height_;
    // Against where the view is going, so that movements asked for in quick succession add up.
    QPoint wanted = animator_->target();
    if (top < wanted.y()) {
        wanted.setY(top);
    } else if (top + line_height_ > wanted.y() + viewport()->height()) {
        wanted.setY(top + line_height_ - viewport()->height());
    }

    const int available = text_width();
    ChunkedLine& row = layout->text.row(part);
    const std::size_t column = position.column - layout->text.begin(part);
    const qreal left = layout->text.left(part);
    // The parts of the row that are going to be on screen are laid out first, since their measured
    // widths can move the caret.
    const qreal estimate = row.x_of(column);
    row.columns_between(estimate - available, estimate + available);
    const int x = static_cast<int>(left + row.x_of(column));
    max_line_width_ = std::max(max_line_width_, scroll_width(left + row.width()));
    update_scroll_bars();
    if (x < wanted.x()) {
        wanted.setX(x);
    } else if (x + caret_width > wanted.x() + available) {
        wanted.setX(x + caret_width - available);
    }
    reveal(wanted);
}

void EditorView::reveal(QPoint position) {
    if (position == animator_->target()) {
        return;
    }
    // A drag follows the pointer, so what it uncovers shows at once.
    if (!smooth_scrolling_ || mouse_drag_ != MouseDrag::none) {
        animator_->scroll_to(position, false);
        return;
    }
    // VS Code moves a line or less up or down at once, so that typing and the arrow keys do not
    // lag; sideways it moves over time.
    if (std::abs(position.y() - verticalScrollBar()->value()) <= line_height_) {
        animator_->scroll_to({horizontalScrollBar()->value(), position.y()}, false);
    }
    animator_->scroll_to(position, true);
}

QPoint EditorView::scroll_target() const {
    return animator_->target();
}

void EditorView::scroll_to(QPoint position) {
    animator_->scroll_to(position, false);
}

bool EditorView::scrolling() const {
    return animator_->moving();
}

void EditorView::set_scroll_clock(std::function<std::int64_t()> now) {
    animator_->set_clock(std::move(now));
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
        apply_rows(*outcome.change);
        // The width of the line numbers, and with it of the text, follows the number of lines.
        rewrap();
        update_scroll_bars();
    }
    if (!hold_reveal_) {
        ensure_cursor_visible();
    }
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

void EditorView::delete_word_left() {
    if (controller_) {
        apply(controller_->delete_word_left());
    }
}

void EditorView::delete_word_right() {
    if (controller_) {
        apply(controller_->delete_word_right());
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
    // The page is one movement of the view, to where its last line leaves the caret.
    hold_reveal_ = true;
    bool moved = false;
    for (int i = 0; i < lines; ++i) {
        const core::EditOutcome outcome =
            controller_->move(down ? core::Motion::down : core::Motion::up, extend);
        moved = moved || outcome.changed_anything();
        apply(outcome);
    }
    hold_reveal_ = false;
    if (moved) {
        ensure_cursor_visible();
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

void EditorView::tab() {
    if (controller_) {
        apply(controller_->tab(insert_spaces_));
    }
}

void EditorView::indent_lines() {
    if (controller_) {
        apply(controller_->indent_lines(insert_spaces_));
    }
}

void EditorView::outdent_lines() {
    if (controller_) {
        apply(controller_->outdent_lines(insert_spaces_));
    }
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
        tab();
    } else if (event->key() == Qt::Key_Backtab) {
        outdent_lines();
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
    sync_rows();
    const QPoint point = text_point(viewport_point);
    const std::size_t last_row = row_count() - 1;
    const auto row_at = [&](int y) {
        return std::min<std::size_t>(static_cast<std::size_t>(std::max(0, y) / line_height_),
                                     last_row);
    };
    const std::size_t from = row_at(drag_origin_.y());
    const std::size_t to = row_at(point.y());
    std::vector<core::Selection> ranges;
    std::vector<std::size_t> row_ends;
    for (std::size_t row = std::min(from, to); row <= std::max(from, to); ++row) {
        const Hit active = hit_at_row(row, point.x());
        ranges.push_back({hit_at_row(row, drag_origin_.x()).offset, active.offset});
        if (active.row_end) {
            row_ends.push_back(active.offset);
        }
    }
    apply(controller_->select_ranges(std::move(ranges), to - std::min(from, to),
                                     std::move(row_ends)));
}

void EditorView::mousePressEvent(QMouseEvent* event) {
    if (!controller_ || event->button() != Qt::LeftButton) {
        QAbstractScrollArea::mousePressEvent(event);
        return;
    }
    setFocus();
    const QPoint point = event->position().toPoint();
    const Hit hit = hit_at(point);
    if (since_double_click_.isValid() &&
        since_double_click_.elapsed() < QApplication::doubleClickInterval()) {
        since_double_click_.invalidate();
        apply(controller_->select_line_at(hit.offset)); // third click
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
            apply(controller_->add_caret(hit.offset, hit.row_end));
            mouse_drag_ = MouseDrag::extend_added_cursor;
        }
    } else if (shift && (column_selection_ || modifiers.testFlag(Qt::AltModifier))) {
        grow_rectangle();
    } else {
        apply(controller_->set_caret(hit.offset, shift, hit.row_end));
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
        const Hit hit = hit_at(point);
        apply(controller_->set_caret(hit.offset, true, hit.row_end));
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

void EditorView::wheelEvent(QWheelEvent* event) {
#if defined(Q_OS_MACOS)
    constexpr bool macos = true;
#else
    constexpr bool macos = false;
#endif
    const core::WheelTurn turn = wheel_turn(*event, QGuiApplication::platformName());
    const core::ScrollDistance distance =
        core::wheel_scroll(turn,
                           {.alt = event->modifiers().testFlag(Qt::AltModifier),
                            .shift = event->modifiers().testFlag(Qt::ShiftModifier)},
                           macos, wheel_);
    if (turn.x != 0 || turn.y != 0) {
        wheel_classifier_.accept(turn);
    }
    const auto moved = [](const QScrollBar* bar, int from, int pixels) {
        return static_cast<int>(
            std::clamp<std::int64_t>(std::int64_t{from} + pixels, bar->minimum(), bar->maximum()));
    };
    const QPoint from = animator_->target();
    const QPoint to(moved(horizontalScrollBar(), from.x(), distance.x),
                    moved(verticalScrollBar(), from.y(), distance.y));
    if (to != from) {
        // A touchpad gives pixels or the phases of a gesture, except where the platform plugin
        // gives neither, and its movements already follow the finger.
        const bool wheel = event->pixelDelta().isNull() && event->phase() == Qt::NoScrollPhase &&
                           wheel_classifier_.is_mouse_wheel();
        animator_->scroll_to(to, smooth_scrolling_ && wheel);
    }
    // The wheel over the editor never scrolls what is around it.
    event->accept();
}

void EditorView::resizeEvent(QResizeEvent* event) {
    QAbstractScrollArea::resizeEvent(event);
    rewrap();
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
        rows_.reset();
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
    // Invalid when no theme gives the editor a shadow.
    QColor shadow;
};

void EditorView::set_colors(const WorkbenchColors* colors) {
    QObject::disconnect(colors_connection_);
    colors_ = colors;
    for (QScrollBar* bar : {verticalScrollBar(), horizontalScrollBar()}) {
        static_cast<ScrollBar*>(bar)->set_colors(colors);
    }
    cornerWidget()->update();
    if (colors_) {
        colors_connection_ = connect(colors_, &WorkbenchColors::changed, this, [this] {
            clear_layouts();
            viewport()->update();
            cornerWidget()->update();
        });
    }
    clear_layouts();
    viewport()->update();
}

void EditorView::Corner::paintEvent(QPaintEvent*) {
    QPainter(this).fillRect(rect(), view_.current_colors().background);
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
                palette.color(QPalette::PlaceholderText),
                QColor()};
    }
    const QColor foreground = colors_->color("editor.foreground");
    // Selected text keeps its colors over a theme's selection, unless the theme gives it one.
    QColor selected_text;
    if (colors_->is_themed("editor.selectionForeground")) {
        selected_text = colors_->color("editor.selectionForeground");
    } else if (!colors_->is_themed("editor.selectionBackground")) {
        selected_text = palette().color(QPalette::HighlightedText);
    }
    return {colors_->color("editor.background"),
            foreground,
            colors_->color("editor.selectionBackground"),
            selected_text,
            colors_->color("editor.lineHighlightBackground"),
            colors_->color("editorLineNumber.foreground"),
            colors_->color("editorLineNumber.activeForeground"),
            colors_->color("editorCursor.foreground"),
            colors_->color("editorGutter.background"),
            colors_->color("editor.findMatchBackground"),
            colors_->color("editor.findMatchHighlightBackground"),
            colors_->color("editorWhitespace.foreground"),
            colors_->is_themed("scrollbar.shadow") ? colors_->color("scrollbar.shadow") : QColor()};
}

void EditorView::draw_whitespace(QPainter& painter, ShownLine& line, std::size_t row,
                                 std::span<const core::ColumnRange> selected,
                                 std::pair<std::size_t, std::size_t> shown, qreal origin_x, int y,
                                 const QColor& color) const {
    const QString& text = line.text.text();
    const std::u16string_view units(reinterpret_cast<const char16_t*>(text.utf16()),
                                    static_cast<std::size_t>(text.size()));
    const std::vector<std::size_t> columns =
        core::whitespace_columns(units, render_whitespace_, selected, shown.first, shown.second);
    ChunkedLine& part = line.text.row(row);
    const std::size_t begin = line.text.begin(row);
    const auto x_of = [&](std::size_t column) {
        return static_cast<int>(origin_x + part.x_of(column - begin));
    };
    // The columns are in the order of their x, so the first visible one is found by bisection.
    const int left = gutter_width();
    const auto first =
        std::lower_bound(columns.begin(), columns.end(), left,
                         [&](std::size_t column, int edge) { return x_of(column + 1) <= edge; });
    painter.setPen(color);
    for (auto column = first; column != columns.end(); ++column) {
        const int x = x_of(*column);
        if (x >= viewport()->width()) {
            break;
        }
        const QChar mark = units[*column] == u'\t' ? QChar(0x2192) : QChar(0xB7);
        painter.drawText(QRect(x, y, char_width_, line_height_), Qt::AlignCenter, QString(mark));
    }
}

void EditorView::draw_scroll_shadow(QPainter& painter, const QColor& color) const {
    QLinearGradient fade(0, 0, 0, shadow_height_);
    // The falloff of VS Code's blurred inset shadow.
    for (const auto& [at, strength] : {std::pair{0.0, 0.5}, std::pair{1.0 / 3, 0.25},
                                       std::pair{2.0 / 3, 0.09}, std::pair{1.0, 0.0}}) {
        QColor stop = color;
        stop.setAlphaF(static_cast<float>(color.alphaF() * strength));
        fade.setColorAt(at, stop);
    }
    painter.fillRect(QRect(0, 0, viewport()->width(), shadow_height_), fade);
}

void EditorView::paintEvent(QPaintEvent*) {
    QPainter painter(viewport());
    const Colors colors = current_colors();
    painter.fillRect(viewport()->rect(), colors.background);
    if (!document_) {
        return;
    }

    sync_rows();
    const core::Text& text = document_->text();
    const auto& selections = document_->selections().all();
    const int scroll_y = verticalScrollBar()->value();
    const std::size_t first = static_cast<std::size_t>(scroll_y / line_height_);
    const std::size_t last =
        std::min(row_count(),
                 static_cast<std::size_t>((scroll_y + viewport()->height()) / line_height_) + 1);
    const std::size_t active_line = text.position_at(document_->selections().primary().active).line;
    const int origin_x = text_origin_x();
    const QColor selection_background = colors.selection_background;
    const QColor selection_foreground = colors.selection_foreground;
    const core::Selection primary = document_->selections().primary();
    int widest = max_line_width_;

    painter.setFont(font());
    for (std::size_t row = first; row < last; ++row) {
        const std::size_t line = line_of_row(row);
        const int y = static_cast<int>(row) * line_height_ - scroll_y;
        if (line == active_line && selections.size() == 1 && selections[0].empty()) {
            painter.fillRect(QRect(0, y, viewport()->width(), line_height_), colors.line_highlight);
        }

        const auto layout = layout_line(line);
        const std::size_t part = row - first_row(line);
        ChunkedLine& chunks = layout->text.row(part);
        const std::size_t begin = layout->text.begin(part);
        const qreal indent = layout->text.left(part);
        const qreal left = origin_x + indent;
        const std::size_t line_begin = text.line_start(line);
        const std::size_t line_end = text.line_end(line);
        const std::size_t line_units = text.utf16_offset(line_begin);
        // The part of the row that is on screen, which is all that formats are made for.
        const qreal from_x = -left;
        const qreal to_x = viewport()->width() - left;
        const auto in_row = chunks.columns_between(from_x, to_x);
        const std::pair<std::size_t, std::size_t> shown{begin + in_row.first,
                                                        begin + in_row.second};
        const std::size_t shown_begin = text.offset_at({line, shown.first});
        const std::size_t shown_end = text.offset_at({line, shown.second});

        QList<QTextLayout::FormatRange> formats;
        for (auto column =
                 std::ranges::lower_bound(layout->control_columns, static_cast<int>(shown.first));
             column != layout->control_columns.end() &&
             static_cast<std::size_t>(*column) < shown.second;
             ++column) {
            QTextCharFormat format;
            format.setBackground(control_background);
            format.setForeground(Qt::white);
            formats.push_back({*column, 1, format});
        }
        // Highlights go first so that a selection over a highlighted match is drawn on top.
        auto highlight = std::lower_bound(
            highlights_.begin(), highlights_.end(), shown_begin,
            [](const core::Match& match, std::size_t offset) { return match.end <= offset; });
        for (; highlight != highlights_.end() && highlight->begin <= shown_end; ++highlight) {
            const std::size_t from = std::max(highlight->begin, shown_begin);
            const std::size_t to = std::min(highlight->end, shown_end);
            if (from < to) {
                QTextCharFormat format;
                // The match the selection is on is the current one.
                const bool current =
                    highlight->begin == primary.start() && highlight->end == primary.end();
                format.setBackground(current ? colors.find_match : colors.find_highlight);
                const int start_column = static_cast<int>(text.utf16_offset(from) - line_units);
                const int end_column = static_cast<int>(text.utf16_offset(to) - line_units);
                formats.push_back({start_column, end_column - start_column, format});
            }
        }
        // Where the selected text takes a color of its own.
        std::vector<QRect> recolored;
        for (const core::Selection& selection : selections) {
            const std::size_t from = std::max(selection.start(), shown_begin);
            const std::size_t to = std::min(selection.end(), shown_end);
            if (from < to) {
                QTextCharFormat format;
                format.setBackground(selection_background);
                const int start_column = static_cast<int>(text.utf16_offset(from) - line_units);
                const int end_column = static_cast<int>(text.utf16_offset(to) - line_units);
                formats.push_back({start_column, end_column - start_column, format});
                if (selection_foreground.isValid()) {
                    const qreal first_x =
                        chunks.x_of(static_cast<std::size_t>(start_column) - begin);
                    const qreal last_x = chunks.x_of(static_cast<std::size_t>(end_column) - begin);
                    recolored.push_back(QRectF(left + std::min(first_x, last_x), y,
                                               std::abs(last_x - first_x), line_height_)
                                            .toAlignedRect());
                }
            }
            // A selection that continues past the line break shows a small block after the text.
            if (part + 1 == layout->text.row_count() && selection.start() <= line_end &&
                selection.end() > line_end && line + 1 < text.line_count()) {
                const int x = static_cast<int>(left + chunks.width());
                painter.fillRect(QRect(x, y, char_width_ / 2 + 2, line_height_),
                                 selection_background);
            }
        }

        // A background fills its line, which is higher than the text that a layout would color.
        // The layout is given only the colors of text: it redraws what it has a format for inside
        // the height of the text, which cuts off underscores that a font draws below it.
        QList<QTextLayout::FormatRange> text_colors;
        for (QTextLayout::FormatRange& format : formats) {
            format.start -= static_cast<int>(begin);
            if (format.format.hasProperty(QTextFormat::BackgroundBrush)) {
                const auto column = static_cast<std::size_t>(format.start);
                const qreal first = chunks.x_of(column);
                const qreal last = chunks.x_of(column + static_cast<std::size_t>(format.length));
                painter.fillRect(
                    QRectF(left + std::min(first, last), y, std::abs(last - first), line_height_),
                    format.format.background());
            }
            if (format.format.hasProperty(QTextFormat::ForegroundBrush)) {
                text_colors.push_back(format);
            }
        }
        painter.save();
        if (!recolored.empty()) {
            QRegion kept(viewport()->rect());
            for (const QRect& area : recolored) {
                kept -= area;
            }
            painter.setClipRegion(kept);
        }
        painter.setPen(colors.foreground);
        chunks.draw(painter, QPointF(left, y + text_top_), text_colors, from_x, to_x);
        for (const QRect& area : recolored) {
            painter.setClipRect(area);
            chunks.draw_in(painter, QPointF(left, y + text_top_), selection_foreground, from_x,
                           to_x);
        }
        painter.restore();
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
            draw_whitespace(painter, *layout, part, selected, shown, left, y, colors.whitespace);
        }
        widest = std::max(widest, scroll_width(indent + chunks.width()));

        if (caret_visible_ && hasFocus()) {
            for (const core::Selection& selection : selections) {
                if (selection.active < line_begin || selection.active > line_end) {
                    continue;
                }
                const std::size_t column = text.utf16_offset(selection.active) - line_units;
                // A caret off screen would only have the part it is in laid out for nothing.
                if (caret_part(*layout, column, selection.active) != part || column < shown.first ||
                    column > shown.second) {
                    continue;
                }
                const int x = static_cast<int>(left + chunks.x_of(column - begin));
                painter.fillRect(QRect(x, y, caret_width, line_height_), colors.cursor);
                if (!preedit_.isEmpty() &&
                    selection.active == document_->selections().primary().active) {
                    QFont underlined = font();
                    underlined.setUnderline(true);
                    painter.setFont(underlined);
                    painter.drawText(QPoint(x + 2, y + text_top_ + QFontMetrics(font()).ascent()),
                                     preedit_);
                    painter.setFont(font());
                }
            }
        }
    }

    // The gutter covers the horizontally scrolled text and carries the line numbers.
    const int gutter = gutter_width();
    painter.fillRect(QRect(0, 0, gutter, viewport()->height()), colors.gutter_background);
    for (std::size_t row = first; row < last; ++row) {
        const std::size_t line = line_of_row(row);
        if (row != first_row(line)) {
            continue;
        }
        const int y = static_cast<int>(row) * line_height_ - scroll_y;
        painter.setPen(line == active_line ? colors.line_number_active : colors.line_number);
        // From the top, like the text, so that numbers and text stand on one baseline.
        painter.drawText(QRect(0, y + text_top_, gutter - gutter_padding_, line_height_),
                         Qt::AlignRight | Qt::AlignTop, QString::number(line + 1));
    }

    if (scroll_y > 0 && colors.shadow.isValid()) {
        draw_scroll_shadow(painter, colors.shadow);
    }

    if (widest != max_line_width_) {
        const_cast<EditorView*>(this)->max_line_width_ = widest;
        const_cast<EditorView*>(this)->update_scroll_bars();
    }
}

} // namespace anvil::ui
