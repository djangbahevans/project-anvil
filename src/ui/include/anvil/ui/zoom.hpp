#pragma once

#include <QObject>

class QAbstractScrollArea;

namespace anvil::ui {

// The zoom of the window: every size of a part that is in pixels goes through `scaled`, so that
// one level multiplies them all. The level is the setting `window.zoomLevel`, which the window
// passes on when the settings change. A part that has laid itself out from `scaled` does it again
// when `changed` is emitted.
class Zoom : public QObject {
    Q_OBJECT

public:
    explicit Zoom(QObject* parent = nullptr);

    double level() const {
        return level_;
    }
    double factor() const {
        return factor_;
    }
    // `size` times the factor, rounded, and at least 1 for a size above 0.
    int scaled(int size) const;

    // Takes the level, clamped to the range of levels, and emits `changed` when it differs from
    // the one before.
    void set_level(double level);

signals:
    void changed();

private:
    double level_ = 0;
    double factor_ = 1;
};

// Gives the scroll bars of `area` the width that the style gives them at zoom level 0 times the
// factor of `level`.
void scale_scroll_bars(QAbstractScrollArea& area, double level);

// A size read through the zoom that a part was given, or as it is for a part that has none.
inline int scaled_by(const Zoom* zoom, int size) {
    return zoom ? zoom->scaled(size) : size;
}

} // namespace anvil::ui
