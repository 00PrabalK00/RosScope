#include <QApplication>

#include "rrcc/main_window.hpp"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("Roscoppe");
    app.setOrganizationName("Prabal Khare");

    rrcc::MainWindow window;
    window.show();

    return QApplication::exec();
}
