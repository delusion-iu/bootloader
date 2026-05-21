#include "mainwindow.h"

#include <QApplication>
#include <QIcon>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    a.setApplicationName("OTA升级控制台");
    a.setApplicationDisplayName("OTA升级控制台");
    a.setWindowIcon(QIcon(":/src/1.png"));
    MainWindow w;
    w.show();
    return a.exec();
}
