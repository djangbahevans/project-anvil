#pragma once

#include <QMainWindow>

namespace anvil::ui {

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
};

} // namespace anvil::ui
