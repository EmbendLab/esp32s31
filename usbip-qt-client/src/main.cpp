#include "mainwindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("USB/IP Client"));
    app.setOrganizationName(QStringLiteral("usbip-win2-qt"));

    MainWindow w;
    w.show();
    return app.exec();
}