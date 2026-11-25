#include "anvil/ui/explorer_view.hpp"

#include "anvil/ui/codicons.hpp"

#include <QApplication>
#include <QFontInfo>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QStackedLayout>
#include <QStyledItemDelegate>
#include <QTreeView>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>

namespace anvil::ui {
namespace {

// The sizes at zoom level 0; the view reads them through its zoom.
constexpr int row_height = 22;
constexpr int indent = 8;
constexpr int twistie_size = 16;
constexpr int label_padding = 8;
constexpr int label_right_padding = 4;
constexpr int header_padding = 20;
constexpr int header_right_padding = 8;
constexpr int header_font_pixels = 11;
constexpr int empty_top_padding = 12;
constexpr int empty_spacing = 12;
constexpr int button_vertical_padding = 4;
constexpr int button_horizontal_padding = 12;

} // namespace

// Draws the label of a row. The tree paints the background of the row and the twistie.
class ExplorerLabelDelegate : public QStyledItemDelegate {
public:
    ExplorerLabelDelegate(const WorkbenchColors& colors, const QAbstractItemView& view,
                          const QPersistentModelIndex& hovered, QObject* parent)
        : QStyledItemDelegate(parent), colors_(colors), view_(view), hovered_(hovered) {}

    void set_zoom(const Zoom* zoom) {
        zoom_ = zoom;
    }

    QColor text_color(const QModelIndex& index) const {
        if (index.data(ExplorerModel::PlaceholderRole).toBool()) {
            return colors_.color("descriptionForeground");
        }
        if (view_.selectionModel()->isSelected(index)) {
            return colors_.color(view_.hasFocus() ? "list.activeSelectionForeground"
                                                  : "list.inactiveSelectionForeground");
        }
        return colors_.color(index == hovered_ ? "list.hoverForeground" : "sideBar.foreground");
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        painter->save();
        QFont font = option.font;
        font.setItalic(index.data(ExplorerModel::PlaceholderRole).toBool());
        painter->setFont(font);
        painter->setPen(text_color(index));
        const QRect text = option.rect.adjusted(scaled_by(zoom_, label_padding), 0,
                                                -scaled_by(zoom_, label_right_padding), 0);
        painter->drawText(
            text, Qt::AlignVCenter | Qt::AlignLeft,
            option.fontMetrics.elidedText(index.data().toString(), Qt::ElideRight, text.width()));
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        return {option.fontMetrics.horizontalAdvance(index.data().toString()) +
                    scaled_by(zoom_, label_padding) + scaled_by(zoom_, label_right_padding),
                scaled_by(zoom_, row_height)};
    }

private:
    const WorkbenchColors& colors_;
    const QAbstractItemView& view_;
    const QPersistentModelIndex& hovered_;
    const Zoom* zoom_ = nullptr;
};

class ExplorerTree : public QTreeView {
public:
    ExplorerTree(const WorkbenchColors& colors, std::function<void(const QModelIndex&)> activate,
                 QWidget* parent)
        : QTreeView(parent), colors_(colors), activate_(std::move(activate)) {
        setHeaderHidden(true);
        setUniformRowHeights(true);
        setRootIsDecorated(true);
        setExpandsOnDoubleClick(false);
        setEditTriggers(NoEditTriggers);
        setSelectionMode(SingleSelection);
        setFrameShape(NoFrame);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollMode(ScrollPerPixel);
        setMouseTracking(true);
        setTextElideMode(Qt::ElideRight);
        delegate_ = new ExplorerLabelDelegate(colors, *this, hovered_, this);
        setItemDelegate(delegate_);
        apply_zoom();
        connect(this, &QTreeView::doubleClicked, this,
                [this](const QModelIndex& index) { activate_(index); });
    }

    void set_zoom(const Zoom* zoom) {
        zoom_ = zoom;
        delegate_->set_zoom(zoom);
        apply_zoom();
    }

    void setModel(QAbstractItemModel* model) override {
        QTreeView::setModel(model);
        connect(model, &QAbstractItemModel::rowsAboutToBeRemoved, this,
                [this](const QModelIndex& parent, int first, int last) {
                    remember_selection_in(parent, first, last);
                });
        connect(
            model, &QAbstractItemModel::rowsRemoved, this,
            [this](const QModelIndex& parent, int first, int) { move_selection(parent, first); });
    }

protected:
    void keyPressEvent(QKeyEvent* event) override {
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
            event->modifiers() == Qt::NoModifier && currentIndex().isValid()) {
            activate_(currentIndex());
            event->accept();
            return;
        }
        QTreeView::keyPressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        const QModelIndex index = indexAt(event->position().toPoint());
        if (index != hovered_) {
            hovered_ = index;
            viewport()->update();
        }
        QTreeView::mouseMoveEvent(event);
    }

    void leaveEvent(QEvent* event) override {
        hovered_ = QModelIndex();
        viewport()->update();
        QTreeView::leaveEvent(event);
    }

    void drawRow(QPainter* painter, const QStyleOptionViewItem& option,
                 const QModelIndex& index) const override {
        const bool selected = selectionModel()->isSelected(index);
        const bool hovered = index == hovered_ && (model()->flags(index) & Qt::ItemIsEnabled);
        const QRect row(0, option.rect.top(), viewport()->width(), option.rect.height());
        if (selected) {
            painter->fillRect(row, colors_.color(hasFocus() ? "list.activeSelectionBackground"
                                                            : "list.inactiveSelectionBackground"));
        } else if (hovered) {
            painter->fillRect(row, colors_.color("list.hoverBackground"));
        }
        QTreeView::drawRow(painter, option, index);
    }

    void drawBranches(QPainter* painter, const QRect& rect,
                      const QModelIndex& index) const override {
        if (!model()->hasChildren(index)) {
            return;
        }
        const auto* delegate = static_cast<const ExplorerLabelDelegate*>(itemDelegate());
        const auto glyph = codicon(isExpanded(index) ? "chevron-down" : "chevron-right");
        const QString family = codicon_font_family();
        if (!glyph || family.isEmpty()) {
            return;
        }
        painter->save();
        QFont font(family);
        font.setPixelSize(scaled_by(zoom_, twistie_size));
        painter->setFont(font);
        painter->setPen(delegate->text_color(index));
        const QRect twistie(rect.right() + 1 - scaled_by(zoom_, indent), rect.top(),
                            scaled_by(zoom_, twistie_size), rect.height());
        painter->drawText(twistie, Qt::AlignCenter, QString(QChar(*glyph)));
        painter->restore();
    }

private:
    void apply_zoom() {
        setIndentation(scaled_by(zoom_, indent));
        scale_scroll_bars(*this, zoom_ ? zoom_->level() : 0);
        doItemsLayout();
        viewport()->update();
    }

    // The selection of a row that is about to go, or of a row below one, goes to the row that takes
    // its place, which is what a view of a folder shows when a file is deleted under the cursor.
    void remember_selection_in(const QModelIndex& parent, int first, int last) {
        selection_moves_ = false;
        for (QModelIndex index = currentIndex(); index.isValid(); index = index.parent()) {
            if (index.parent() == parent && index.row() >= first && index.row() <= last) {
                selection_moves_ = true;
                break;
            }
        }
    }

    void move_selection(const QModelIndex& parent, int first) {
        if (!selection_moves_) {
            return;
        }
        selection_moves_ = false;
        const int count = model()->rowCount(parent);
        const QModelIndex target =
            count > 0 ? model()->index(std::min(first, count - 1), 0, parent) : parent;
        if (target.isValid()) {
            setCurrentIndex(target);
        }
    }

    const WorkbenchColors& colors_;
    std::function<void(const QModelIndex&)> activate_;
    ExplorerLabelDelegate* delegate_ = nullptr;
    const Zoom* zoom_ = nullptr;
    QPersistentModelIndex hovered_;
    bool selection_moves_ = false;
};

// The name of the folder above the tree, in capitals.
class ExplorerView::Header : public QWidget {
public:
    Header(const WorkbenchColors& colors, QWidget* parent) : QWidget(parent), colors_(colors) {
        setFixedHeight(scaled_by(zoom_, row_height));
    }

    void set_zoom(const Zoom* zoom) {
        zoom_ = zoom;
        setFixedHeight(scaled_by(zoom_, row_height));
        update();
    }

    void set_text(const QString& text) {
        name_ = text;
        update();
    }
    const QString& name() const {
        return name_;
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), colors_.color("sideBar.background"));
        QFont font = painter.font();
        font.setPixelSize(scaled_by(zoom_, header_font_pixels));
        font.setBold(true);
        painter.setFont(font);
        painter.setPen(colors_.color("sideBar.foreground"));
        const int padding = scaled_by(zoom_, header_padding);
        const QRect text(padding, 0, width() - padding - scaled_by(zoom_, header_right_padding),
                         height());
        painter.drawText(
            text, Qt::AlignVCenter | Qt::AlignLeft,
            painter.fontMetrics().elidedText(name_.toUpper(), Qt::ElideRight, text.width()));
    }

private:
    const WorkbenchColors& colors_;
    const Zoom* zoom_ = nullptr;
    QString name_;
};

ExplorerView::ExplorerView(const WorkbenchColors& colors, QWidget* parent,
                           core::DirectoryLister lister, DirectoryWatchFactory watch,
                           std::unique_ptr<ListingRunner> runner)
    : QWidget(parent), colors_(colors),
      model_(new ExplorerModel(std::move(lister), std::move(watch), std::move(runner), this)) {
    setAutoFillBackground(true);

    auto* folder_page = new QWidget(this);
    auto* folder_layout = new QVBoxLayout(folder_page);
    folder_layout->setContentsMargins(0, 0, 0, 0);
    folder_layout->setSpacing(0);
    header_ = new Header(colors_, folder_page);
    tree_ = new ExplorerTree(
        colors_,
        [this](const QModelIndex& index) {
            if (model_->data(index, ExplorerModel::PlaceholderRole).toBool()) {
                return;
            }
            if (model_->is_directory(index)) {
                tree_->setExpanded(index, !tree_->isExpanded(index));
            } else {
                emit file_activated(QString::fromStdString(model_->path_of(index).string()));
            }
        },
        folder_page);
    tree_->setModel(model_);
    connect(tree_, &QTreeView::collapsed, model_,
            [this](const QModelIndex& index) { model_->cancel_loading(index); });
    folder_layout->addWidget(header_);
    folder_layout->addWidget(tree_, 1);

    auto* empty_page = new QWidget(this);
    auto* empty_layout = new QVBoxLayout(empty_page);
    empty_layout_ = empty_layout;
    message_ = new QLabel(tr("You have not yet opened a folder."), empty_page);
    message_->setWordWrap(true);
    open_folder_ = new QPushButton(tr("Open Folder"), empty_page);
    open_folder_->setCursor(Qt::PointingHandCursor);
    open_folder_->setAutoDefault(false);
    empty_layout->addWidget(message_);
    empty_layout->addWidget(open_folder_);
    empty_layout->addStretch(1);

    pages_ = new QStackedLayout(this);
    pages_->addWidget(empty_page);
    pages_->addWidget(folder_page);
    pages_->setCurrentWidget(empty_page);

    connect(open_folder_, &QPushButton::clicked, this, &ExplorerView::open_folder_requested);
    connect(model_, &ExplorerModel::watch_problem, this, &ExplorerView::watch_problem);
    connect(&colors_, &WorkbenchColors::changed, this, [this] { apply_colors(); });
    base_font_pixels_ = QFontInfo(font()).pixelSize();
    apply_zoom();
}

void ExplorerView::set_zoom(const Zoom* zoom) {
    if (zoom_) {
        disconnect(zoom_, &Zoom::changed, this, nullptr);
    }
    zoom_ = zoom;
    if (zoom_) {
        connect(zoom_, &Zoom::changed, this, &ExplorerView::apply_zoom);
    }
    apply_zoom();
}

void ExplorerView::apply_zoom() {
    header_->set_zoom(zoom_);
    static_cast<ExplorerTree*>(tree_)->set_zoom(zoom_);
    QFont view_font = font();
    view_font.setPixelSize(scaled_by(zoom_, base_font_pixels_));
    setFont(view_font);
    // A push button has a font of its own that a parent's font does not reach.
    message_->setFont(view_font);
    open_folder_->setFont(view_font);
    const int padding = scaled_by(zoom_, header_padding);
    empty_layout_->setContentsMargins(padding, scaled_by(zoom_, empty_top_padding), padding, 0);
    empty_layout_->setSpacing(scaled_by(zoom_, empty_spacing));
    apply_colors();
}

ExplorerView::~ExplorerView() = default;

void ExplorerView::apply_colors() {
    QPalette palette = this->palette();
    palette.setColor(QPalette::Window, colors_.color("sideBar.background"));
    palette.setColor(QPalette::WindowText, colors_.color("sideBar.foreground"));
    setPalette(palette);
    message_->setPalette(palette);
    open_folder_->setStyleSheet(
        QStringLiteral(
            "QPushButton { background: %1; color: %2; border: none; padding: %4px %5px; }"
            "QPushButton:hover { background: %3; }")
            .arg(colors_.color("button.background").name(QColor::HexArgb),
                 colors_.color("button.foreground").name(QColor::HexArgb),
                 colors_.color("button.hoverBackground").name(QColor::HexArgb))
            .arg(scaled_by(zoom_, button_vertical_padding))
            .arg(scaled_by(zoom_, button_horizontal_padding)));
    QPalette tree_palette = tree_->palette();
    tree_palette.setColor(QPalette::Base, colors_.color("sideBar.background"));
    // The tree draws the selection itself; the style would paint the Highlight over the twistie
    // column.
    tree_palette.setColor(QPalette::Highlight, Qt::transparent);
    tree_->setPalette(tree_palette);
    tree_->viewport()->update();
    header_->update();
}

void ExplorerView::set_workspace(std::optional<std::filesystem::path> folder, const QString& name) {
    const bool has_folder = folder.has_value();
    model_->set_folder(std::move(folder));
    header_->set_text(name);
    pages_->setCurrentIndex(has_folder ? 1 : 0);
}

void ExplorerView::set_excludes(core::FileExcludes excludes) {
    model_->set_excludes(std::move(excludes));
}

bool ExplorerView::shows_folder() const {
    return pages_->currentIndex() == 1;
}

QString ExplorerView::folder_name() const {
    return header_->name();
}

} // namespace anvil::ui
