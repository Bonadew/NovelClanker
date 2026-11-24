#include "MainWindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("NovelWriter");
    QApplication::setOrganizationName("NovelWriter");

    MainWindow window;
    window.resize(1000, 700);
    window.show();

    return QApplication::exec();
}
