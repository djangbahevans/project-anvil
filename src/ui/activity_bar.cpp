#include "anvil/ui/activity_bar.hpp"

#include <QPainter>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

namespace anvil::ui {

namespace {

constexpr int indicator_width = 2;

} // namespace

class ActivityBar::Button : public QToolButton {
public:
    Button(const ActivityBar& bar, Entry entry, QWidget* parent)
        : QToolButton(parent), bar_(bar), colors_(bar.colors_), entry_(std::move(entry)) {
        setCheckable(true);
        setAutoRaise(true);
        setFocusPolicy(Qt::TabFocus);
        setText(entry_.title);
        setToolTip(entry_.title);
        setAccessibleName(entry_.title);
        apply_zoom();
    }

    void apply_zoom() {
        setFixedSize(bar_.zoomed(button_size), bar_.zoomed(button_size));
        update();
    }

    const std::string& id() const {
        return entry_.id;
    }
    void set_side(Side side) {
        side_ = side;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        const bool active = isChecked();
        if (active) {
            const int indicator = bar_.zoomed(indicator_width);
            painter.fillRect(
                QRect(side_ == Side::left ? 0 : width() - indicator, 0, indicator, height()),
                colors_.color("activityBar.activeBorder"));
        }
        const QColor color =
            colors_.color(active || underMouse() || hasFocus() ? "activityBar.foreground"
                                                               : "activityBar.inactiveForeground");

        // The icon is a mask: only its shape is used, in the color of the state.
        QPixmap icon = entry_.icon.pixmap(QSize(bar_.zoomed(icon_size), bar_.zoomed(icon_size)),
                                          devicePixelRatioF());
        QPainter tint(&icon);
        tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
        tint.fillRect(icon.rect(), color);
        tint.end();
        const QSizeF logical = icon.deviceIndependentSize();
        painter.drawPixmap(
            QPointF((width() - logical.width()) / 2, (height() - logical.height()) / 2), icon);
    }

private:
    const ActivityBar& bar_;
    const WorkbenchColors& colors_;
    Entry entry_;
    Side side_ = Side::left;
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

void ActivityBar::set_zoom(const Zoom* zoom) {
    if (zoom_) {
        disconnect(zoom_, &Zoom::changed, this, nullptr);
    }
    zoom_ = zoom;
    if (zoom_) {
        connect(zoom_, &Zoom::changed, this, &ActivityBar::apply_zoom);
    }
    apply_zoom();
}

void ActivityBar::apply_zoom() {
    for (Button* button : buttons_) {
        button->apply_zoom();
    }
}

void ActivityBar::set_entries(const std::vector<Entry>& entries) {
    for (Button* button : buttons_) {
        layout_->removeWidget(button);
        delete button;
    }
    buttons_.clear();
    for (const Entry& entry : entries) {
        auto* button = new Button(*this, entry, this);
        button->set_side(side_);
        layout_->insertWidget(static_cast<int>(buttons_.size()), button);
        buttons_.push_back(button);
        connect(button, &QToolButton::clicked, this,
                [this, id = entry.id] { emit activated(QString::fromStdString(id)); });
    }
}

void ActivityBar::set_side(Side side) {
    side_ = side;
    for (Button* button : buttons_) {
        button->set_side(side);
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
