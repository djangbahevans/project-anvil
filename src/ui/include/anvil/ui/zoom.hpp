#pragma once

#include <QObject>

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

} // namespace anvil::ui
