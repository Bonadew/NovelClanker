// End-to-end test of the synopsis and plan views, driven programmatically against a fake LM Studio server.
// Run with QT_QPA_PLATFORM=minimal (no window appears). Usage: mainwindow_test <scratch-dir>

#include "AiSettings.h"
#include "FakeLmServer.h"
#include "MainWindow.h"
#include "NovelStore.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QTabWidget>

#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

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
    QTabWidget *tabs;
    QPlainTextEdit *synopsis;
    QPushButton *generate;
    QLabel *synopsisStatus;
    QPlainTextEdit *plan;
    QLabel *planStatus;
    QPushButton *stop;
    int planTab;
};

Ui ui(MainWindow &w)
{
    Ui u{w.findChild<QTabWidget *>("tabs"),          w.findChild<QPlainTextEdit *>("synopsisEdit"),
         w.findChild<QPushButton *>("generatePlanButton"), w.findChild<QLabel *>("synopsisStatus"),
         w.findChild<QPlainTextEdit *>("planEdit"),   w.findChild<QLabel *>("planStatus"),
         w.findChild<QPushButton *>("stopPlanButton"), -1};
    if (u.tabs) {
        u.planTab = u.tabs->indexOf(u.plan->parentWidget());
    }
    return u;
}

QString toQ(const fs::path &p)
{
    return QString::fromStdU16String(p.u16string());
}

// Synopsis with whitespace that must survive exactly: leading/trailing spaces, tabs, a non-breaking space,
// blank lines, emoji.
const QString kSynopsis = QString::fromUtf8("  Mira, a lighthouse keeper,\tfinds a\xC2\xA0letter.\n\n\n"
                                            "She sails north \xE2\x9B\xB5 to return it.   ");
const QString kPlan = "# The Letter\n\n## Premise\nMira returns a letter.\n\n## Structure\n\n### Act 1: Home\n";

void testGenerateEditAndSave(const fs::path &dir)
{
    FakeServer server;
    server.routes["/v1/chat/completions"] = {
        200, sseStream({"<think>outline</think>", "# The Letter\n\n", "## Premise\nMira returns a letter.\n\n",
                        "## Structure\n\n### Act 1: Home\n"}),
        "text/event-stream"};
    AiSettings settings;
    settings.serverUrl = server.url().toString();
    settings.plannerModel = "planner-x";
    settings.save();

    {
        MainWindow w;
        EXPECT(w.openNovel(toQ(dir), NovelStore::Mode::Create));
        const Ui u = ui(w);
        EXPECT(u.tabs && u.synopsis && u.generate && u.plan && u.planStatus && u.stop);
        EXPECT(u.tabs->count() == 3);
        EXPECT(u.tabs->tabText(1) == "Synopsis" && u.tabs->tabText(2) == "Plan");
        EXPECT(!u.tabs->isTabVisible(u.planTab)); // no plan yet
        EXPECT(u.synopsisStatus->text().contains("planner-x"));

        u.synopsis->setPlainText(kSynopsis);
        u.generate->click();
        EXPECT(waitFor([&] { return u.planStatus->text().startsWith("Plan generated"); }));

        // The plan opens in its own view, editable, without the reasoning.
        EXPECT(u.tabs->isTabVisible(u.planTab) && u.tabs->currentIndex() == u.planTab);
        EXPECT(u.plan->toPlainText() == kPlan.trimmed());
        EXPECT(!u.plan->isReadOnly() && u.stop->isHidden());
        EXPECT(u.generate->isEnabled());

        // The request used the planner model and contained the synopsis verbatim.
        const QJsonObject request = QJsonDocument::fromJson(server.lastRequestBody).object();
        EXPECT(request.value("model").toString() == "planner-x");
        EXPECT(request.value("messages").toArray().at(1).toObject().value("content").toString().contains(kSynopsis));

        // Edit the plan; closing the window saves it.
        u.plan->moveCursor(QTextCursor::End);
        u.plan->insertPlainText("\n#### Chapter 1: The letter\n");
        w.close();
    }

    NovelStore store(dir, NovelStore::Mode::Open);
    EXPECT(QString::fromStdString(store.synopsis()) == kSynopsis);
    EXPECT(QString::fromStdString(store.plan()) == kPlan.trimmed() + "\n#### Chapter 1: The letter\n");
}

void testReopenShowsSynopsisAndPlan(const fs::path &dir)
{
    MainWindow w;
    EXPECT(w.openNovel(toQ(dir), NovelStore::Mode::Open));
    const Ui u = ui(w);
    EXPECT(u.synopsis->toPlainText().replace(QChar::Nbsp, ' ') == QString(kSynopsis).replace(QChar::Nbsp, ' '));
    EXPECT(u.tabs->isTabVisible(u.planTab));
    EXPECT(u.plan->toPlainText().startsWith("# The Letter"));
}

void testStop(const fs::path &dir)
{
    FakeServer server; // a stream that never finishes
    server.routes["/v1/chat/completions"] = {200, sseStream({"# Partial plan\n", "## Premise\n"}, "", false),
                                             "text/event-stream", true};
    AiSettings settings;
    settings.serverUrl = server.url().toString();
    settings.plannerModel = "planner-x";
    settings.save();

    {
        MainWindow w;
        EXPECT(w.openNovel(toQ(dir), NovelStore::Mode::Create));
        const Ui u = ui(w);
        u.synopsis->setPlainText("A synopsis.");
        u.generate->click();
        EXPECT(waitFor([&] { return u.plan->toPlainText().contains("## Premise"); }));
        EXPECT(u.plan->isReadOnly() && !u.stop->isHidden() && !u.generate->isEnabled());

        u.stop->click();
        EXPECT(!u.plan->isReadOnly() && u.stop->isHidden() && u.generate->isEnabled());
        EXPECT(u.planStatus->text().contains("incomplete"));
        EXPECT(u.plan->toPlainText() == "# Partial plan\n## Premise\n");
        w.close();
    }
    NovelStore store(dir, NovelStore::Mode::Open);
    EXPECT(store.plan() == "# Partial plan\n## Premise\n");
    EXPECT(store.synopsis() == "A synopsis.");
}

void testReasoningUsedWholeLimit(const fs::path &dir)
{
    FakeServer server; // a reasoning model that hits its length limit before writing anything
    server.routes["/v1/chat/completions"] = {200, sseStream({}, "length", true, {"Let me think about acts..."}),
                                             "text/event-stream"};
    AiSettings settings;
    settings.serverUrl = server.url().toString();
    settings.plannerModel = "planner-x";
    settings.save();

    {
        MainWindow w;
        EXPECT(w.openNovel(toQ(dir), NovelStore::Mode::Create));
        const Ui u = ui(w);
        u.synopsis->setPlainText("A synopsis.");
        u.generate->click();
        EXPECT(waitFor([&] { return u.planStatus->text().contains("length limit before writing"); }));
        EXPECT(u.plan->toPlainText().isEmpty() && !u.plan->isReadOnly() && u.generate->isEnabled());
        w.close();
    }
    NovelStore store(dir, NovelStore::Mode::Open);
    EXPECT(store.plan().empty());
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName("NovelWriterTests");
    QApplication::setApplicationName("MainWindowTest");
    QSettings().clear();
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <scratch-dir>\n", argv[0]);
        return 2;
    }
    const fs::path root = argv[1];
    fs::remove_all(root);
    fs::create_directories(root);

    testGenerateEditAndSave(root / "generate.novel");
    testReopenShowsSynopsisAndPlan(root / "generate.novel");
    testStop(root / "stop.novel");
    testReasoningUsedWholeLimit(root / "reasoning.novel");

    QSettings().clear();
    if (g_failures == 0) {
        std::puts("All MainWindow tests passed.");
    }
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
