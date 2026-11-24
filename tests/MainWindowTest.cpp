// End-to-end test of the synopsis and plan views, driven programmatically against a fake LM Studio server.
// Run with QT_QPA_PLATFORM=minimal (no window appears). Usage: mainwindow_test <scratch-dir>

#include "AiSettings.h"
#include "FakeLmServer.h"
#include "MainWindow.h"
#include "NovelStore.h"
#include "PlanChat.h"

#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTreeWidget>

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
    QTreeWidget *planList;
    QPushButton *newPlan;
    QLabel *planOrigin;
    QPlainTextEdit *plan;
    QLabel *planStatus;
    QPushButton *stop;
    int planTab = -1;

    // "title|model" for every plan in the list, e.g. "Plan 1|planner-x Plan 2|(by hand)".
    QString planRows() const
    {
        QStringList rows;
        for (int i = 0; i < planList->topLevelItemCount(); ++i) {
            rows << planList->topLevelItem(i)->text(0) + "|" + planList->topLevelItem(i)->text(1);
        }
        return rows.join(' ');
    }
};

Ui ui(MainWindow &w)
{
    Ui u{w.findChild<QTabWidget *>("tabs"),
         w.findChild<QPlainTextEdit *>("synopsisEdit"),
         w.findChild<QPushButton *>("generatePlanButton"),
         w.findChild<QLabel *>("synopsisStatus"),
         w.findChild<QTreeWidget *>("planList"),
         w.findChild<QPushButton *>("newPlanButton"),
         w.findChild<QLabel *>("planOrigin"),
         w.findChild<QPlainTextEdit *>("planEdit"),
         w.findChild<QLabel *>("planStatus"),
         w.findChild<QPushButton *>("stopPlanButton")};
    for (int i = 0; u.tabs && i < u.tabs->count(); ++i) {
        if (u.tabs->tabText(i) == "Plan") {
            u.planTab = i;
        }
    }
    return u;
}

QString toQ(const fs::path &p)
{
    return QString::fromStdU16String(p.u16string());
}

void useServer(const FakeServer &server, const QString &plannerModel)
{
    AiSettings settings;
    settings.serverUrl = server.url().toString();
    settings.plannerModel = plannerModel;
    settings.save();
}

std::string storedPlans(const NovelStore &store)
{
    std::string out;
    for (const auto &plan : store.plans()) {
        out += (out.empty() ? "" : " ") + plan.title + "|" + (plan.model.empty() ? "hand" : plan.model);
    }
    return out;
}

// Synopsis with whitespace that must survive exactly: leading/trailing spaces, tabs, a non-breaking space,
// blank lines, emoji.
const QString kSynopsis = QString::fromUtf8("  Mira, a lighthouse keeper,\tfinds a\xC2\xA0letter.\n\n\n"
                                            "She sails north \xE2\x9B\xB5 to return it.   ");
const QString kPlan = "# The Letter\n\n## Premise\nMira returns a letter.\n\n## Structure\n\n### Act 1: Home";
const QByteArray kPlanStream = sseStream({"<think>outline</think>", "# The Letter\n\n",
                                          "## Premise\nMira returns a letter.\n\n", "## Structure\n\n### Act 1: Home\n"});

void testMultiplePlans(const fs::path &dir)
{
    FakeServer server;
    server.routes["/v1/chat/completions"] = {200, kPlanStream, "text/event-stream"};
    useServer(server, "planner-x");

    {
        MainWindow w;
        EXPECT(w.openNovel(toQ(dir), NovelStore::Mode::Create));
        const Ui u = ui(w);
        EXPECT(u.tabs && u.synopsis && u.generate && u.planList && u.newPlan && u.planOrigin && u.plan && u.stop);
        EXPECT(u.planTab >= 0 && u.tabs->isTabVisible(u.planTab));
        EXPECT(u.planList->topLevelItemCount() == 0 && !u.plan->isEnabled());
        EXPECT(u.synopsisStatus->text().contains("planner-x"));

        // First generation: a new plan that remembers the model, shown in the Plan view without reasoning.
        u.synopsis->setPlainText(kSynopsis);
        u.generate->click();
        EXPECT(waitFor([&] { return u.planStatus->text().startsWith("Done"); }));
        EXPECT(u.tabs->currentIndex() == u.planTab);
        EXPECT(u.planRows() == "Plan 1|planner-x");
        EXPECT(u.planOrigin->text() == "Generated with planner-x.");
        EXPECT(u.plan->toPlainText() == kPlan);
        EXPECT(!u.plan->isReadOnly() && u.stop->isHidden() && u.generate->isEnabled());

        const QJsonObject request = QJsonDocument::fromJson(server.lastRequestBody).object();
        EXPECT(request.value("model").toString() == "planner-x");
        EXPECT(request.value("messages").toArray().at(1).toObject().value("content").toString().contains(kSynopsis));

        // Edit the generated plan by hand: it still counts as generated by planner-x.
        u.plan->moveCursor(QTextCursor::End);
        u.plan->insertPlainText("\n#### Chapter 1: The letter");

        // Second generation with another model: a second plan; the first is kept as it was.
        useServer(server, "planner-y");
        u.generate->click();
        EXPECT(waitFor([&] { return u.planStatus->text().startsWith("Done"); }));
        EXPECT(u.planRows() == "Plan 1|planner-x Plan 2|planner-y");
        EXPECT(u.planList->currentItem() == u.planList->topLevelItem(1));
        EXPECT(u.planOrigin->text() == "Generated with planner-y.");

        // A plan written by hand.
        u.newPlan->click();
        EXPECT(u.planRows() == "Plan 1|planner-x Plan 2|planner-y Plan 3|(by hand)");
        EXPECT(u.planOrigin->text() == "Written by hand." && u.plan->isEnabled() && u.plan->toPlainText().isEmpty());
        u.plan->insertPlainText("# My own plan");

        // Rename it (as the inline editor would), then switch back to the first plan: edits are kept.
        u.planList->topLevelItem(2)->setText(0, "  Mine  ");
        EXPECT(u.planList->topLevelItem(2)->text(0) == "Mine");
        u.planList->setCurrentItem(u.planList->topLevelItem(0));
        EXPECT(u.planOrigin->text() == "Generated with planner-x.");
        EXPECT(u.plan->toPlainText() == kPlan + "\n#### Chapter 1: The letter");
        w.close();
    }

    NovelStore store(dir, NovelStore::Mode::Open);
    EXPECT(QString::fromStdString(store.synopsis()) == kSynopsis);
    EXPECT(storedPlans(store) == "Plan 1|planner-x Plan 2|planner-y Mine|hand");
    EXPECT(QString::fromStdString(store.planText(store.plans()[0].id)) == kPlan + "\n#### Chapter 1: The letter");
    EXPECT(QString::fromStdString(store.planText(store.plans()[1].id)) == kPlan);
    EXPECT(store.planText(store.plans()[2].id) == "# My own plan");
}

void testReopenShowsPlans(const fs::path &dir)
{
    MainWindow w;
    EXPECT(w.openNovel(toQ(dir), NovelStore::Mode::Open));
    const Ui u = ui(w);
    EXPECT(u.synopsis->toPlainText().replace(QChar::Nbsp, ' ') == QString(kSynopsis).replace(QChar::Nbsp, ' '));
    EXPECT(u.planRows() == "Plan 1|planner-x Plan 2|planner-y Mine|(by hand)");
    EXPECT(u.planList->currentItem() == u.planList->topLevelItem(0)); // first plan selected
    EXPECT(u.plan->toPlainText().startsWith("# The Letter"));
    u.planList->setCurrentItem(u.planList->topLevelItem(2));
    EXPECT(u.planOrigin->text() == "Written by hand.");
}

void testStopKeepsPartialPlan(const fs::path &dir)
{
    FakeServer server; // a stream that never finishes
    server.routes["/v1/chat/completions"] = {200, sseStream({"# Partial plan\n", "## Premise\n"}, "", false),
                                             "text/event-stream", true};
    useServer(server, "planner-x");
    {
        MainWindow w;
        EXPECT(w.openNovel(toQ(dir), NovelStore::Mode::Create));
        const Ui u = ui(w);
        u.synopsis->setPlainText("A synopsis.");
        u.generate->click();
        EXPECT(waitFor([&] { return u.plan->toPlainText().contains("## Premise"); }));
        EXPECT(u.plan->isReadOnly() && !u.stop->isHidden() && !u.generate->isEnabled());
        EXPECT(!u.planList->isEnabled() && !u.newPlan->isEnabled()); // stay on the plan being written

        u.stop->click();
        EXPECT(!u.plan->isReadOnly() && u.stop->isHidden() && u.generate->isEnabled() && u.planList->isEnabled());
        EXPECT(u.planStatus->text().contains("incomplete"));
        EXPECT(u.planRows() == "Plan 1|planner-x");
        EXPECT(u.plan->toPlainText() == "# Partial plan\n## Premise\n");
        w.close();
    }
    NovelStore store(dir, NovelStore::Mode::Open);
    EXPECT(storedPlans(store) == "Plan 1|planner-x");
    EXPECT(store.planText(store.plans()[0].id) == "# Partial plan\n## Premise\n");
    EXPECT(store.synopsis() == "A synopsis.");
}

void testEmptyAnswerAddsNoPlan(const fs::path &dir)
{
    FakeServer server; // a reasoning model that hits its length limit before writing anything
    server.routes["/v1/chat/completions"] = {200, sseStream({}, "length", true, {"Let me think about acts..."}),
                                             "text/event-stream"};
    useServer(server, "planner-x");
    {
        MainWindow w;
        EXPECT(w.openNovel(toQ(dir), NovelStore::Mode::Create));
        const Ui u = ui(w);
        u.newPlan->click(); // an existing hand-written plan
        u.plan->insertPlainText("# Keep me");
        u.synopsis->setPlainText("A synopsis.");
        u.generate->click();
        EXPECT(waitFor([&] { return u.planStatus->text().contains("length limit before writing"); }));
        // The empty generated plan is removed again and the previous plan is shown.
        EXPECT(u.planRows() == "Plan 1|(by hand)");
        EXPECT(u.plan->toPlainText() == "# Keep me" && !u.plan->isReadOnly() && u.generate->isEnabled());
        w.close();
    }
    NovelStore store(dir, NovelStore::Mode::Open);
    EXPECT(storedPlans(store) == "Plan 1|hand");
    EXPECT(store.planText(store.plans()[0].id) == "# Keep me");
}

void testPlannerModelOverride(const fs::path &dir)
{
    FakeServer server;
    server.routes["/api/v1/models"] = {200, kSampleNativeModels}; // alpha-27b, zeta-7b (+ an embedding model)
    server.routes["/v1/chat/completions"] = {200, kPlanStream, "text/event-stream"};
    useServer(server, "planner-x");

    {
        MainWindow w;
        EXPECT(w.openNovel(toQ(dir), NovelStore::Mode::Create));
        const Ui u = ui(w);
        auto *combo = w.findChild<QComboBox *>("planModelCombo");
        EXPECT(combo != nullptr);
        EXPECT(waitFor([&] { return combo->count() == 3; })); // the server's models were fetched
        EXPECT(combo->itemText(0) == "Settings default (planner-x)" && combo->currentIndex() == 0);
        EXPECT(combo->findData("alpha-27b") > 0 && combo->findData("zeta-7b") > 0);
        EXPECT(combo->findData("nomic-embed") < 0);
        EXPECT(u.synopsisStatus->text().contains("planner-x (from Settings)"));
        u.synopsis->setPlainText("A synopsis.");

        // Choosing a model here overrides Settings for this novel.
        combo->setCurrentIndex(combo->findData("zeta-7b"));
        EXPECT(u.synopsisStatus->text().contains("generated with zeta-7b"));
        u.generate->click();
        EXPECT(waitFor([&] { return u.planStatus->text().startsWith("Done"); }));
        EXPECT(QJsonDocument::fromJson(server.lastRequestBody).object().value("model").toString() == "zeta-7b");
        EXPECT(u.planRows() == "Plan 1|zeta-7b");

        // Back to the Settings default.
        combo->setCurrentIndex(0);
        u.generate->click();
        EXPECT(waitFor([&] { return u.planStatus->text().startsWith("Done") && u.planRows().contains("Plan 2"); }));
        EXPECT(QJsonDocument::fromJson(server.lastRequestBody).object().value("model").toString() == "planner-x");
        EXPECT(u.planRows() == "Plan 1|zeta-7b Plan 2|planner-x");

        combo->setCurrentIndex(combo->findData("alpha-27b"));
        w.close();
    }
    EXPECT(AiSettings::load().plannerModel == "planner-x"); // Settings untouched
    {
        NovelStore store(dir, NovelStore::Mode::Open);
        EXPECT(store.planModelOverride() == "alpha-27b"); // remembered with the novel
    }
    {
        MainWindow w; // reopening restores the choice
        EXPECT(w.openNovel(toQ(dir), NovelStore::Mode::Open));
        auto *combo = w.findChild<QComboBox *>("planModelCombo");
        EXPECT(combo->currentData().toString() == "alpha-27b"); // before the model list arrives...
        EXPECT(waitFor([&] { return combo->count() == 3; }));
        EXPECT(combo->currentData().toString() == "alpha-27b"); // ...and after
    }
}

void testChatRefinesPlan(const fs::path &dir)
{
    FakeServer server;
    server.routes["/v1/chat/completions"] = {200, kPlanStream, "text/event-stream"};
    useServer(server, "planner-x");
    const auto sendChat = [](MainWindow &w, const QString &text) {
        auto *input = w.findChild<QPlainTextEdit *>("chatInput");
        input->setPlainText(text);
        w.findChild<QPushButton *>("chatSendButton")->click();
    };
    const auto requestJson = [&] { return QJsonDocument::fromJson(server.lastRequestBody).object(); };

    {
        MainWindow w;
        EXPECT(w.openNovel(toQ(dir), NovelStore::Mode::Create));
        const Ui u = ui(w);
        auto *header = w.findChild<QLabel *>("chatHeader");
        auto *transcript = w.findChild<QTextBrowser *>("chatTranscript");
        auto *input = w.findChild<QPlainTextEdit *>("chatInput");
        EXPECT(header && transcript && input);
        EXPECT(!input->isEnabled()); // no plan selected yet

        u.synopsis->setPlainText("Mira returns a letter.");
        u.generate->click();
        EXPECT(waitFor([&] { return u.planStatus->text().startsWith("Done"); }));
        EXPECT(input->isEnabled());
        EXPECT(header->text() == "Chat with planner-x, which generated this plan.");

        // The author edits the plan, then asks for a change: the model sees the edited plan.
        u.plan->moveCursor(QTextCursor::End);
        u.plan->insertPlainText("\n- my note");
        useServer(server, "planner-y"); // Settings changing doesn't matter: the plan's own model is used
        server.routes["/v1/chat/completions"] = {
            200, sseStream({"Added an Act 2.", "\n\n<plan>\n# The Letter\n\n### Act 1: Home\n",
                            "### Act 2: Away\n</plan>"}),
            "text/event-stream"};
        sendChat(w, "Please add a second act.");
        EXPECT(waitFor([&] { return u.plan->toPlainText().contains("Act 2: Away"); }));
        EXPECT(requestJson().value("model").toString() == "planner-x");
        const QJsonArray messages = requestJson().value("messages").toArray();
        EXPECT(messages.size() == 2);
        EXPECT(messages.at(0).toObject().value("content").toString().contains("- my note"));
        EXPECT(messages.at(0).toObject().value("content").toString().contains("Mira returns a letter."));
        EXPECT(messages.at(1).toObject().value("content").toString().startsWith("Please add a second act."));

        EXPECT(u.plan->toPlainText() == "# The Letter\n\n### Act 1: Home\n### Act 2: Away");
        EXPECT(!u.plan->isReadOnly() && u.planList->isEnabled() && input->isEnabled() && input->toPlainText().isEmpty());
        EXPECT(transcript->toPlainText().contains("Please add a second act.") &&
               transcript->toPlainText().contains("Added an Act 2.") &&
               transcript->toPlainText().contains("Plan updated"));

        // The revision is one undoable step.
        u.plan->undo();
        EXPECT(u.plan->toPlainText() == kPlan + "\n- my note");
        u.plan->redo();
        EXPECT(u.plan->toPlainText().contains("Act 2: Away"));

        // A question: answered, plan unchanged, history sent along.
        server.routes["/v1/chat/completions"] = {200, sseStream({"Act 2 is where Mira travels."}),
                                                 "text/event-stream"};
        sendChat(w, "What happens in Act 2?");
        EXPECT(waitFor([&] { return transcript->toPlainText().contains("Act 2 is where Mira travels."); }));
        EXPECT(u.plan->toPlainText().contains("Act 2: Away"));
        const QJsonArray second = requestJson().value("messages").toArray();
        EXPECT(second.size() == 4); // system, earlier question, earlier answer, new question
        EXPECT(second.at(2).toObject().value("content").toString().contains("includes this revision"));
        EXPECT(second.at(0).toObject().value("content").toString().contains("### Act 2: Away")); // current plan

        // A revised plan that gets cut off is not applied.
        server.routes["/v1/chat/completions"] = {200, sseStream({"Rewrote it.\n<plan>\n# Broken"}, "length"),
                                                 "text/event-stream"};
        sendChat(w, "Rewrite everything.");
        EXPECT(waitFor([&] { return transcript->toPlainText().contains("was not applied"); }));
        EXPECT(u.plan->toPlainText().contains("Act 2: Away"));

        // A hand-written plan talks to this novel's planner model.
        u.newPlan->click();
        EXPECT(header->text() == "Chat with planner-y (this novel's planner model).");
        EXPECT(!transcript->toPlainText().contains("Please add")); // each plan has its own conversation
        server.routes["/v1/chat/completions"] = {200, sseStream({"Sure.\n<plan>\n# Started\n</plan>"}),
                                                 "text/event-stream"};
        sendChat(w, "Start a plan for me.");
        EXPECT(waitFor([&] { return u.plan->toPlainText() == "# Started"; }));
        EXPECT(requestJson().value("model").toString() == "planner-y");
        w.close();
    }

    NovelStore store(dir, NovelStore::Mode::Open);
    EXPECT(storedPlans(store) == "Plan 1|planner-x Plan 2|hand"); // chatting doesn't change a plan's origin
    EXPECT(store.planText(store.plans()[0].id) == "# The Letter\n\n### Act 1: Home\n### Act 2: Away");
    EXPECT(store.planText(store.plans()[1].id) == "# Started");
    const QList<PlanChat::Turn> history = PlanChat::parse(QString::fromStdString(store.planChat(store.plans()[0].id)));
    EXPECT(history.size() == 6);
    EXPECT(history.size() == 6 && history[1].planApplied && !history[3].planApplied && !history[5].planApplied);
}

void testChatHistoryIsRestored(const fs::path &dir)
{
    MainWindow w;
    EXPECT(w.openNovel(toQ(dir), NovelStore::Mode::Open));
    auto *transcript = w.findChild<QTextBrowser *>("chatTranscript");
    EXPECT(transcript->toPlainText().contains("Please add a second act.") &&
           transcript->toPlainText().contains("Act 2 is where Mira travels."));
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

    testMultiplePlans(root / "plans.novel");
    testReopenShowsPlans(root / "plans.novel");
    testStopKeepsPartialPlan(root / "stop.novel");
    testEmptyAnswerAddsNoPlan(root / "empty.novel");
    testPlannerModelOverride(root / "override.novel");
    testChatRefinesPlan(root / "chat.novel");
    testChatHistoryIsRestored(root / "chat.novel");

    QSettings().clear();
    if (g_failures == 0) {
        std::puts("All MainWindow tests passed.");
    }
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
