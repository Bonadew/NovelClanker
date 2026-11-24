#include "MainWindow.h"

#include <QApplication>
#include <QFileInfo>
#include <QSettings>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("NovelWriter");
    QApplication::setOrganizationName("NovelWriter");

    MainWindow window;
    window.resize(1100, 750);
    window.show();

    // Open the novel given on the command line, or else the one used last time.
    const QStringList args = QApplication::arguments();
    const QString novel = args.size() > 1 ? args.at(1) : QSettings().value("lastNovel").toString();
    if (!novel.isEmpty() && QFileInfo::exists(novel)) {
        window.openNovel(novel, NovelStore::Mode::Open);
    }

    return QApplication::exec();
}
