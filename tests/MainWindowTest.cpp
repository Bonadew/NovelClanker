// End-to-end test of the synopsis and plan views, driven programmatically against a fake LM Studio server.
// Run with QT_QPA_PLATFORM=minimal (no window appears). Usage: mainwindow_test <scratch-dir>

#include "AiSettings.h"
#include "FakeLmServer.h"
#include "MainWindow.h"
#include "NovelStore.h"
#include "PlanChat.h"
#include "Theme.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QStyle>
#include <QStyleHints>
#include <QTextBlock>
#include <QTextLayout>
#include <QTabWidget>
#include <QTextCursor>
#include <QTextBrowser>
#include <QTest>
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

// @relation(FR-PLN-1, FR-PLN-3, FR-PLN-4, FR-GEN-1, FR-GEN-2, FR-GEN-3, FR-SYN-3, scope=function)
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
        EXPECT(u.planOrigin->text() == "Generated with planner-x using the \"Three acts (25/50/25)\" template.");
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
        EXPECT(u.planOrigin->text() == "Generated with planner-y using the \"Three acts (25/50/25)\" template.");

        // A plan written by hand.
        u.newPlan->click();
        EXPECT(u.planRows() == "Plan 1|planner-x Plan 2|planner-y Plan 3|(by hand)");
        EXPECT(u.planOrigin->text() == "Written by hand." && u.plan->isEnabled() && u.plan->toPlainText().isEmpty());
        u.plan->insertPlainText("# My own plan");

        // Rename it (as the inline editor would), then switch back to the first plan: edits are kept.
        u.planList->topLevelItem(2)->setText(0, "  Mine  ");
        EXPECT(u.planList->topLevelItem(2)->text(0) == "Mine");
        u.planList->setCurrentItem(u.planList->topLevelItem(0));
        EXPECT(u.planOrigin->text() == "Generated with planner-x using the \"Three acts (25/50/25)\" template.");
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

// @relation(FR-PLN-1, FR-SYN-1, scope=function)
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

// @relation(FR-GEN-4, FR-GEN-7, NFR-2, scope=function)
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

// @relation(FR-GEN-5, scope=function)
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

// @relation(FR-SYN-2, scope=function)
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

// @relation(FR-CHT-1, FR-CHT-2, FR-CHT-3, FR-CHT-6, scope=function)
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

// @relation(FR-CHT-5, scope=function)
void testChatHistoryIsRestored(const fs::path &dir)
{
    MainWindow w;
    EXPECT(w.openNovel(toQ(dir), NovelStore::Mode::Open));
    auto *transcript = w.findChild<QTextBrowser *>("chatTranscript");
    EXPECT(transcript->toPlainText().contains("Please add a second act.") &&
           transcript->toPlainText().contains("Act 2 is where Mira travels."));
}

QString treeOutline(QTreeWidget *tree)
{
    QStringList acts;
    for (int a = 0; a < tree->topLevelItemCount(); ++a) {
        QTreeWidgetItem *act = tree->topLevelItem(a);
        QStringList chapters;
        for (int c = 0; c < act->childCount(); ++c) {
            QStringList scenes;
            for (int s = 0; s < act->child(c)->childCount(); ++s) {
                scenes << act->child(c)->child(s)->text(0);
            }
            chapters << act->child(c)->text(0) + "(" + scenes.join(',') + ")";
        }
        acts << act->text(0) + "[" + chapters.join(',') + "]";
    }
    return acts.join(' ');
}

// @relation(FR-IMP-3, FR-IMP-4, FR-IMP-5, FR-STR-7, scope=function)
void testCreateStructureFromPlan(const fs::path &dir)
{
    FakeServer server;
    useServer(server, "planner-x");
    const QString plan = "# The Letter\n\n## Story spine\n1. Mira finds a letter (Act 1)\n\n## Structure\n\n"
                         "### Act 1: Home\nMira's world.\n\n"
                         "#### Chapter 1: The Cold Room\nIntroduce Mira's grief.\n"
                         "- **Scene 1: The Letter**: POV: Mira. Setting: her brother's room. She finds the letter.\n"
                         "- **Scene 2: The Council**: POV: Mira. The council offers her the post.\n\n"
                         "### Act 2: Away\n\n#### Chapter 2: The Storm\n"
                         "- **Scene 1: Waves**: POV: Mira. She nearly loses the letter.\n\n"
                         "## Open questions\n- Who knew?\n";
    {
        MainWindow w;
        EXPECT(w.openNovel(toQ(dir), NovelStore::Mode::Create));
        const Ui u = ui(w);
        auto *tree = w.findChild<QTreeWidget *>("manuscriptTree");
        auto *brief = w.findChild<QLabel *>("briefLabel");
        auto *create = w.findChild<QPushButton *>("createStructureButton");
        EXPECT(tree && brief && create);
        EXPECT(treeOutline(tree) == "Act 1[Chapter 1(Scene 1)]"); // a new novel's untouched start
        EXPECT(!create->isEnabled());                             // no plan selected

        u.newPlan->click();
        u.plan->insertPlainText(plan);
        EXPECT(create->isEnabled());
        create->click(); // the untouched start is replaced without asking

        EXPECT(treeOutline(tree) == "Act 1: Home[Chapter 1: The Cold Room(The Letter,The Council)] "
                                    "Act 2: Away[Chapter 2: The Storm(Waves)]");
        EXPECT(u.tabs->currentIndex() == 0); // back on the Manuscript tab
        EXPECT(tree->currentItem() && tree->currentItem()->text(0) == "The Letter");
        EXPECT(!brief->isHidden() &&
               brief->text() == "Brief: POV: Mira. Setting: her brother's room. She finds the letter.");
        tree->setCurrentItem(tree->topLevelItem(0)->child(0));
        EXPECT(brief->text() == "Brief: Introduce Mira's grief.");
        tree->setCurrentItem(tree->topLevelItem(1)); // Act 2 has no brief
        EXPECT(brief->isHidden());

        // Scenes start empty and are ready for writing.
        tree->setCurrentItem(tree->topLevelItem(0)->child(0)->child(1));
        auto *scene = w.findChild<QPlainTextEdit *>("sceneEdit");
        EXPECT(scene->isEnabled() && scene->toPlainText().isEmpty());
        scene->insertPlainText("The council hall was cold.");
        w.close();
    }
    NovelStore store(dir, NovelStore::Mode::Open);
    EXPECT(store.acts().size() == 2);
    EXPECT(store.acts()[0].brief == "Mira's world.");
    EXPECT(store.acts()[0].chapters[0].scenes[1].brief == "POV: Mira. The council offers her the post.");
    EXPECT(store.sceneText(store.acts()[0].chapters[0].scenes[1].id) == "The council hall was cold.");
    EXPECT(store.plans().size() == 1); // the plan itself is unchanged
}

// @relation(FR-TPL-1, FR-PLN-4, scope=function)
void testPlanTemplateChoice(const fs::path &dir)
{
    FakeServer server;
    server.routes["/v1/chat/completions"] = {
        200,
        sseStream({"# Tide Tables\n\n## Stage progression\n1. **Denial**: nothing has changed.\n\n## Structure\n\n",
                   "### Stage 1: Denial\n#### Chapter 1: The Map Room\n",
                   "- **Scene 1: Ink**: POV: Elin. Time: early spring. She redraws the harbour.\n",
                   "### Stage 2: Bargaining\n#### Chapter 2: Detours\n",
                   "- **Scene 1: Letter**: POV: Elin. Time: her 71st birthday. She refuses a visit.\n"}),
        "text/event-stream"};
    useServer(server, "planner-x");
    {
        MainWindow w;
        EXPECT(w.openNovel(toQ(dir), NovelStore::Mode::Create));
        const Ui u = ui(w);
        auto *combo = w.findChild<QComboBox *>("planTemplateCombo");
        auto *hint = w.findChild<QLabel *>("planTemplateHint");
        EXPECT(combo && hint && combo->count() == 2);
        EXPECT(combo->currentData().toString() == "three-act" && hint->text().contains("three acts"));

        combo->setCurrentIndex(combo->findData("inner-stages"));
        EXPECT(hint->text().contains("inner stages"));
        u.synopsis->setPlainText("Elin, a retired cartographer, slowly stops leaving her harbour town.");
        u.generate->click();
        EXPECT(waitFor([&] { return u.planStatus->text().startsWith("Done"); }));

        const QString prompt = QJsonDocument::fromJson(server.lastRequestBody)
                                   .object()
                                   .value("messages")
                                   .toArray()
                                   .at(1)
                                   .toObject()
                                   .value("content")
                                   .toString();
        EXPECT(prompt.contains("## Stage progression") && !prompt.contains("### Act 1:"));
        EXPECT(u.planOrigin->text() == "Generated with planner-x using the \"Inner stages across years\" template.");

        // The stage plan becomes the manuscript's structure.
        w.findChild<QPushButton *>("createStructureButton")->click();
        EXPECT(treeOutline(w.findChild<QTreeWidget *>("manuscriptTree")) ==
               "Stage 1: Denial[Chapter 1: The Map Room(Ink)] Stage 2: Bargaining[Chapter 2: Detours(Letter)]");
        w.close();
    }
    {
        NovelStore store(dir, NovelStore::Mode::Open);
        EXPECT(store.planTemplate() == "inner-stages");
        EXPECT(store.plans().size() == 1 && store.plans()[0].templateId == "inner-stages" &&
               store.plans()[0].model == "planner-x");
    }
    MainWindow w; // the novel's template choice is restored
    EXPECT(w.openNovel(toQ(dir), NovelStore::Mode::Open));
    EXPECT(w.findChild<QComboBox *>("planTemplateCombo")->currentData().toString() == "inner-stages");
}

QString requestUserPrompt(const FakeServer &server)
{
    const QJsonArray messages = QJsonDocument::fromJson(server.lastRequestBody).object().value("messages").toArray();
    return messages.isEmpty() ? QString() : messages.last().toObject().value("content").toString();
}

// @relation(FR-PRS-1, FR-PRS-2, FR-PRS-3, FR-PRS-5, FR-PRS-8, FR-PRS-9, FR-PRS-10, FR-PRS-11, FR-PRS-15, NFR-2, scope=function)
void testProseWriter(const fs::path &dir)
{
    FakeServer server;
    useServer(server, "planner-x");
    AiSettings settings = AiSettings::load();
    settings.proseModel = "writer-x";
    settings.save();
    const QString plan = "# The Letter\n\n## Structure\n\n### Act 1: Home\n\n#### Chapter 1: The Cold Room\n"
                         "- **Scene 1: The Letter**: POV: Mira. She finds the letter.\n"
                         "- **Scene 2: The Council**: POV: Mira. The council offers her the post.\n";
    {
        MainWindow w;
        EXPECT(w.openNovel(toQ(dir), NovelStore::Mode::Create));
        const Ui u = ui(w);
        auto *tree = w.findChild<QTreeWidget *>("manuscriptTree");
        auto *scene = w.findChild<QPlainTextEdit *>("sceneEdit");
        auto *write = w.findChild<QPushButton *>("writeSceneButton");
        auto *expand = w.findChild<QPushButton *>("expandButton");
        auto *length = w.findChild<QComboBox *>("proseLengthCombo");
        auto *stop = w.findChild<QPushButton *>("proseStopButton");
        auto *status = w.findChild<QLabel *>("proseStatus");
        EXPECT(write && expand && length && stop && status);
        u.synopsis->setPlainText("Mira, a lighthouse keeper, finds a letter.");
        u.newPlan->click();
        u.plan->insertPlainText(plan);
        w.findChild<QPushButton *>("createStructureButton")->click();
        EXPECT(tree->currentItem() && tree->currentItem()->text(0) == "The Letter");
        EXPECT(write->isEnabled() && !expand->isEnabled()); // nothing to expand in an empty scene
        EXPECT(length->currentData().toInt() == 500);

        // Write the scene from its brief. The preamble is removed; the text streams into the editor.
        server.routes["/v1/chat/completions"] = {
            200, sseStream({"Here is the scene:\n\n", "Mira climbed ", "the stairs.\n\nThe letter waited."}),
            "text/event-stream"};
        length->setCurrentIndex(length->findData(1000));
        write->click();
        EXPECT(!write->isEnabled() && scene->isReadOnly() && !tree->isEnabled());
        EXPECT(waitFor([&] { return status->text().startsWith("Done"); }));
        EXPECT(scene->toPlainText() == "Mira climbed the stairs.\n\nThe letter waited.");
        EXPECT(!scene->isReadOnly() && tree->isEnabled() && write->isEnabled() && expand->isEnabled());
        const QJsonObject request = QJsonDocument::fromJson(server.lastRequestBody).object();
        EXPECT(request.value("model").toString() == "writer-x");
        QString prompt = requestUserPrompt(server);
        EXPECT(prompt.contains("POV: Mira. She finds the letter.") && prompt.contains("about 1000 words"));
        EXPECT(prompt.contains("<synopsis>\nMira, a lighthouse keeper, finds a letter."));
        EXPECT(prompt.contains("<next_scene_brief>\nPOV: Mira. The council offers her the post."));
        EXPECT(!prompt.contains("<previous_text>"));
        scene->undo(); // the whole generated scene is one undo step
        EXPECT(scene->toPlainText().isEmpty());
        scene->redo();
        EXPECT(scene->toPlainText() == "Mira climbed the stairs.\n\nThe letter waited.");

        // Expand a selected passage in steps; the text around it stays.
        scene->setPlainText("Intro.\n\nShe opens it.\n\nThe end came soon.");
        QTextCursor cursor = scene->textCursor();
        cursor.setPosition(7); // includes the blank line before the passage, which is left alone
        cursor.setPosition(21, QTextCursor::KeepAnchor);
        scene->setTextCursor(cursor);
        EXPECT(expand->text() == "Expand Selection");
        length->setCurrentIndex(length->findData(200));
        // The model repeats the text after the passage; the repeat is dropped.
        server.routes["/v1/chat/completions"] = {200, sseStream({"She opens it slowly, ", "and reads.\n\nThe end came soon."}),
                                                 "text/event-stream"};
        expand->click();
        EXPECT(waitFor([&] { return status->text().startsWith("Done"); }));
        EXPECT(scene->toPlainText() == "Intro.\n\nShe opens it slowly, and reads.\n\nThe end came soon.");
        prompt = requestUserPrompt(server);
        EXPECT(prompt.contains("<passage>\nShe opens it.\n</passage>"));
        EXPECT(prompt.contains("<text_before>\nIntro.\n</text_before>"));
        EXPECT(prompt.contains("<text_after>\nThe end came soon.\n</text_after>"));
        EXPECT(prompt.contains("about 200 words"));
        scene->undo();
        EXPECT(scene->toPlainText() == "Intro.\n\nShe opens it.\n\nThe end came soon.");
        scene->redo();

        // The next scene gets the end of this one as context. Stop keeps what was written so far.
        tree->setCurrentItem(tree->topLevelItem(0)->child(0)->child(1));
        EXPECT(scene->toPlainText().isEmpty() && status->text().isEmpty());
        server.routes["/v1/chat/completions"] = {200, sseStream({"The council hall ", "was cold."}, "", false),
                                                 "text/event-stream", true};
        write->click();
        EXPECT(waitFor([&] { return scene->toPlainText() == "The council hall was cold."; }));
        EXPECT(stop->isVisibleTo(&w));
        prompt = requestUserPrompt(server);
        EXPECT(prompt.contains("<previous_text>\nIntro.\n\nShe opens it slowly, and reads.\n\nThe end came soon.\n</previous_text>"));
        EXPECT(!prompt.contains("<next_scene_brief>"));
        stop->click();
        EXPECT(status->text().startsWith("Stopped"));
        EXPECT(!scene->isReadOnly() && !stop->isVisibleTo(&w) && write->isEnabled());
        EXPECT(scene->toPlainText() == "The council hall was cold.");

        // A model that keeps writing far past the chosen length is stopped at the end of a sentence.
        QStringList runaway;
        for (int i = 0; i < 120; ++i) {
            runaway << QStringLiteral("The wind rose again. ");
        }
        server.routes["/v1/chat/completions"] = {200, sseStream(runaway, "", false), "text/event-stream", true};
        scene->selectAll();
        expand->click();
        EXPECT(waitFor([&] { return status->text().startsWith("Done"); }));
        EXPECT(status->text().contains("kept writing past the chosen length"));
        const qsizetype words = scene->toPlainText().split(' ', Qt::SkipEmptyParts).size();
        EXPECT(words == 248); // 62 whole sentences of four words: the most within 250
        EXPECT(scene->toPlainText().endsWith("The wind rose again."));
        EXPECT(!scene->isReadOnly() && !stop->isVisibleTo(&w) && expand->isEnabled());
        w.close();
    }
    NovelStore store(dir, NovelStore::Mode::Open);
    const auto &scenes = store.acts()[0].chapters[0].scenes;
    EXPECT(store.sceneText(scenes[0].id) == "Intro.\n\nShe opens it slowly, and reads.\n\nThe end came soon.");
    EXPECT(QString::fromStdString(store.sceneText(scenes[1].id)).startsWith("The wind rose again."));
    EXPECT(QSettings().value("prose/length").toInt() == 200); // the length choice is remembered
}

// Lightness of the colour the highlighter gave the first block's text.
int firstBlockLightness(const QPlainTextEdit *edit)
{
    const auto formats = edit->document()->firstBlock().layout()->formats();
    return formats.isEmpty() ? -1 : formats.first().format.foreground().color().lightness();
}

// @relation(FR-APP-1, FR-APP-2, FR-APP-3, FR-APP-4, FR-APP-5, scope=function)
void testTheme(const fs::path &dir)
{
    const QString systemStyle = QApplication::style()->name();
    {
        MainWindow w;
        EXPECT(w.openNovel(toQ(dir), NovelStore::Mode::Create));
        auto *system = w.findChild<QAction *>("themeSystemAction");
        auto *light = w.findChild<QAction *>("themeLightAction");
        auto *dark = w.findChild<QAction *>("themeDarkAction");
        auto *switchTheme = w.findChild<QAction *>("switchThemeAction");
        EXPECT(system && light && dark && switchTheme);
        EXPECT(system->isChecked() && !light->isChecked() && !dark->isChecked()); // default: follow the system
        EXPECT(switchTheme->shortcut() == QKeySequence("Ctrl+Shift+D"));

        auto *scene = w.findChild<QPlainTextEdit *>("sceneEdit");
        scene->insertPlainText("# A heading");

        dark->trigger();
        QApplication::processEvents(); // widgets take on the application palette through events
        EXPECT(Theme::isDark(QApplication::palette()));
        EXPECT(Theme::isDark(scene->palette()));
        EXPECT(QApplication::style()->name().compare("fusion", Qt::CaseInsensitive) == 0);
        // (The color scheme hint to the platform isn't checked: the headless test platform ignores it.)
        EXPECT(QSettings().value("appearance/theme").toString() == "dark");
        EXPECT(firstBlockLightness(scene) > 150); // headings turn light on a dark background

        switchTheme->trigger(); // dark -> light
        EXPECT(light->isChecked() && !Theme::isDark(QApplication::palette()));
        EXPECT(QSettings().value("appearance/theme").toString() == "light");
        EXPECT(firstBlockLightness(scene) >= 0 && firstBlockLightness(scene) < 130);

        switchTheme->trigger(); // light -> dark
        EXPECT(dark->isChecked() && Theme::isDark(QApplication::palette()));

        // Optional: NOVELCLANKER_SCREENSHOT_DIR=<dir> (with QT_QPA_PLATFORM=offscreen) saves light.png and dark.png.
        if (const QString shots = qEnvironmentVariable("NOVELCLANKER_SCREENSHOT_DIR"); !shots.isEmpty()) {
            scene->setPlainText("# Chapter One\n\nThe harbour was **quiet**, and the gulls\n> watched from the "
                                "breakwater.\n\n- a list item\n- `code`");
            w.resize(1100, 700);
            w.show();
            for (QAction *mode : {light, dark}) {
                mode->trigger();
                QApplication::processEvents();
                w.grab().save(shots + "/" + (mode == light ? "light" : "dark") + ".png");
            }
        }
        w.close();
    }
    {
        MainWindow w; // the choice is remembered
        EXPECT(w.findChild<QAction *>("themeDarkAction")->isChecked());
        w.findChild<QAction *>("themeSystemAction")->trigger();
        EXPECT(QApplication::style()->name() == systemStyle); // the platform's own style again
        EXPECT(QSettings().value("appearance/theme").toString() == "system");
    }
    EXPECT(Theme::fromString("dark") == Theme::Mode::Dark && Theme::fromString("bogus") == Theme::Mode::System);
}

// Point size the highlighter gave the first block (a heading) of an editor.
qreal firstBlockPointSize(const QPlainTextEdit *edit)
{
    const auto formats = edit->document()->firstBlock().layout()->formats();
    return formats.isEmpty() ? -1 : formats.first().format.fontPointSize();
}

// @relation(FR-APP-6, FR-APP-7, scope=function)
void testTextSize(const fs::path &dir)
{
    {
        MainWindow w;
        EXPECT(w.openNovel(toQ(dir), NovelStore::Mode::Create));
        auto *scene = w.findChild<QPlainTextEdit *>("sceneEdit");
        auto *synopsis = w.findChild<QPlainTextEdit *>("synopsisEdit");
        auto *plan = w.findChild<QPlainTextEdit *>("planEdit");
        auto *zoomIn = w.findChild<QAction *>("zoomInAction");
        auto *zoomOut = w.findChild<QAction *>("zoomOutAction");
        auto *zoomReset = w.findChild<QAction *>("zoomResetAction");
        EXPECT(scene && synopsis && plan && zoomIn && zoomOut && zoomReset);
        EXPECT(scene->font().pointSize() == 12); // default
        EXPECT(zoomIn->shortcuts().contains(QKeySequence::ZoomIn) && zoomIn->shortcuts().contains(QKeySequence("Ctrl+=")));
        EXPECT(zoomOut->shortcuts().contains(QKeySequence::ZoomOut));
        EXPECT(zoomReset->shortcut() == QKeySequence("Ctrl+0"));

        scene->insertPlainText("# Heading");
        EXPECT(qFuzzyCompare(firstBlockPointSize(scene), 12 * 1.8));

        // Real key presses while writing in the scene.
        w.show();
        w.activateWindow();
        scene->setFocus();
        EXPECT(QTest::qWaitForWindowActive(&w)); // shortcuts go to the active window
        QTest::keyClick(scene, Qt::Key_Plus, Qt::ControlModifier);
        EXPECT(scene->font().pointSize() == 13);
        QTest::keyClick(scene, Qt::Key_Equal, Qt::ControlModifier); // the same key without Shift
        EXPECT(scene->font().pointSize() == 14);
        EXPECT(synopsis->font().pointSize() == 14 && plan->font().pointSize() == 14); // all writing editors
        EXPECT(qFuzzyCompare(firstBlockPointSize(scene), 14 * 1.8));                  // headings follow
        EXPECT(scene->toPlainText() == "# Heading");                                  // no "+" or "=" typed
        QTest::keyClick(scene, Qt::Key_Minus, Qt::ControlModifier);
        QTest::keyClick(scene, Qt::Key_Minus, Qt::ControlModifier);
        QTest::keyClick(scene, Qt::Key_Minus, Qt::ControlModifier);
        EXPECT(scene->font().pointSize() == 11);
        EXPECT(QSettings().value("appearance/textSize").toInt() == 11);
        QTest::keyClick(scene, Qt::Key_0, Qt::ControlModifier);
        EXPECT(scene->font().pointSize() == 12);

        // Limits: the menu entries are disabled at the ends.
        for (int i = 0; i < 30; ++i) {
            zoomIn->trigger();
        }
        EXPECT(scene->font().pointSize() == 48 && !zoomIn->isEnabled() && zoomOut->isEnabled());
        zoomOut->trigger();
        EXPECT(scene->font().pointSize() == 40 && zoomIn->isEnabled());
        w.close();
    }
    MainWindow w; // the size is remembered
    EXPECT(w.findChild<QPlainTextEdit *>("sceneEdit")->font().pointSize() == 40);
    w.findChild<QAction *>("zoomResetAction")->trigger();
    EXPECT(QSettings().value("appearance/textSize").toInt() == 12);
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName("NovelClankerTests");
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
    testCreateStructureFromPlan(root / "structure.novel");
    testPlanTemplateChoice(root / "template.novel");
    testProseWriter(root / "prose.novel");
    testTheme(root / "theme.novel");
    testTextSize(root / "textsize.novel");

    QSettings().clear();
    if (g_failures == 0) {
        std::puts("All MainWindow tests passed.");
    }
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
