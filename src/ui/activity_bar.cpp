#include "anvil/ui/activity_bar.hpp"

#include <QPainter>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

namespace anvil::ui {

class ActivityBar::Button : public QToolButton {
public:
    Button(const WorkbenchColors& colors, Entry entry, QWidget* parent)
        : QToolButton(parent), colors_(colors), entry_(std::move(entry)) {
        setCheckable(true);
        setAutoRaise(true);
        setFocusPolicy(Qt::TabFocus);
        setFixedSize(button_size, button_size);
        setText(entry_.title);
        setToolTip(entry_.title);
        setAccessibleName(entry_.title);
    }

    const std::string& id() const {
        return entry_.id;
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        const bool active = isChecked();
        if (active) {
            painter.fillRect(QRect(0, 0, 2, height()), colors_.color("activityBar.activeBorder"));
        }
        const QColor color =
            colors_.color(active || underMouse() || hasFocus() ? "activityBar.foreground"
                                                               : "activityBar.inactiveForeground");

        // The icon is a mask: only its shape is used, in the color of the state.
        QPixmap icon = entry_.icon.pixmap(QSize(icon_size, icon_size), devicePixelRatioF());
        QPainter tint(&icon);
        tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
        tint.fillRect(icon.rect(), color);
        tint.end();
        const QSizeF logical = icon.deviceIndependentSize();
        painter.drawPixmap(
            QPointF((width() - logical.width()) / 2, (height() - logical.height()) / 2), icon);
    }

private:
    const WorkbenchColors& colors_;
    Entry entry_;
};

ActivityBar::ActivityBar(const WorkbenchColors& colors, QWidget* parent)
    : QWidget(parent), colors_(colors) {
    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(0);
    layout_->addStretch(1);
    connect(&colors_, &WorkbenchColors::changed, this, [this] {
        for (Button* button : buttons_) {
            button->update();
        }
    });
}

void ActivityBar::set_entries(const std::vector<Entry>& entries) {
    for (Button* button : buttons_) {
        layout_->removeWidget(button);
        delete button;
    }
    buttons_.clear();
    for (const Entry& entry : entries) {
        auto* button = new Button(colors_, entry, this);
        layout_->insertWidget(static_cast<int>(buttons_.size()), button);
        buttons_.push_back(button);
        connect(button, &QToolButton::clicked, this,
                [this, id = entry.id] { emit activated(QString::fromStdString(id)); });
    }
}

void ActivityBar::set_active(const std::string& id) {
    for (Button* button : buttons_) {
        button->setChecked(!id.empty() && button->id() == id);
    }
}

QToolButton* ActivityBar::button(const std::string& id) const {
    const auto found = std::ranges::find(buttons_, id, &Button::id);
    return found == buttons_.end() ? nullptr : *found;
}

std::vector<std::string> ActivityBar::ids() const {
    std::vector<std::string> result;
    for (const Button* button : buttons_) {
        result.push_back(button->id());
    }
    return result;
}

} // namespace anvil::ui
