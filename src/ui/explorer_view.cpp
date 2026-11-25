#include "anvil/ui/explorer_view.hpp"

#include "anvil/ui/codicons.hpp"

#include <QApplication>
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

constexpr int row_height = 22;
constexpr int indent = 8;
constexpr int twistie_size = 16;
constexpr int label_padding = 8;
constexpr int header_padding = 20;

} // namespace

// Draws the label of a row. The tree paints the background of the row and the twistie.
class ExplorerLabelDelegate : public QStyledItemDelegate {
public:
    ExplorerLabelDelegate(const WorkbenchColors& colors, const QAbstractItemView& view,
                          const QPersistentModelIndex& hovered, QObject* parent)
        : QStyledItemDelegate(parent), colors_(colors), view_(view), hovered_(hovered) {}

    QColor text_color(const QModelIndex& index) const {
        if (view_.selectionModel()->isSelected(index)) {
            return colors_.color(view_.hasFocus() ? "list.activeSelectionForeground"
                                                  : "list.inactiveSelectionForeground");
        }
        return colors_.color(index == hovered_ ? "list.hoverForeground" : "sideBar.foreground");
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        painter->save();
        painter->setFont(option.font);
        painter->setPen(text_color(index));
        const QRect text = option.rect.adjusted(label_padding, 0, -4, 0);
        painter->drawText(
            text, Qt::AlignVCenter | Qt::AlignLeft,
            option.fontMetrics.elidedText(index.data().toString(), Qt::ElideRight, text.width()));
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        return {option.fontMetrics.horizontalAdvance(index.data().toString()) + label_padding + 4,
                row_height};
    }

private:
    const WorkbenchColors& colors_;
    const QAbstractItemView& view_;
    const QPersistentModelIndex& hovered_;
};

class ExplorerTree : public QTreeView {
public:
    ExplorerTree(const WorkbenchColors& colors, std::function<void(const QModelIndex&)> activate,
                 QWidget* parent)
        : QTreeView(parent), colors_(colors), activate_(std::move(activate)) {
        setHeaderHidden(true);
        setUniformRowHeights(true);
        setIndentation(indent);
        setRootIsDecorated(true);
        setExpandsOnDoubleClick(false);
        setEditTriggers(NoEditTriggers);
        setSelectionMode(SingleSelection);
        setFrameShape(NoFrame);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollMode(ScrollPerPixel);
        setMouseTracking(true);
        setTextElideMode(Qt::ElideRight);
        setItemDelegate(new ExplorerLabelDelegate(colors, *this, hovered_, this));
        connect(this, &QTreeView::doubleClicked, this,
                [this](const QModelIndex& index) { activate_(index); });
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
        const bool hovered = index == hovered_;
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
        font.setPixelSize(twistie_size);
        painter->setFont(font);
        painter->setPen(delegate->text_color(index));
        const QRect twistie(rect.right() + 1 - indent, rect.top(), twistie_size, rect.height());
        painter->drawText(twistie, Qt::AlignCenter, QString(QChar(*glyph)));
        painter->restore();
    }

private:
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
    QPersistentModelIndex hovered_;
    bool selection_moves_ = false;
};

// The name of the folder above the tree, in capitals.
class ExplorerView::Header : public QWidget {
public:
    Header(const WorkbenchColors& colors, QWidget* parent) : QWidget(parent), colors_(colors) {
        setFixedHeight(row_height);
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
        font.setPixelSize(11);
        font.setBold(true);
        painter.setFont(font);
        painter.setPen(colors_.color("sideBar.foreground"));
        const QRect text(header_padding, 0, width() - header_padding - 8, height());
        painter.drawText(
            text, Qt::AlignVCenter | Qt::AlignLeft,
            painter.fontMetrics().elidedText(name_.toUpper(), Qt::ElideRight, text.width()));
    }

private:
    const WorkbenchColors& colors_;
    QString name_;
};

ExplorerView::ExplorerView(const WorkbenchColors& colors, QWidget* parent,
                           core::DirectoryLister lister, DirectoryWatchFactory watch)
    : QWidget(parent), colors_(colors),
      model_(new ExplorerModel(std::move(lister), std::move(watch), this)) {
    setAutoFillBackground(true);

    auto* folder_page = new QWidget(this);
    auto* folder_layout = new QVBoxLayout(folder_page);
    folder_layout->setContentsMargins(0, 0, 0, 0);
    folder_layout->setSpacing(0);
    header_ = new Header(colors_, folder_page);
    tree_ = new ExplorerTree(
        colors_,
        [this](const QModelIndex& index) {
            if (model_->is_directory(index)) {
                tree_->setExpanded(index, !tree_->isExpanded(index));
            } else {
                emit file_activated(QString::fromStdString(model_->path_of(index).string()));
            }
        },
        folder_page);
    tree_->setModel(model_);
    folder_layout->addWidget(header_);
    folder_layout->addWidget(tree_, 1);

    auto* empty_page = new QWidget(this);
    auto* empty_layout = new QVBoxLayout(empty_page);
    empty_layout->setContentsMargins(header_padding, 12, header_padding, 0);
    empty_layout->setSpacing(12);
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
        QStringLiteral("QPushButton { background: %1; color: %2; border: none; padding: 4px 12px; }"
                       "QPushButton:hover { background: %3; }")
            .arg(colors_.color("button.background").name(QColor::HexArgb),
                 colors_.color("button.foreground").name(QColor::HexArgb),
                 colors_.color("button.hoverBackground").name(QColor::HexArgb)));
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
