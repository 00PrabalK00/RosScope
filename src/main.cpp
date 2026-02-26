#include <QApplication>
#include <QDir>

#include "rrcc/main_window.hpp"
#include "rrcc/telemetry.hpp"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("RosScope");
    app.setOrganizationName("Prabal Khare");
    QObject::connect(&app, &QCoreApplication::aboutToQuit, []() {
        const QString path = QDir(QDir::currentPath()).filePath("logs/telemetry_last_exit.json");
        rrcc::Telemetry::instance().exportToFile(path);
    });

    rrcc::MainWindow window;
    window.show();

    return QApplication::exec();
}
