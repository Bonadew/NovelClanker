// Tests for the settings view, driven programmatically (run with QT_QPA_PLATFORM=minimal: no window appears).
//
// Optional live check: set NOVELCLANKER_LIVE_LMSTUDIO=http://127.0.0.1:1234 to also open the dialog against a
// real LM Studio server and print what it lists.

#include "AiSettings.h"
#include "FakeLmServer.h"
#include "SettingsDialog.h"

#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>

#include <cstdio>
#include <cstdlib>

namespace {

int g_failures = 0;

void expect(bool ok, const char *what, int line)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL (line %d): %s\n", line, what);
        ++g_failures;
    }
}

#define EXPECT(cond) expect((cond), #cond, __LINE__)

// Processes events until `done()` is true or the timeout expires.
template <typename F> bool waitFor(F done, int timeoutMs = 10000)
{
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeoutMs) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    return done();
}

struct Ui {
    QLineEdit *server;
    QLabel *status;
    QComboBox *planner;
    QComboBox *prose;
};

Ui ui(SettingsDialog &dialog)
{
    return {dialog.findChild<QLineEdit *>("serverEdit"), dialog.findChild<QLabel *>("statusLabel"),
            dialog.findChild<QComboBox *>("plannerCombo"), dialog.findChild<QComboBox *>("proseCombo")};
}

QStringList comboIds(const QComboBox *combo)
{
    QStringList ids;
    for (int i = 0; i < combo->count(); ++i) {
        ids << combo->itemData(i).toString();
    }
    return ids;
}

// @relation(FR-SET-1, FR-SET-4, scope=function)
void testDefaults()
{
    QSettings().clear();
    const AiSettings settings = AiSettings::load();
    EXPECT(settings.serverUrl == "http://127.0.0.1:1234");
    EXPECT(settings.plannerModel.isEmpty() && settings.proseModel.isEmpty());
    EXPECT(settings.reasoning == Reasoning::Default);
}

// The reasoning choice is shown, saved and shown again.
// @relation(AI-14, scope=function)
void testReasoningSetting()
{
    QSettings().clear();
    FakeServer server; // the dialog lists the server's models: never ask a real LM Studio from a test
    server.routes["/api/v1/models"] = {200, kSampleNativeModels};
    AiSettings saved;
    saved.serverUrl = server.url().toString();
    saved.save();
    {
        SettingsDialog dialog;
        auto *combo = dialog.findChild<QComboBox *>("reasoningCombo");
        EXPECT(combo && combo->count() == 3 && combo->currentText() == "Model default");
        combo->setCurrentIndex(combo->findData(static_cast<int>(Reasoning::Off)));
        dialog.accept();
    }
    EXPECT(AiSettings::load().reasoning == Reasoning::Off);
    EXPECT(QSettings().value("lmStudio/reasoning").toString() == "off");
    {
        SettingsDialog dialog;
        auto *combo = dialog.findChild<QComboBox *>("reasoningCombo");
        EXPECT(combo->currentText() == "Off where possible");
        combo->setCurrentIndex(combo->findData(static_cast<int>(Reasoning::Low)));
        dialog.accept();
    }
    EXPECT(AiSettings::load().reasoning == Reasoning::Low);
    QSettings().clear();
}

// @relation(FR-SET-2, FR-SET-3, FR-SET-4, scope=function)
void testFetchSelectAndSave()
{
    FakeServer server;
    server.routes["/api/v1/models"] = {200, kSampleNativeModels};

    AiSettings saved;
    saved.serverUrl = server.url().toString();
    saved.plannerModel = "removed-model"; // no longer on the server
    saved.save();

    SettingsDialog dialog;
    const Ui w = ui(dialog);
    EXPECT(w.server && w.status && w.planner && w.prose);
    EXPECT(w.server->text() == saved.serverUrl);
    EXPECT(waitFor([&] { return w.status->text().startsWith("Connected"); }));
    EXPECT(w.status->text().contains("2 model(s) available") && w.status->text().contains("1 loaded"));

    // Both dropdowns list the same LLMs (no embedding model); the planner keeps its missing saved model.
    EXPECT((comboIds(w.prose) == QStringList{"alpha-27b", "zeta-7b"}));
    EXPECT((comboIds(w.planner) == QStringList{"removed-model", "alpha-27b", "zeta-7b"}));
    EXPECT(w.planner->currentData().toString() == "removed-model");
    EXPECT(w.planner->currentText().contains("not on this server"));
    EXPECT(w.prose->currentIndex() == -1); // nothing chosen yet
    EXPECT(w.prose->itemText(0).contains("IQ2_S") && w.prose->itemText(0).contains("loaded"));

    w.planner->setCurrentIndex(w.planner->findData("zeta-7b"));
    w.prose->setCurrentIndex(w.prose->findData("alpha-27b"));
    w.server->setText("  " + server.url().toString() + "/v1/  "); // gets normalized on save
    dialog.accept();

    const AiSettings after = AiSettings::load();
    EXPECT(after.serverUrl == server.url().toString());
    EXPECT(after.plannerModel == "zeta-7b");
    EXPECT(after.proseModel == "alpha-27b");
}

// @relation(FR-SET-3, scope=function)
void testUnreachableKeepsSelection()
{
    QTcpServer probe; // a port with nothing listening
    probe.listen(QHostAddress::LocalHost);
    const quint16 port = probe.serverPort();
    probe.close();

    AiSettings saved;
    saved.serverUrl = QStringLiteral("http://127.0.0.1:%1").arg(port);
    saved.plannerModel = "planner-x";
    saved.proseModel = "prose-y";
    saved.save();

    SettingsDialog dialog;
    const Ui w = ui(dialog);
    EXPECT(waitFor([&] { return w.status->text().startsWith("Could not get models"); }));
    // Saved choices are still shown and kept when saving.
    EXPECT(w.planner->currentData().toString() == "planner-x");
    EXPECT(w.prose->currentData().toString() == "prose-y");
    dialog.accept();
    EXPECT(AiSettings::load().plannerModel == "planner-x");
}

// @relation(FR-SET-1, scope=function)
void testInvalidAddressNotSaved()
{
    AiSettings saved;
    saved.serverUrl = "http://127.0.0.1:1234";
    saved.save();

    SettingsDialog dialog;
    const Ui w = ui(dialog);
    w.server->setText("ftp://nope");
    dialog.findChild<QPushButton *>("refreshButton")->click();
    EXPECT(w.status->text().contains("not a valid server address"));
    // accept() refuses (shows a warning box); don't block on it in a test: just check nothing was saved.
    EXPECT(AiSettings::load().serverUrl == "http://127.0.0.1:1234");
}

void liveCheck(const QString &url)
{
    AiSettings saved;
    saved.serverUrl = url;
    saved.save();
    SettingsDialog dialog;
    const Ui w = ui(dialog);
    waitFor([&] { return !w.status->text().startsWith("Connecting"); }, 15000);
    std::printf("Live LM Studio (%s): %s\n", qPrintable(url), qPrintable(w.status->text()));
    for (int i = 0; i < w.prose->count() && i < 5; ++i) {
        std::printf("  %s  [%s]\n", qPrintable(w.prose->itemText(i)), qPrintable(w.prose->itemData(i).toString()));
    }
    if (w.prose->count() > 5) {
        std::printf("  ... %d more\n", w.prose->count() - 5);
    }
    EXPECT(w.status->text().startsWith("Connected"));
    EXPECT(w.planner->count() == w.prose->count());
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    // Keep test settings away from the real application's settings.
    QApplication::setOrganizationName("NovelClankerTests");
    QApplication::setApplicationName("SettingsDialogTest");

    testDefaults();
    testReasoningSetting();
    testFetchSelectAndSave();
    testUnreachableKeepsSelection();
    testInvalidAddressNotSaved();
    if (const QByteArray live = qgetenv("NOVELCLANKER_LIVE_LMSTUDIO"); !live.isEmpty()) {
        liveCheck(QString::fromUtf8(live));
    }
    QSettings().clear();

    if (g_failures == 0) {
        std::puts("All SettingsDialog tests passed.");
    }
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
