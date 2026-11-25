#include "MainWindow.h"
#include "Theme.h"

#include <QApplication>
#include <QFileInfo>
#include <QSettings>

int main(int argc, char *argv[])
{
#if defined(Q_OS_LINUX)
    // Qt's fontconfig is built into the app (Conan) and would look for its configuration in the build machine's
    // package folder, find none, and then no fonts. Use the system's configuration, unless the user chose one.
    if (qEnvironmentVariableIsEmpty("FONTCONFIG_FILE") && QFileInfo::exists("/etc/fonts/fonts.conf")) {
        qputenv("FONTCONFIG_FILE", "/etc/fonts/fonts.conf");
    }
#endif
    QApplication app(argc, argv);
    QApplication::setApplicationName("NovelWriter");
    QApplication::setOrganizationName("NovelWriter");
    Theme::apply(Theme::load()); // light, dark or the system's, as chosen in View > Theme

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
