#include "anvil/ui/quick_pick.hpp"

#include <QAbstractListModel>
#include <QApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPainter>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QVBoxLayout>
#include <algorithm>
#include <numeric>

namespace anvil::ui {
namespace {

constexpr int max_width = 600;
constexpr int window_margin = 8;
constexpr int text_padding = 8;
constexpr int detail_row_height = 2 * QuickPick::row_height;

} // namespace

class QuickPick::Model : public QAbstractListModel {
public:
    struct Row {
        std::size_t item = 0;
        std::vector<core::MatchRange> label_ranges;
        std::vector<core::MatchRange> description_ranges;
    };

    explicit Model(const std::vector<QuickPickItem>& items, QObject* parent)
        : QAbstractListModel(parent), items_(items) {}

    void reset(std::vector<Row> rows) {
        beginResetModel();
        rows_ = std::move(rows);
        endResetModel();
    }

    const std::vector<Row>& rows() const {
        return rows_;
    }

    const QuickPickItem& item_of(const QModelIndex& index) const {
        return items_[rows_[static_cast<std::size_t>(index.row())].item];
    }
    const Row& row_of(const QModelIndex& index) const {
        return rows_[static_cast<std::size_t>(index.row())];
    }

    int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : static_cast<int>(rows_.size());
    }

    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || role != Qt::DisplayRole) {
            return {};
        }
        return item_of(index).label;
    }

private:
    const std::vector<QuickPickItem>& items_;
    std::vector<Row> rows_;
};

class QuickPick::Delegate : public QStyledItemDelegate {
public:
    Delegate(const WorkbenchColors& colors, QObject* parent)
        : QStyledItemDelegate(parent), colors_(colors) {}

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex& index) const override {
        const auto* model = static_cast<const Model*>(index.model());
        const bool two_lines = !model->item_of(index).detail.isEmpty();
        return {0, two_lines ? detail_row_height : row_height};
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        const auto* model = static_cast<const Model*>(index.model());
        const QuickPickItem& item = model->item_of(index);
        const Model::Row& row = model->row_of(index);
        const bool selected = option.state & QStyle::State_Selected;

        painter->save();
        painter->setClipRect(option.rect);
        painter->fillRect(option.rect, colors_.color(selected ? "quickInputList.focusBackground"
                                                              : "quickInput.background"));

        const QColor text =
            colors_.color(selected ? "quickInputList.focusForeground" : "quickInput.foreground");
        const QColor dim = selected ? text : colors_.color("descriptionForeground");
        const QColor accent = selected ? text : colors_.color("list.highlightForeground");

        QFont bold = option.font;
        bold.setBold(true);
        const QFontMetrics plain_metrics(option.font);
        const QFontMetrics bold_metrics(bold);

        const int right = option.rect.right() - text_padding;
        int keybinding_width = 0;
        if (!item.keybinding.isEmpty()) {
            keybinding_width = plain_metrics.horizontalAdvance(item.keybinding);
            painter->setFont(option.font);
            painter->setPen(dim);
            painter->drawText(
                QRect(right - keybinding_width, option.rect.top(), keybinding_width, row_height),
                Qt::AlignVCenter | Qt::AlignRight, item.keybinding);
        }
        const int limit = right - keybinding_width - (keybinding_width > 0 ? text_padding : 0);
        painter->setClipRect(QRect(option.rect.left(), option.rect.top(),
                                   std::max(0, limit - option.rect.left()), option.rect.height()));

        int x = option.rect.left() + text_padding;
        const int baseline =
            option.rect.top() + (row_height - plain_metrics.height()) / 2 + plain_metrics.ascent();
        const auto draw_runs = [&](const QString& string,
                                   const std::vector<core::MatchRange>& ranges,
                                   const QColor& base) {
            qsizetype position = 0;
            const auto draw = [&](qsizetype from, qsizetype to, bool highlighted) {
                if (from >= to) {
                    return;
                }
                const QString part = string.mid(from, to - from);
                painter->setFont(highlighted ? bold : option.font);
                painter->setPen(highlighted ? accent : base);
                painter->drawText(x, baseline, part);
                x += (highlighted ? bold_metrics : plain_metrics).horizontalAdvance(part);
            };
            const auto clamp = [&](std::size_t offset) {
                return static_cast<qsizetype>(
                    std::min(offset, static_cast<std::size_t>(string.size())));
            };
            for (const core::MatchRange& range : ranges) {
                const qsizetype start = clamp(range.start);
                const qsizetype end = clamp(range.end);
                draw(position, start, false);
                draw(start, end, true);
                position = std::max(position, end);
            }
            draw(position, string.size(), false);
        };

        draw_runs(item.label, row.label_ranges, text);
        if (!item.description.isEmpty()) {
            x += text_padding;
            draw_runs(item.description, row.description_ranges, dim);
        }
        if (!item.detail.isEmpty()) {
            QFont small = option.font;
            small.setPointSizeF(option.font.pointSizeF() * 0.9);
            painter->setFont(small);
            painter->setPen(dim);
            painter->drawText(option.rect.left() + text_padding,
                              option.rect.top() + row_height + QFontMetrics(small).ascent() + 2,
                              item.detail);
        }
        painter->restore();
    }

private:
    const WorkbenchColors& colors_;
};

QuickPick::QuickPick(QWidget* window, const WorkbenchColors& colors)
    : QFrame(window), colors_(colors) {
    hide();

    filter_ = new QLineEdit(this);
    filter_->installEventFilter(this);
    list_ = new QListView(this);
    list_->setFrameShape(QFrame::NoFrame);
    list_->setFocusPolicy(Qt::NoFocus);
    list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setItemDelegate(new Delegate(colors_, list_));
    model_ = new Model(items_, this);
    list_->setModel(model_);
    empty_ = new QLabel(this);
    empty_->setContentsMargins(text_padding, 0, text_padding, 0);
    empty_->setFixedHeight(row_height);
    empty_->hide();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);
    layout->addWidget(filter_);
    layout->addWidget(list_);
    layout->addWidget(empty_);

    connect(filter_, &QLineEdit::textChanged, this, [this] { refilter(); });
    connect(list_, &QListView::clicked, this, [this](const QModelIndex& index) { choose(index); });
    connect(qApp, &QApplication::focusChanged, this,
            [this](QWidget*, QWidget* now) { on_focus_changed(now); });
    window->installEventFilter(this);
    connect(&colors_, &WorkbenchColors::changed, this, [this] { apply_colors(); });
    apply_colors();
}

QuickPick::~QuickPick() = default;

void QuickPick::apply_colors() {
    QPalette input = filter_->palette();
    input.setColor(QPalette::Base, colors_.color("input.background"));
    input.setColor(QPalette::Text, colors_.color("input.foreground"));
    input.setColor(QPalette::PlaceholderText, colors_.color("input.placeholderForeground"));
    filter_->setPalette(input);

    QPalette list = list_->palette();
    list.setColor(QPalette::Base, colors_.color("quickInput.background"));
    list_->setPalette(list);

    QPalette message = empty_->palette();
    message.setColor(QPalette::WindowText, colors_.color("descriptionForeground"));
    empty_->setPalette(message);

    list_->viewport()->update();
    update();
}

void QuickPick::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), colors_.color("quickInput.background"));
    painter.setPen(colors_.color("widget.border"));
    painter.drawRect(rect().adjusted(0, 0, -1, -1));
}

void QuickPick::set_items(std::vector<QuickPickItem> items) {
    items_ = std::move(items);
    cached_.clear();
    cached_.reserve(items_.size());
    has_detail_ = false;
    for (const QuickPickItem& item : items_) {
        cached_.push_back({item.label.toStdString(), item.description.toStdString()});
        has_detail_ = has_detail_ || !item.detail.isEmpty();
    }
    list_->setUniformItemSizes(!has_detail_);
    refilter();
}

void QuickPick::set_placeholder(const QString& text) {
    filter_->setPlaceholderText(text);
}

void QuickPick::set_empty_message(const QString& text) {
    empty_->setText(text);
}

void QuickPick::set_ignored_prefix(const QString& prefix) {
    ignored_prefix_ = prefix;
}

void QuickPick::set_top_offset(int pixels) {
    top_offset_ = pixels;
}

void QuickPick::open(const QString& text) {
    QWidget* focused = QApplication::focusWidget();
    if (focused && focused != this && !isAncestorOf(focused)) {
        previous_focus_ = focused;
    }
    filter_->setText(text);
    refilter();
    show();
    raise();
    filter_->setFocus();
    filter_->setCursorPosition(static_cast<int>(text.size()));
}

void QuickPick::cancel() {
    if (!isVisible()) {
        return;
    }
    close_popup(true);
    emit cancelled();
}

std::vector<std::size_t> QuickPick::shown_items() const {
    std::vector<std::size_t> shown;
    for (const Model::Row& row : model_->rows()) {
        shown.push_back(row.item);
    }
    return shown;
}

std::vector<core::MatchRange> QuickPick::label_highlights(std::size_t row) const {
    return row < model_->rows().size() ? model_->rows()[row].label_ranges
                                       : std::vector<core::MatchRange>{};
}

void QuickPick::refilter() {
    QString text = filter_->text();
    if (!ignored_prefix_.isEmpty() && text.startsWith(ignored_prefix_)) {
        text.remove(0, ignored_prefix_.size());
    }
    const core::PreparedQuery query = core::prepare_query(text.toStdString());

    std::vector<Model::Row> rows;
    if (query.normalized.empty()) {
        rows.resize(items_.size());
        for (std::size_t i = 0; i < rows.size(); ++i) {
            rows[i].item = i;
        }
    } else {
        std::vector<core::ItemScore> scores(items_.size());
        std::vector<std::size_t> order;
        for (std::size_t i = 0; i < items_.size(); ++i) {
            scores[i] = core::score_item({cached_[i].label, cached_[i].description, {}}, query);
            if (scores[i].score != 0) {
                order.push_back(i);
            }
        }
        std::ranges::stable_sort(order, [&](std::size_t a, std::size_t b) {
            return core::compare_items({cached_[a].label, cached_[a].description, {}}, scores[a],
                                       {cached_[b].label, cached_[b].description, {}}, scores[b],
                                       query) < 0;
        });
        rows.reserve(order.size());
        for (const std::size_t i : order) {
            rows.push_back(
                {i, std::move(scores[i].label_match), std::move(scores[i].description_match)});
        }
    }
    model_->reset(std::move(rows));
    if (model_->rowCount() > 0) {
        list_->setCurrentIndex(model_->index(0));
        list_->scrollToTop();
    }
    reposition();
}

void QuickPick::reposition() {
    QWidget* window = parentWidget();
    const int width = std::min(max_width, window->width() - 2 * window_margin);
    const int rows = model_->rowCount();

    int list_height = 0;
    if (rows > 0) {
        int content = 0;
        if (has_detail_) {
            for (int row = 0; row < rows; ++row) {
                content += model_->item_of(model_->index(row)).detail.isEmpty() ? row_height
                                                                                : detail_row_height;
            }
        } else {
            content = rows * row_height;
        }
        const int limit = std::max(3 * row_height, static_cast<int>(window->height() * 0.4));
        list_height = std::min(content, limit / row_height * row_height + 6);
    }
    list_->setVisible(rows > 0);
    empty_->setVisible(rows == 0 && !empty_->text().isEmpty());
    list_->setFixedHeight(list_height);

    setFixedWidth(std::max(0, width));
    adjustSize();
    move((window->width() - this->width()) / 2, top_offset_);
}

void QuickPick::choose(const QModelIndex& index) {
    if (!index.isValid()) {
        return;
    }
    const std::size_t item = model_->row_of(index).item;
    close_popup(true);
    emit accepted(item);
}

void QuickPick::move_selection(int rows, bool wrap) {
    const int count = model_->rowCount();
    if (count == 0) {
        return;
    }
    int target = list_->currentIndex().isValid() ? list_->currentIndex().row() + rows : 0;
    if (wrap) {
        target = ((target % count) + count) % count;
    } else {
        target = std::clamp(target, 0, count - 1);
    }
    list_->setCurrentIndex(model_->index(target));
}

void QuickPick::close_popup(bool restore_focus) {
    hide();
    if (restore_focus && previous_focus_) {
        previous_focus_->setFocus();
    }
    previous_focus_.clear();
}

void QuickPick::on_focus_changed(QWidget* now) {
    if (!isVisible() || (now && (now == this || isAncestorOf(now)))) {
        return;
    }
    close_popup(false);
    emit cancelled();
}

bool QuickPick::eventFilter(QObject* watched, QEvent* event) {
    if (watched == parentWidget()) {
        if (event->type() == QEvent::Resize && isVisible()) {
            reposition();
        }
        return false;
    }
    if (watched != filter_ || event->type() != QEvent::KeyPress) {
        return false;
    }
    const auto* key = static_cast<QKeyEvent*>(event);
    const int page = std::max(1, list_->viewport()->height() / row_height - 1);
    switch (key->key()) {
    case Qt::Key_Down:
        move_selection(1, true);
        return true;
    case Qt::Key_Up:
        move_selection(-1, true);
        return true;
    case Qt::Key_PageDown:
        move_selection(page, false);
        return true;
    case Qt::Key_PageUp:
        move_selection(-page, false);
        return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        choose(list_->currentIndex());
        return true;
    case Qt::Key_Escape:
        cancel();
        return true;
    default:
        return false;
    }
}

} // namespace anvil::ui
