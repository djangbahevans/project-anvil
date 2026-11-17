#include "anvil/ui/main_window.hpp"

#include "anvil/platform/paths.hpp"

#include <QLabel>
#include <QStatusBar>
#include <QString>

namespace anvil::ui {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("Anvil"));
    resize(1200, 800);

    auto* placeholder = new QLabel(QStringLiteral("Anvil"), this);
    placeholder->setAlignment(Qt::AlignCenter);
    setCentralWidget(placeholder);

    const auto config_dir = platform::user_config_dir();
    statusBar()->showMessage(config_dir ? QString::fromStdString(config_dir->generic_string())
                                        : QString::fromStdString(config_dir.error().message));
}

} // namespace anvil::ui
