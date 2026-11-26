#pragma once

#include "anvil/ui/workbench_colors.hpp"
#include "anvil/ui/zoom.hpp"

#include <QSplitter>
#include <string_view>

namespace anvil::ui {

// A splitter whose handles are VS Code's sashes: between the parts there is only a 1 px line of a
// border color, and the grab area lies over both of them, shown in the `sash.hoverBorder` color
// while it is hovered or dragged. The parts of the window and the groups of the editor area use it
// with their own border key. The line is painted on `editor.background`, so that a translucent
// border color blends with something opaque.
class Splitter : public QSplitter {
    Q_OBJECT

public:
    // The room that a sash takes between its parts, at every zoom.
    static constexpr int line_width = 1;
    // The least width of the grab area at zoom level 0.
    static constexpr int sash_width = 4;

    // The colors must outlive the splitter, and the key of the border must outlive it too.
    Splitter(Qt::Orientation orientation, const WorkbenchColors& colors, std::string_view border,
             QWidget* parent = nullptr);

    // The zoom that the width of the grab areas is read through, which must outlive the splitter.
    // Null means level 0.
    void set_zoom(const Zoom* zoom);

protected:
    QSplitterHandle* createHandle() override;

private:
    // How far a grab area reaches to each side of its line.
    int reach() const;

    const WorkbenchColors& colors_;
    std::string_view border_;
    const Zoom* zoom_ = nullptr;
};

} // namespace anvil::ui
