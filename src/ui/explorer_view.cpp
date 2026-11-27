#include "anvil/ui/explorer_view.hpp"

#include "anvil/exthost/workspace_file_edit.hpp"
#include "anvil/platform/file.hpp"
#include "anvil/platform/file_system.hpp"
#include "anvil/ui/codicons.hpp"
#include "anvil/ui/scroll_bar.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QContextMenuEvent>
#include <QFile>
#include <QFontInfo>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QStackedLayout>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>
#include <set>

namespace anvil::ui {
namespace {

// The sizes at zoom level 0; the view reads them through its zoom.
constexpr int row_height = 22;
constexpr int indent = 8;
constexpr int twistie_size = 16;
// From the start of a row's item to its label: a column of 16 px for the twistie of a folder or
// the icon of a file, and 6 px. The tree puts an item one indent after the start of its level, so
// the column of the first level is one indent from the edge, as in VS Code.
constexpr int label_padding = 22;
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

    // The field for a name is made by the explorer, which knows what the name is for.
    using EditorFactory = std::function<QWidget*(QWidget*, const QModelIndex&)>;
    void set_editor_factory(EditorFactory factory) {
        make_editor_ = std::move(factory);
    }

    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem&,
                          const QModelIndex& index) const override {
        return make_editor_ ? make_editor_(parent, index) : nullptr;
    }
    // The field handles its own keys and focus: the delegate's filter would commit a name on Enter
    // and on losing the focus, and close the field on Escape, before the explorer has looked at it.
    bool eventFilter(QObject*, QEvent*) override {
        return false;
    }
    void setEditorData(QWidget*, const QModelIndex&) const override {}
    void setModelData(QWidget*, QAbstractItemModel*, const QModelIndex&) const override {}
    void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option,
                              const QModelIndex&) const override {
        editor->setGeometry(option.rect.adjusted(scaled_by(zoom_, label_padding - 4), 1, -2, -1));
    }

private:
    const WorkbenchColors& colors_;
    const QAbstractItemView& view_;
    const QPersistentModelIndex& hovered_;
    const Zoom* zoom_ = nullptr;
    EditorFactory make_editor_;
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
        setSelectionMode(ExtendedSelection);
        hover_expand_.setSingleShot(true);
        hover_expand_.setInterval(hover_expand_ms);
        connect(&hover_expand_, &QTimer::timeout, this, [this] {
            if (dragging() && drop_target_.isValid()) {
                expand(drop_target_);
            }
        });
        setFrameShape(NoFrame);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto* bar = new ScrollBar(Qt::Vertical, ScrollBar::list_width, "sideBar.background");
        bar->set_colors(&colors);
        setVerticalScrollBar(bar);
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

    void set_file_language(ExplorerView::FileLanguage language) {
        file_language_ = std::move(language);
        viewport()->update();
    }

    std::optional<SetiGlyph> file_icon(const QModelIndex& index) const {
        if (!index.isValid() || index.data(ExplorerModel::IsDirectoryRole).toBool() ||
            index.data(ExplorerModel::PlaceholderRole).toBool()) {
            return std::nullopt;
        }
        const std::string name = index.data().toString().toStdString();
        std::optional<SetiIcon> icon = seti_icon_by_name(name);
        if (!icon) {
            // The row of a file that is being named has no name, and no path to ask about.
            const std::string language =
                file_language_ && !name.empty()
                    ? file_language_(static_cast<const ExplorerModel*>(model())->path_of(index))
                    : std::string();
            icon = seti_file_icon(name, language);
        }
        return colors_.kind() == core::ThemeKind::light ? icon->light : icon->dark;
    }

    // What the explorer does with a drag; the tree only follows the pointer.
    struct DragHandlers {
        std::function<std::vector<std::filesystem::path>(const QModelIndexList&)> sources;
        std::function<std::optional<QModelIndex>(const std::vector<std::filesystem::path>&,
                                                 const QModelIndex&)>
            folder;
        std::function<void(const std::vector<std::filesystem::path>&, const QModelIndex&, bool)>
            drop;
    };
    void set_drag_handlers(DragHandlers handlers) {
        drag_handlers_ = std::move(handlers);
    }
    void set_hover_expand_delay(int milliseconds) {
        hover_expand_.setInterval(milliseconds);
    }
    // Whether a drag is over a place that takes the rows, and the folder, invalid for the folder of
    // the workspace.
    std::optional<QModelIndex> drop_folder() const {
        return dragging() && takes_drop_ ? std::optional<QModelIndex>(drop_target_) : std::nullopt;
    }

    void set_editor_factory(ExplorerLabelDelegate::EditorFactory factory) {
        static_cast<ExplorerLabelDelegate*>(itemDelegate())->set_editor_factory(std::move(factory));
    }
    void set_context_menu_handler(std::function<void(const QPoint&, const QPoint&)> handler) {
        context_menu_ = std::move(handler);
    }
    void close_name_editor(QWidget* editor) {
        closeEditor(editor, QAbstractItemDelegate::NoHint);
    }

    // Makes the row the current one and the only selected one.
    void select_only(const QModelIndex& index) {
        selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect |
                                                     QItemSelectionModel::Rows);
    }

    void setModel(QAbstractItemModel* model) override {
        // Before the base class connects, which moves the current row away from one that goes.
        connect(model, &QAbstractItemModel::rowsAboutToBeRemoved, this,
                [this](const QModelIndex& parent, int first, int last) {
                    remember_selection_in(parent, first, last);
                });
        QTreeView::setModel(model);
        connect(
            model, &QAbstractItemModel::rowsRemoved, this,
            [this](const QModelIndex& parent, int first, int) { move_selection(parent, first); });
    }

protected:
    void contextMenuEvent(QContextMenuEvent* event) override {
        if (context_menu_) {
            context_menu_(event->pos(), event->globalPos());
        }
    }

    void keyPressEvent(QKeyEvent* event) override {
        if (dragging()) {
            if (event->key() == Qt::Key_Escape) {
                end_drag();
            } else if (event->key() == Qt::Key_Control) {
                show_drag_cursor(true);
            }
            event->accept();
            return;
        }
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
            event->modifiers() == Qt::NoModifier && currentIndex().isValid()) {
            activate_(currentIndex());
            event->accept();
            return;
        }
        QTreeView::keyPressEvent(event);
    }

    void keyReleaseEvent(QKeyEvent* event) override {
        if (dragging() && event->key() == Qt::Key_Control) {
            show_drag_cursor(false);
        }
        QTreeView::keyReleaseEvent(event);
    }

    // The tree drags the rows itself: `QDrag` runs a loop of its own, and the selection that is
    // dragged is the one at the press, which a press with a modifier would change.
    void mousePressEvent(QMouseEvent* event) override {
        end_drag();
        const QPoint position = event->position().toPoint();
        const QModelIndex index = indexAt(position);
        if (press_on_twistie(event)) {
            return;
        }
        // Left of the label of a folder is the twistie, which a press opens and closes; the icon
        // there of a file is part of its row.
        if (event->button() == Qt::LeftButton && index.isValid() &&
            (model()->flags(index) & Qt::ItemIsSelectable) &&
            !index.data(ExplorerModel::PendingRole).toBool() &&
            (position.x() >= visualRect(index).left() ||
             !index.data(ExplorerModel::IsDirectoryRole).toBool())) {
            press_ = position;
            pressed_rows_ = selectionModel()->isSelected(index) ? selectionModel()->selectedRows()
                                                                : QModelIndexList{index};
        }
        QTreeView::mousePressEvent(event);
    }

    // The second press of a double click is a press on the twistie like the first, not the
    // double click that opens or closes the row.
    void mouseDoubleClickEvent(QMouseEvent* event) override {
        if (!press_on_twistie(event)) {
            QTreeView::mouseDoubleClickEvent(event);
        }
    }

    void mouseReleaseEvent(QMouseEvent* event) override {
        if (!dragging() || event->button() != Qt::LeftButton) {
            pressed_rows_.clear();
            QTreeView::mouseReleaseEvent(event);
            return;
        }
        const std::vector<std::filesystem::path> sources = sources_;
        const std::optional<QModelIndex> folder = folder_at(event->position().toPoint());
        const bool copy = event->modifiers() & Qt::ControlModifier;
        end_drag();
        setState(NoState);
        event->accept();
        if (folder) {
            drag_handlers_.drop(sources, *folder, copy);
        }
    }

    void paintEvent(QPaintEvent* event) override {
        QTreeView::paintEvent(event);
        if (drop_folder() != std::optional<QModelIndex>(QModelIndex())) {
            return;
        }
        // The space below the rows is part of the folder of the workspace.
        int bottom = 0;
        for (QModelIndex index = model()->index(0, 0); index.isValid(); index = indexBelow(index)) {
            bottom = visualRect(index).bottom() + 1;
        }
        QPainter painter(viewport());
        painter.fillRect(QRect(0, bottom, viewport()->width(), viewport()->height() - bottom),
                         colors_.color("list.dropBackground"));
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (!pressed_rows_.isEmpty() && (event->buttons() & Qt::LeftButton)) {
            drag_to(event->position().toPoint(), event->modifiers() & Qt::ControlModifier);
            return;
        }
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
        } else if (takes_drop(index)) {
            painter->fillRect(row, colors_.color("list.dropBackground"));
        } else if (hovered && !dragging()) {
            painter->fillRect(row, colors_.color("list.hoverBackground"));
        }
        QTreeView::drawRow(painter, option, index);
    }

    void drawBranches(QPainter* painter, const QRect& rect,
                      const QModelIndex& index) const override {
        if (!index.data(ExplorerModel::IsDirectoryRole).toBool()) {
            draw_file_icon(painter, rect, index);
            return;
        }
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
        const QRect twistie(rect.right() + 1, rect.top(), scaled_by(zoom_, twistie_size),
                            rect.height());
        painter->drawText(twistie, Qt::AlignCenter, QString(QChar(*glyph)));
        painter->restore();
    }

private:
    static constexpr int hover_expand_ms = 500;

    // Opens or closes the folder whose twistie the left button was pressed on. The tree does that
    // itself for a press before the item of the row, which covers the indent left of the twistie.
    bool press_on_twistie(QMouseEvent* event) {
        const QPoint position = event->position().toPoint();
        const QModelIndex index = indexAt(position);
        if (event->button() != Qt::LeftButton || !index.isValid() ||
            !index.data(ExplorerModel::IsDirectoryRole).toBool() || !model()->hasChildren(index)) {
            return false;
        }
        const int left = visualRect(index).left();
        if (position.x() < left || position.x() >= left + scaled_by(zoom_, twistie_size)) {
            return false;
        }
        setExpanded(index, !isExpanded(index));
        event->accept();
        return true;
    }

    // In the column that the twistie of a folder has, where the item of the row starts.
    void draw_file_icon(QPainter* painter, const QRect& branches, const QModelIndex& index) const {
        const std::optional<SetiGlyph> glyph = file_icon(index);
        const QString family = seti_font_family();
        if (!glyph || family.isEmpty()) {
            return;
        }
        painter->save();
        QFont icon_font(family);
        const int icon_pixels = QFontInfo(font()).pixelSize() * seti_font_percent / 100;
        icon_font.setPixelSize(icon_pixels);
        painter->setFont(icon_font);
        painter->setPen(QColor::fromRgba(glyph->color));
        const int width = scaled_by(zoom_, twistie_size);
        const int left = branches.right() + 1;
        // Wider than the column, so that a glyph that fills its square is not cut.
        painter->drawText(QRect(left - width / 2, branches.top(), 2 * width, branches.height()),
                          Qt::AlignCenter, QString(QChar(glyph->code)));
        painter->restore();
    }

    bool dragging() const {
        return !sources_.empty();
    }

    std::optional<QModelIndex> folder_at(const QPoint& position) const {
        if (!viewport()->rect().contains(position)) {
            return std::nullopt;
        }
        return drag_handlers_.folder(sources_, indexAt(position));
    }

    void drag_to(const QPoint& position, bool copy) {
        if (!dragging()) {
            if ((position - press_).manhattanLength() < QApplication::startDragDistance()) {
                return;
            }
            sources_ = drag_handlers_.sources(pressed_rows_);
            if (sources_.empty()) {
                return;
            }
        }
        const std::optional<QModelIndex> folder = folder_at(position);
        takes_drop_ = folder.has_value();
        const QModelIndex target = folder.value_or(QModelIndex());
        if (target != drop_target_ || !takes_drop_) {
            drop_target_ = target;
            hover_expand_.stop();
            if (target.isValid() && !isExpanded(target)) {
                hover_expand_.start();
            }
        }
        show_drag_cursor(copy);
        viewport()->update();
    }

    void show_drag_cursor(bool copy) {
        viewport()->setCursor(!takes_drop_ ? Qt::ForbiddenCursor
                              : copy       ? Qt::DragCopyCursor
                                           : Qt::DragMoveCursor);
    }

    void end_drag() {
        const bool was_dragging = dragging();
        pressed_rows_.clear();
        sources_.clear();
        takes_drop_ = false;
        drop_target_ = QModelIndex();
        hover_expand_.stop();
        if (was_dragging) {
            viewport()->unsetCursor();
            viewport()->update();
        }
    }

    // Whether the row is the folder that a drag would drop into, or below it.
    bool takes_drop(const QModelIndex& index) const {
        if (!dragging() || !takes_drop_) {
            return false;
        }
        if (!drop_target_.isValid()) {
            return true;
        }
        for (QModelIndex row = index; row.isValid(); row = row.parent()) {
            if (row == drop_target_) {
                return true;
            }
        }
        return false;
    }

    void apply_zoom() {
        setIndentation(scaled_by(zoom_, indent));
        static_cast<ScrollBar*>(verticalScrollBar())->set_zoom_level(zoom_ ? zoom_->level() : 0);
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
            select_only(target);
        }
    }

    const WorkbenchColors& colors_;
    std::function<void(const QModelIndex&)> activate_;
    ExplorerLabelDelegate* delegate_ = nullptr;
    const Zoom* zoom_ = nullptr;
    ExplorerView::FileLanguage file_language_;
    QPersistentModelIndex hovered_;
    bool selection_moves_ = false;
    std::function<void(const QPoint&, const QPoint&)> context_menu_;
    DragHandlers drag_handlers_;
    QPoint press_;
    // The rows of a press that can become a drag.
    QModelIndexList pressed_rows_;
    // What is being dragged; empty when nothing is.
    std::vector<std::filesystem::path> sources_;
    bool takes_drop_ = false;
    QPersistentModelIndex drop_target_;
    QTimer hover_expand_;
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

// The field for the name of a new item or of a renamed one, and under it the reason why the name is
// refused. The reason is a child of the tree's viewport, like the field.
class ExplorerView::NameEditor : public QLineEdit {
public:
    NameEditor(const WorkbenchColors& colors, QWidget* parent)
        : QLineEdit(parent), problem_(new QLabel(parent)) {
        setStyleSheet(
            QStringLiteral("QLineEdit { background: %1; color: %2; border: 1px solid %3; "
                           "padding: 0 3px; selection-background-color: %4; }")
                .arg(colors.color("input.background").name(QColor::HexArgb),
                     colors.color("input.foreground").name(QColor::HexArgb),
                     colors.color("focusBorder").name(QColor::HexArgb),
                     colors.color("list.activeSelectionBackground").name(QColor::HexArgb)));
        problem_->setWordWrap(true);
        problem_->setMargin(3);
        problem_->setStyleSheet(
            QStringLiteral("QLabel { background: %1; color: %2; border: 1px solid %3; }")
                .arg(colors.color("inputValidation.errorBackground").name(QColor::HexArgb),
                     colors.color("inputValidation.errorForeground").name(QColor::HexArgb),
                     colors.color("inputValidation.errorBorder").name(QColor::HexArgb)));
        problem_->hide();
    }
    ~NameEditor() override {
        delete problem_;
    }

    std::function<void(const QString&)> on_accept;
    std::function<void()> on_cancel;
    std::function<void()> on_blur;

    // Shows why the name is refused under the field, or nothing for an empty text.
    void set_problem(const QString& text) {
        problem_->setText(text);
        problem_->setVisible(!text.isEmpty());
        place_problem();
    }
    QString problem() const {
        return problem_->text();
    }

    // A line edit and a label have fonts of their own that the font of the view does not reach.
    void set_scaled_font(const QFont& font) {
        setFont(font);
        problem_->setFont(font);
        place_problem();
    }

protected:
    void keyPressEvent(QKeyEvent* event) override {
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
            event->modifiers() == Qt::NoModifier) {
            event->accept();
            if (on_accept) {
                on_accept(text());
            }
            return;
        }
        if (event->key() == Qt::Key_Escape && event->modifiers() == Qt::NoModifier) {
            event->accept();
            if (on_cancel) {
                on_cancel();
            }
            return;
        }
        QLineEdit::keyPressEvent(event);
    }

    void focusOutEvent(QFocusEvent* event) override {
        QLineEdit::focusOutEvent(event);
        if (event->reason() != Qt::ActiveWindowFocusReason &&
            event->reason() != Qt::PopupFocusReason && on_blur) {
            on_blur();
        }
    }

    void moveEvent(QMoveEvent* event) override {
        QLineEdit::moveEvent(event);
        place_problem();
    }
    void resizeEvent(QResizeEvent* event) override {
        QLineEdit::resizeEvent(event);
        place_problem();
    }

private:
    void place_problem() {
        if (!problem_->isVisible()) {
            return;
        }
        const int width = std::max(width_hint(), this->width());
        problem_->setFixedWidth(width);
        problem_->setFixedHeight(problem_->heightForWidth(width));
        problem_->move(x(), y() + height() + 1);
        problem_->raise();
    }
    int width_hint() const {
        return parentWidget() ? parentWidget()->width() - x() - 4 : width();
    }

    QLabel* problem_;
};

ExplorerActions default_explorer_actions() {
    return {
        .move_to_trash =
            [](const std::filesystem::path& path) {
                return QFile::moveToTrash(QString::fromStdString(path.string()));
            },
        .confirm =
            [](QWidget* parent, const QString& message, const QString& detail,
               const QString& button) {
                QMessageBox box(QMessageBox::Warning, QString(), message, QMessageBox::NoButton,
                                parent);
                box.setInformativeText(detail);
                QPushButton* accept = box.addButton(button, QMessageBox::AcceptRole);
                box.addButton(QMessageBox::Cancel);
                box.setDefaultButton(accept);
                box.exec();
                return box.clickedButton() == accept;
            },
        .confirm_move =
            [](QWidget* parent, const QString& message, const QString& detail) {
                QMessageBox box(QMessageBox::Question, QString(), message, QMessageBox::NoButton,
                                parent);
                box.setInformativeText(detail);
                QPushButton* accept =
                    box.addButton(ExplorerView::tr("Move"), QMessageBox::AcceptRole);
                box.addButton(QMessageBox::Cancel);
                box.setDefaultButton(accept);
                auto* again = new QCheckBox(ExplorerView::tr("Do not ask me again"));
                box.setCheckBox(again);
                box.exec();
                return MoveAnswer{.move = box.clickedButton() == accept,
                                  .do_not_ask_again = again->isChecked()};
            },
    };
}

ExplorerView::ExplorerView(const WorkbenchColors& colors, QWidget* parent,
                           core::DirectoryLister lister, DirectoryWatchFactory watch,
                           std::unique_ptr<ListingRunner> runner, ExplorerActions actions)
    : QWidget(parent), colors_(colors),
      model_(new ExplorerModel(std::move(lister), std::move(watch), std::move(runner), this)),
      actions_(std::move(actions)) {
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
    auto* tree = static_cast<ExplorerTree*>(tree_);
    tree->set_editor_factory(
        [this](QWidget* parent, const QModelIndex& row) { return make_name_editor(parent, row); });
    tree->set_context_menu_handler([this](const QPoint& local, const QPoint& global) {
        const QModelIndex under = tree_->indexAt(local);
        if (under.isValid() && model_->flags(under) & Qt::ItemIsSelectable &&
            !model_->data(under, ExplorerModel::PendingRole).toBool()) {
            // A right click on a selected row keeps the selection.
            if (tree_->selectionModel()->isSelected(under)) {
                tree_->selectionModel()->setCurrentIndex(under, QItemSelectionModel::NoUpdate);
            } else {
                tree_->setCurrentIndex(under);
            }
        } else {
            tree_->clearSelection();
            tree_->setCurrentIndex(QModelIndex());
        }
        emit context_menu_requested(global);
    });
    connect(tree_, &QTreeView::collapsed, model_, [this](const QModelIndex& index) {
        model_->cancel_loading(index);
        if (new_request_ && !new_request_->root && new_request_->directory == index) {
            new_request_.reset();
        }
        if (editing_) {
            for (QModelIndex row = editing_->row; row.isValid(); row = row.parent()) {
                if (row == index) {
                    finish_editing();
                    break;
                }
            }
        }
    });
    connect(model_, &ExplorerModel::directory_read, this, [this] { continue_new(); });
    connect(model_, &ExplorerModel::directory_read, this, &ExplorerView::expand_reached);
    connect(tree_, &QTreeView::expanded, this, [this] { emit expanded_folders_changed(); });
    connect(tree_, &QTreeView::collapsed, this, [this] { emit expanded_folders_changed(); });
    tree->set_drag_handlers(
        {.sources = [this](const QModelIndexList& rows) { return paths_of(rows); },
         .folder = [this](const std::vector<std::filesystem::path>& sources,
                          const QModelIndex& under) { return folder_to_drop_into(sources, under); },
         .drop =
             [this](const std::vector<std::filesystem::path>& sources, const QModelIndex& folder,
                    bool copy) {
                 drop_items(sources, folder.isValid() ? model_->path_of(folder) : *model_->folder(),
                            copy);
             }});
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
    if (editor_) {
        editor_->set_scaled_font(view_font);
    }
    const int padding = scaled_by(zoom_, header_padding);
    empty_layout_->setContentsMargins(padding, scaled_by(zoom_, empty_top_padding), padding, 0);
    empty_layout_->setSpacing(scaled_by(zoom_, empty_spacing));
    apply_colors();
}

ExplorerView::~ExplorerView() {
    *alive_ = false;
}

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
    finish_editing();
    new_request_.reset();
    unreached_.clear();
    model_->set_folder(std::move(folder));
    header_->set_text(name);
    pages_->setCurrentIndex(has_folder ? 1 : 0);
}

void ExplorerView::set_excludes(core::FileExcludes excludes) {
    model_->set_excludes(std::move(excludes));
}

void ExplorerView::set_file_language(FileLanguage language) {
    static_cast<ExplorerTree*>(tree_)->set_file_language(std::move(language));
}

void ExplorerView::refresh_icons() {
    tree_->viewport()->update();
}

std::optional<SetiGlyph> ExplorerView::file_icon(const QModelIndex& row) const {
    return static_cast<const ExplorerTree*>(tree_)->file_icon(row);
}

namespace {

bool is_real_row(const ExplorerModel& model, const QModelIndex& index) {
    return index.isValid() && !model.data(index, ExplorerModel::PlaceholderRole).toBool() &&
           !model.data(index, ExplorerModel::PendingRole).toBool();
}

std::string file_name_of(const std::filesystem::path& path) {
    return path.filename().string();
}

} // namespace

std::string ExplorerView::relative_path(const QModelIndex& directory) const {
    return model_->path_of(directory).lexically_relative(*model_->folder()).generic_string();
}

std::vector<std::string> ExplorerView::expanded_folders() const {
    std::set<std::string> folders = unreached_;
    if (model_->folder()) {
        collect_expanded({}, folders);
    }
    return {folders.begin(), folders.end()};
}

void ExplorerView::collect_expanded(const QModelIndex& directory,
                                    std::set<std::string>& folders) const {
    for (int row = 0; row < model_->rowCount(directory); ++row) {
        const QModelIndex index = model_->index(row, 0, directory);
        if (!is_real_row(*model_, index) || !model_->is_directory(index)) {
            continue;
        }
        if (tree_->isExpanded(index)) {
            folders.insert(relative_path(index));
        }
        collect_expanded(index, folders);
    }
}

void ExplorerView::restore_expanded(const std::vector<std::string>& folders) {
    unreached_.clear();
    if (!model_->folder()) {
        return;
    }
    for (const std::string& folder : folders) {
        const std::filesystem::path path(folder);
        // What was saved names a folder below the workspace's, in its shortest form.
        if (path.is_relative() && path.has_filename() && path.lexically_normal() == path &&
            *path.begin() != "." && *path.begin() != "..") {
            unreached_.insert(path.generic_string());
        }
    }
    expand_reached({});
}

void ExplorerView::expand_reached(const QModelIndex& directory) {
    if (unreached_.empty() || !model_->is_read(directory)) {
        return;
    }
    std::set<std::string> directories;
    for (int row = 0; row < model_->rowCount(directory); ++row) {
        const QModelIndex index = model_->index(row, 0, directory);
        if (!is_real_row(*model_, index) || !model_->is_directory(index)) {
            continue;
        }
        directories.insert(file_name_of(model_->path_of(index)));
        if (unreached_.erase(relative_path(index)) > 0) {
            tree_->expand(index);
        }
        expand_reached(index);
    }

    const std::string prefix = directory.isValid() ? relative_path(directory) + "/" : std::string();
    const std::filesystem::path path =
        directory.isValid() ? model_->path_of(directory) : *model_->folder();
    // A folder without a row can still be there, hidden by `files.exclude` or not readable.
    const auto dropped = std::erase_if(unreached_, [&](const std::string& folder) {
        if (!folder.starts_with(prefix)) {
            return false;
        }
        const std::string name =
            folder.substr(prefix.size(), folder.find('/', prefix.size()) - prefix.size());
        if (directories.contains(name)) {
            return false;
        }
        std::error_code error;
        const std::filesystem::file_type type = std::filesystem::status(path / name, error).type();
        return type == std::filesystem::file_type::not_found ||
               (!error && type != std::filesystem::file_type::directory);
    });
    if (dropped > 0) {
        emit expanded_folders_changed();
    }
}

std::vector<std::filesystem::path> ExplorerView::selected_paths() const {
    return paths_of(tree_->selectionModel()->selectedRows());
}

std::vector<std::filesystem::path> ExplorerView::paths_of(const QModelIndexList& rows) const {
    std::vector<std::filesystem::path> paths;
    for (const QModelIndex& row : rows) {
        if (is_real_row(*model_, row)) {
            paths.push_back(model_->path_of(row));
        }
    }
    std::ranges::sort(paths);
    std::vector<std::filesystem::path> outermost;
    for (const std::filesystem::path& path : paths) {
        if (std::ranges::none_of(outermost, [&](const std::filesystem::path& other) {
                return exthost::is_path_within(path, other);
            })) {
            outermost.push_back(path);
        }
    }
    return outermost;
}

QModelIndex ExplorerView::focused_row() const {
    const QModelIndex current = tree_->currentIndex();
    if (is_real_row(*model_, current) && tree_->selectionModel()->isSelected(current)) {
        return current;
    }
    for (const QModelIndex& row : tree_->selectionModel()->selectedRows()) {
        if (is_real_row(*model_, row)) {
            return row;
        }
    }
    return {};
}

std::optional<std::filesystem::path> ExplorerView::focused_path() const {
    const QModelIndex row = focused_row();
    return row.isValid() ? std::optional(model_->path_of(row)) : std::nullopt;
}

bool ExplorerView::selection_is_directory() const {
    const QModelIndex row = focused_row();
    return !row.isValid() || model_->is_directory(row);
}

void ExplorerView::set_hover_expand_delay(int milliseconds) {
    static_cast<ExplorerTree*>(tree_)->set_hover_expand_delay(milliseconds);
}

std::optional<std::filesystem::path> ExplorerView::drop_target() const {
    const auto folder = static_cast<const ExplorerTree*>(tree_)->drop_folder();
    if (!folder) {
        return std::nullopt;
    }
    return folder->isValid() ? model_->path_of(*folder) : *model_->folder();
}

std::optional<QModelIndex>
ExplorerView::folder_to_drop_into(const std::vector<std::filesystem::path>& sources,
                                  const QModelIndex& under) const {
    if (!model_->folder()) {
        return std::nullopt;
    }
    const QModelIndex folder =
        is_real_row(*model_, under) && model_->is_directory(under) ? under : under.parent();
    const std::filesystem::path directory =
        folder.isValid() ? model_->path_of(folder) : *model_->folder();
    bool moves_something = false;
    for (const std::filesystem::path& source : sources) {
        if (exthost::is_path_within(directory, source)) {
            return std::nullopt;
        }
        moves_something = moves_something || source.parent_path() != directory;
    }
    return moves_something ? std::optional(folder) : std::nullopt;
}

bool ExplorerView::can_move(const std::filesystem::path& source,
                            const std::filesystem::path& directory) const {
    if (source.parent_path() == directory || exthost::is_path_within(directory, source)) {
        return false;
    }
    const std::filesystem::path destination = directory / source.filename();
    return !platform::stat_path(destination) ||
           (!exthost::is_path_within(source, destination) &&
            !(unsaved_changes_ && unsaved_changes_(destination)));
}

bool ExplorerView::move_confirmed(const std::vector<std::filesystem::path>& sources,
                                  const std::filesystem::path& directory) {
    QStringList names;
    for (const std::filesystem::path& source : sources) {
        if (can_move(source, directory)) {
            names.push_back(QString::fromStdString(file_name_of(source)));
        }
    }
    if (names.isEmpty()) {
        return true;
    }
    const QString folder = directory == *model_->folder()
                               ? folder_name()
                               : QString::fromStdString(file_name_of(directory));
    QString message;
    QString detail;
    if (names.size() == 1) {
        message = tr("Are you sure you want to move '%1' into '%2'?").arg(names.front(), folder);
    } else {
        message = tr("Are you sure you want to move the following %1 files into '%2'?")
                      .arg(QString::number(names.size()), folder);
        // As many names as VS Code lists.
        constexpr qsizetype listed = 10;
        detail = names.mid(0, listed).join(QLatin1Char('\n'));
        if (const qsizetype more = names.size() - listed; more > 0) {
            detail +=
                QLatin1Char('\n') + (more == 1 ? tr("...1 additional file not shown")
                                               : tr("...%1 additional files not shown").arg(more));
        }
    }
    const MoveAnswer answer = actions_.confirm_move(this, message, detail);
    if (answer.move && answer.do_not_ask_again) {
        emit move_confirmation_turned_off();
    }
    return answer.move;
}

void ExplorerView::drop_items(const std::vector<std::filesystem::path>& sources,
                              const std::filesystem::path& directory, bool copy) {
    finish_editing();
    if (!copy && confirm_drag_and_drop_ && actions_.confirm_move &&
        !move_confirmed(sources, directory)) {
        return;
    }
    std::vector<std::filesystem::path> written;
    QString failure;
    const auto fail = [&failure](const QString& message) {
        if (failure.isEmpty()) {
            failure = message;
        }
    };
    for (const std::filesystem::path& source : sources) {
        if (source.parent_path() == directory || exthost::is_path_within(directory, source)) {
            continue;
        }
        const std::filesystem::path destination = directory / source.filename();
        const QString name = QString::fromStdString(file_name_of(source));
        const bool replaces = platform::stat_path(destination).has_value();
        if (replaces) {
            // Replacing removes the target first, which would take the source with it.
            if (exthost::is_path_within(source, destination)) {
                fail(tr("%1 cannot replace a folder that holds it").arg(name));
                continue;
            }
            const auto unsaved = [&] {
                return !copy && unsaved_changes_ && unsaved_changes_(destination);
            };
            const QString kept = tr("%1 has unsaved changes and is not replaced").arg(name);
            if (unsaved()) {
                fail(kept);
                continue;
            }
            if (actions_.confirm &&
                !actions_.confirm(this,
                                  tr("A file or folder with the name '%1' already exists in the "
                                     "destination folder. Do you want to replace it?")
                                      .arg(name),
                                  tr("This action is irreversible!"), tr("Replace"))) {
                continue;
            }
            // The question runs a nested event loop, in which a document can be changed.
            if (unsaved()) {
                fail(kept);
                continue;
            }
        }
        const std::expected<void, Error> done =
            copy ? platform::copy_path(source, destination, replaces)
                 : platform::rename_path(source, destination, replaces);
        if (!done) {
            fail(tr("%1 failed: %2")
                     .arg(copy ? tr("Copying %1").arg(name) : tr("Moving %1").arg(name),
                          QString::fromStdString(done.error().message)));
            continue;
        }
        written.push_back(destination);
        // The documents of the replaced file go before those of the moved one take its place.
        if (replaces) {
            emit deleted({destination});
        }
        if (!copy) {
            emit renamed(QString::fromStdString(source.string()),
                         QString::fromStdString(destination.string()));
        }
    }
    for (const std::filesystem::path& source : sources) {
        model_->reload(source.parent_path());
    }
    model_->reload(directory);
    QItemSelection selection;
    QModelIndex first;
    for (const std::filesystem::path& path : written) {
        if (const QModelIndex row = model_->index_of(path); row.isValid()) {
            selection.select(row, row);
            first = first.isValid() ? first : row;
        }
    }
    if (first.isValid()) {
        tree_->selectionModel()->setCurrentIndex(first, QItemSelectionModel::NoUpdate);
        tree_->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect);
        tree_->scrollTo(first);
    }
    if (!failure.isEmpty()) {
        emit operation_failed(failure);
    }
}

bool ExplorerView::is_editing() const {
    return editing_.has_value() && editor_ != nullptr;
}

QLineEdit* ExplorerView::name_editor() const {
    return editor_;
}

QString ExplorerView::name_problem() const {
    return editor_ ? editor_->problem() : QString();
}

void ExplorerView::new_file() {
    begin_new(false);
}

void ExplorerView::new_folder() {
    begin_new(true);
}

void ExplorerView::begin_new(bool folder) {
    if (!shows_folder()) {
        return;
    }
    finish_editing();
    QModelIndex directory;
    if (const QModelIndex row = focused_row(); row.isValid()) {
        directory = model_->is_directory(row) ? row : row.parent();
    }
    new_request_ = NewRequest{folder, !directory.isValid(), QPersistentModelIndex(directory)};
    if (directory.isValid()) {
        tree_->expand(directory);
    }
    continue_new();
}

void ExplorerView::continue_new() {
    if (!new_request_) {
        return;
    }
    const QModelIndex directory = new_request_->directory;
    if (!new_request_->root && !directory.isValid()) {
        new_request_.reset();
        return;
    }
    if (!model_->is_read(directory)) {
        return;
    }
    const bool folder = new_request_->folder;
    new_request_.reset();
    const QModelIndex row = model_->add_pending_entry(directory, folder);
    begin_editing(folder ? Edit::create_folder : Edit::create_file, row,
                  directory.isValid() ? model_->path_of(directory) : *model_->folder(), {});
}

void ExplorerView::rename_selected() {
    const QModelIndex row = focused_row();
    if (!row.isValid()) {
        return;
    }
    finish_editing();
    const std::filesystem::path path = model_->path_of(row);
    begin_editing(Edit::rename, row, path.parent_path(), path);
}

void ExplorerView::begin_editing(Edit kind, const QModelIndex& row, std::filesystem::path directory,
                                 std::filesystem::path original) {
    editing_ = Editing{kind, std::move(directory), std::move(original), QPersistentModelIndex(row)};
    const std::uint64_t serial = ++edit_serial_;
    tree_->setCurrentIndex(row);
    tree_->scrollTo(row);
    tree_->edit(row);
    if (editor_ == nullptr) {
        editing_.reset();
        model_->remove_pending_entry();
        return;
    }
    // The tree selects all of the text when it opens the field.
    if (kind == Edit::rename) {
        const QString name = editor_->text();
        const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
        if (!model_->is_directory(row) && dot > 0) {
            editor_->setSelection(0, static_cast<int>(dot));
        } else {
            editor_->selectAll();
        }
    }
    // The tree deletes the field when its row goes, which ends the editing.
    connect(editor_, &QObject::destroyed, this, [this, serial, alive = alive_] {
        if (*alive && serial == edit_serial_) {
            editing_.reset();
            model_->remove_pending_entry();
        }
    });
}

QWidget* ExplorerView::make_name_editor(QWidget* parent, const QModelIndex&) {
    auto* editor = new NameEditor(colors_, parent);
    editor->set_scaled_font(font());
    if (editing_ && editing_->kind == Edit::rename) {
        editor->setText(QString::fromStdString(file_name_of(editing_->original)));
    }
    editor->on_accept = [this](const QString& name) { accept_name(name); };
    editor->on_cancel = [this] { finish_editing(); };
    editor->on_blur = [this] {
        if (!editing_) {
            return;
        }
        if (editor_ && !editor_->text().isEmpty() && problem_with(editor_->text()).isEmpty()) {
            accept_name(editor_->text());
        } else {
            finish_editing();
        }
    };
    connect(editor, &QLineEdit::textChanged, this, [this](const QString& text) {
        if (editor_) {
            editor_->set_problem(text.isEmpty() ? QString() : problem_with(text));
        }
    });
    editor_ = editor;
    return editor;
}

QString ExplorerView::problem_with(const QString& name) const {
    if (!editing_) {
        return {};
    }
    if (name.trimmed().isEmpty()) {
        return tr("A file or folder name must be provided.");
    }
    if (name.startsWith(QLatin1Char('/')) || name.startsWith(QLatin1Char('\\'))) {
        return tr("A file or folder name cannot start with a slash.");
    }
    const bool invalid = name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')) ||
                         name == QLatin1String(".") || name == QLatin1String("..") ||
                         name.contains(QChar(0));
    if (invalid) {
        return tr("The name \"%1\" is not valid as a file or folder name. Please choose a "
                  "different name.")
            .arg(name);
    }
    const std::filesystem::path path = editing_->directory / name.toStdString();
    if (editing_->kind == Edit::rename && path == editing_->original) {
        return {};
    }
    if (platform::stat_path(path)) {
        return tr("A file or folder \"%1\" already exists at this location. Please choose a "
                  "different name.")
            .arg(name);
    }
    return {};
}

void ExplorerView::accept_name(const QString& name) {
    if (!editing_ || !editor_) {
        return;
    }
    const Editing edit = *editing_;
    if (edit.kind == Edit::rename && name.toStdString() == file_name_of(edit.original)) {
        finish_editing();
        return;
    }
    if (const QString problem = problem_with(name); !problem.isEmpty()) {
        editor_->set_problem(problem);
        return;
    }
    const std::filesystem::path target = edit.directory / name.toStdString();
    std::expected<void, Error> done;
    switch (edit.kind) {
    case Edit::create_file:
        done = platform::write_file(target, "");
        break;
    case Edit::create_folder:
        done = platform::make_directory(target);
        break;
    case Edit::rename:
        done = platform::rename_path(edit.original, target, false);
        break;
    }
    finish_editing();
    model_->reload(edit.directory);
    if (!done) {
        emit operation_failed(
            tr("%1 failed: %2")
                .arg(edit.kind == Edit::rename ? tr("Renaming") : tr("Creating %1").arg(name),
                     QString::fromStdString(done.error().message)));
        return;
    }
    select_path(target);
    if (edit.kind == Edit::rename) {
        emit renamed(QString::fromStdString(edit.original.string()),
                     QString::fromStdString(target.string()));
    } else {
        emit created(QString::fromStdString(target.string()), edit.kind == Edit::create_folder);
    }
}

void ExplorerView::finish_editing() {
    editing_.reset();
    ++edit_serial_;
    if (editor_) {
        static_cast<ExplorerTree*>(tree_)->close_name_editor(editor_);
        editor_.clear();
    }
    model_->remove_pending_entry();
}

void ExplorerView::select_path(const std::filesystem::path& path) {
    if (const QModelIndex row = model_->index_of(path); row.isValid()) {
        tree_->setCurrentIndex(row);
        tree_->scrollTo(row);
    }
}

void ExplorerView::trash_selected() {
    delete_items(false);
}

void ExplorerView::delete_selected() {
    delete_items(true);
}

void ExplorerView::delete_items(bool permanently) {
    // What to delete is fixed now: the question below runs a nested event loop, in which the
    // selection can change.
    const std::vector<std::filesystem::path> paths = selected_paths();
    if (paths.empty()) {
        return;
    }
    const bool trash = !permanently && trash_enabled_;
    if (confirm_delete_ && actions_.confirm) {
        const auto is_folder = [](const std::filesystem::path& path) {
            const auto stat = platform::stat_path(path);
            return stat && (stat->type & platform::file_type::directory) &&
                   !(stat->type & platform::file_type::symbolic_link);
        };
        const QString name = QString::fromStdString(file_name_of(paths.front()));
        QString message;
        if (paths.size() > 1) {
            message = (trash ? tr("Are you sure you want to delete the following %1 "
                                  "files/directories and their contents?")
                             : tr("Are you sure you want to permanently delete the following %1 "
                                  "files/directories and their contents?"))
                          .arg(paths.size());
        } else if (is_folder(paths.front())) {
            message = (trash ? tr("Are you sure you want to delete '%1' and its contents?")
                             : tr("Are you sure you want to permanently delete '%1' and its "
                                  "contents?"))
                          .arg(name);
        } else {
            message = (trash ? tr("Are you sure you want to delete '%1'?")
                             : tr("Are you sure you want to permanently delete '%1'?"))
                          .arg(name);
        }
        QString detail;
        if (trash) {
#if defined(Q_OS_WIN)
            detail = paths.size() > 1 ? tr("You can restore these files from the Recycle Bin.")
                                      : tr("You can restore this file from the Recycle Bin.");
#else
            detail = paths.size() > 1 ? tr("You can restore these files from the Trash.")
                                      : tr("You can restore this file from the Trash.");
#endif
        } else {
            detail = tr("This action is irreversible!");
        }
        const QString button =
#if defined(Q_OS_WIN)
            trash ? tr("Move to Recycle Bin") : tr("Delete");
#else
            trash ? tr("Move to Trash") : tr("Delete");
#endif
        if (!actions_.confirm(this, message, detail, button)) {
            return;
        }
    }

    std::vector<std::filesystem::path> removed;
    QString failure;
    for (const std::filesystem::path& path : paths) {
        QString reason;
        if (trash && actions_.move_to_trash) {
            if (!actions_.move_to_trash(path)) {
                reason = tr("the item cannot be moved to the trash");
            }
        } else if (auto done = platform::remove_path(path, true); !done) {
            reason = QString::fromStdString(done.error().message);
        }
        if (reason.isEmpty()) {
            removed.push_back(path);
        } else if (failure.isEmpty()) {
            failure = tr("Deleting %1 failed: %2")
                          .arg(QString::fromStdString(file_name_of(path)), reason);
        }
    }
    std::set<std::filesystem::path> parents;
    for (const std::filesystem::path& path : paths) {
        parents.insert(path.parent_path());
    }
    for (const std::filesystem::path& parent : parents) {
        model_->reload(parent);
    }
    if (!removed.empty()) {
        emit deleted(removed);
    }
    if (!failure.isEmpty()) {
        emit operation_failed(failure);
    }
}

bool ExplorerView::shows_folder() const {
    return pages_->currentIndex() == 1;
}

QString ExplorerView::folder_name() const {
    return header_->name();
}

} // namespace anvil::ui
