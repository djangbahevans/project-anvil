#include "anvil/ui/main_window.hpp"

#include <QApplication>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    anvil::ui::MainWindow window;
    window.show();
    return app.exec();
}
