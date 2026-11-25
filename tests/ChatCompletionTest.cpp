// Tests for ChatCompletion (streamed chat completions), stripReasoning() and the plan prompt.

#include "FakeLmServer.h"
#include "LmChat.h"
#include "PlanChat.h"
#include "PlanPrompt.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QRegularExpression>
#include <QTimer>

#include <cstdio>
#include <cstdlib>
#include <optional>

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

struct Outcome {
    QStringList updates; // every textChanged() value
    int thinking = 0;
    std::optional<QString> text;
    bool truncated = false;
    std::optional<QString> error;
};

Outcome run(const QUrl &server, int timeoutMs = 10000)
{
    ChatCompletion chat(server, "test-model", PlanPrompt::messages("A synopsis."), 0.6);
    Outcome out;
    QEventLoop loop;
    QObject::connect(&chat, &ChatCompletion::textChanged, [&](const QString &t) { out.updates << t; });
    QObject::connect(&chat, &ChatCompletion::thinking, [&] { ++out.thinking; });
    QObject::connect(&chat, &ChatCompletion::finished, &loop, [&](const QString &t, bool truncated) {
        out.text = t;
        out.truncated = truncated;
        loop.quit();
    });
    QObject::connect(&chat, &ChatCompletion::failed, &loop, [&](const QString &e) {
        out.error = e;
        loop.quit();
    });
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    chat.start();
    loop.exec();
    return out;
}

// @relation(AI-5, FR-GEN-3, scope=function)
void testStripReasoning()
{
    EXPECT(stripReasoning("Hello") == "Hello");
    EXPECT(stripReasoning("<think>plan it</think>\n\n# Plan") == "# Plan");
    EXPECT(stripReasoning("<think>a</think>A<think>b</think>B") == "AB");
    EXPECT(stripReasoning("<think>still thinking") == "");
    EXPECT(stripReasoning("# Plan\n<thi") == "# Plan\n");                // tag still arriving
    EXPECT(stripReasoning("reasoning without opening tag</think>\n# Plan") == "# Plan"); // template opened it
    EXPECT(stripReasoning("  \n# Plan  ") == "# Plan  ");
}

// @relation(AI-6, FR-TPL-2, scope=function)
void testPrompt()
{
    const QString synopsis = "  Line one\r\n\tLine two \xC2\xA0 with \"quotes\"  ";
    const QList<ChatMessage> messages = PlanPrompt::messages(synopsis);
    EXPECT(messages.size() == 2);
    EXPECT(messages[0].role == "system" && messages[1].role == "user");
    EXPECT(messages[1].content.contains(synopsis)); // embedded verbatim
    EXPECT(messages[1].content.endsWith(synopsis + "\n\"\"\""));
    EXPECT(messages[1].content.contains("### Act 1:") && messages[1].content.contains("#### Chapter 1:") &&
           messages[1].content.contains("- **Scene 1:"));
}

// @relation(FR-TPL-2, FR-TPL-3, FR-TPL-5, AI-6, scope=function)
void testPlanTemplates()
{
    const auto &all = PlanPrompt::templates();
    EXPECT(all.size() == 2);
    EXPECT(all[0].id == PlanPrompt::kThreeActTemplate && all[1].id == PlanPrompt::kInnerStagesTemplate);
    EXPECT(PlanPrompt::templateById("no-such-template").id == PlanPrompt::kThreeActTemplate); // default
    EXPECT(PlanPrompt::templateById("").id == PlanPrompt::kThreeActTemplate);

    // The default is the three-act prompt, unchanged.
    EXPECT(PlanPrompt::userPrompt("S.") == PlanPrompt::userPrompt("S.", PlanPrompt::kThreeActTemplate));
    EXPECT(PlanPrompt::userPrompt("S.").contains("### Act 1:") && PlanPrompt::userPrompt("S.").contains("Story spine"));

    // The inner-stages prompt: stages instead of acts, time markers, no forced climax, synopsis verbatim.
    const QString synopsis = "  Elin, a retired cartographer, maps her town.\r\n\tShe never leaves it.  ";
    const QString stages = PlanPrompt::userPrompt(synopsis, PlanPrompt::kInnerStagesTemplate);
    EXPECT(stages.contains("## Stage progression") && stages.contains("### Stage 1:") && !stages.contains("### Act 1:"));
    EXPECT(stages.contains("#### Chapter 1:") && stages.contains("- **Scene 1:") && stages.contains("Time:"));
    EXPECT(stages.contains("many years later") && stages.contains("happy ending"));
    EXPECT(stages.endsWith(synopsis + "\n\"\"\""));
    const QList<ChatMessage> m = PlanPrompt::messages(synopsis, PlanPrompt::kInnerStagesTemplate);
    EXPECT(m.size() == 2 && m[0].content == PlanPrompt::systemPrompt() && m[1].content == stages);
}

// @relation(AI-7, AI-8, FR-CHT-2, FR-CHT-3, FR-CHT-4, scope=function)
void testPlanChat()
{
    using PlanChat::Turn;
    // History round trip.
    const QList<Turn> history{{"user", "Shorter Act 1", false}, {"assistant", "Merged chapters 2 and 3.", true},
                              {"user", "Why?", false}, {"assistant", "Pacing.", false}};
    const QList<Turn> back = PlanChat::parse(PlanChat::serialize(history));
    EXPECT(back.size() == 4 && back[1].planApplied && !back[3].planApplied && back[2].content == "Why?");
    EXPECT(PlanChat::serialize({}).isEmpty() && PlanChat::parse("").isEmpty() && PlanChat::parse("not json").isEmpty());

    // Messages: the current plan and synopsis go in the system prompt; revised plans aren't repeated.
    const QList<ChatMessage> m = PlanChat::messages("The synopsis.", "# Current plan", history, "Add a subplot");
    EXPECT(m.size() == 6);
    EXPECT(m[0].role == "system" && m[0].content.contains("<synopsis>\nThe synopsis.\n</synopsis>") &&
           m[0].content.contains("<current_plan>\n# Current plan\n</current_plan>") && m[0].content.contains("<plan>"));
    EXPECT(m[2].role == "assistant" && m[2].content.startsWith("Merged chapters 2 and 3.") &&
           m[2].content.contains("includes this revision"));
    EXPECT(m[4].content == "Pacing.");
    EXPECT(m[5].role == "user" && m[5].content.startsWith("Add a subplot") &&
           m[5].content.contains("complete plan")); // reminder on the latest message only
    EXPECT(m[1].content == "Shorter Act 1");

    // Partial revisions are detected.
    QString full = "# Plan\n\n## Structure\n";
    for (int a = 1; a <= 3; ++a) {
        full += QString("\n### Act %1: A\n").arg(a);
        for (int c = 1; c <= 4; ++c) {
            full += QString("\n#### Chapter %1: C\n- **Scene 1: S**: something happens here.\n").arg((a - 1) * 4 + c);
        }
    }
    QString longer = full;
    longer.replace("#### Chapter 5: C\n", "#### Chapter 5: C\n- **Scene 0: New**: an added scene.\n");
    EXPECT(PlanChat::incompleteReason(full, longer).isEmpty());
    EXPECT(PlanChat::incompleteReason(full, full.left(full.indexOf("### Act 3"))).contains("2 of the plan's 3 acts"));
    const QString excerpt = "#### Chapter 10: C\n- **Scene 1: S**: x\n#### Chapter 11: C\n- **Scene 1: S**: y\n";
    EXPECT(!PlanChat::incompleteReason(full, excerpt).isEmpty());
    QString noActs = full;
    noActs.remove(QRegularExpression("### Act \\d: A\n"));
    EXPECT(PlanChat::incompleteReason(full, noActs).contains("acts"));
    EXPECT(PlanChat::incompleteReason("# Short plan", "# Even shorter").isEmpty()); // tiny plans aren't judged
    // Stage-style parts count like acts.
    QString staged = full;
    staged.replace("### Act ", "### Stage ");
    EXPECT(PlanChat::incompleteReason(staged, staged).isEmpty());
    EXPECT(PlanChat::incompleteReason(staged, staged.left(staged.indexOf("### Stage 3"))).contains("2 of the plan's 3 acts"));
    // Headings written with narrow no-break spaces (gpt-oss) still count.
    QString narrow = full;
    narrow.replace("### Act ", QString::fromUtf8("### Act "));
    narrow.replace("#### Chapter ", QString::fromUtf8("#### Chapter "));
    EXPECT(PlanChat::incompleteReason(full, narrow).isEmpty());
    EXPECT(PlanChat::incompleteReason(narrow, narrow.left(narrow.indexOf(QString::fromUtf8("Act 3")))).contains("acts"));

    // Replies.
    PlanChat::Reply r = PlanChat::parseReply("Tightened Act 1.\n\n<plan>\n# Plan\n\n### Act 1: Home\n</plan>\n");
    EXPECT(r.message == "Tightened Act 1." && r.plan && *r.plan == "# Plan\n\n### Act 1: Home" && !r.planCutOff);
    r = PlanChat::parseReply("Done.\n<plan>\n```markdown\n# Fenced\n```\n</plan>\nTell me if you want more.");
    EXPECT(r.plan && *r.plan == "# Fenced" && r.message == "Done.\n\nTell me if you want more.");
    r = PlanChat::parseReply("The midpoint is in chapter 9.");
    EXPECT(r.message == "The midpoint is in chapter 9." && !r.plan);
    r = PlanChat::parseReply("Revised.\n<plan>\n# Plan\n### Act 1");
    EXPECT(r.message == "Revised." && !r.plan && r.planCutOff);
    r = PlanChat::parseReply("<plan>\n   \n</plan>");
    EXPECT(!r.plan && r.message.isEmpty());

    // Streaming display hides the plan block (and a tag still arriving).
    int words = -1;
    EXPECT(PlanChat::streamingMessage("Working on it.\n<pl", &words) == "Working on it." && words == 0);
    EXPECT(PlanChat::streamingMessage("Done.\n<plan>\n# Plan one two", &words) == "Done." && words == 4);
    EXPECT(PlanChat::streamingMessage("Done.\n<plan>\n# P\n</plan>\nMore?", &words) == "Done.\n\nMore?");
}

// @relation(AI-3, scope=function)
void testStreaming()
{
    FakeServer server;
    server.routes["/v1/chat/completions"] = {200, sseStream({"# My ", "Plan\n\n", "## Premise\nA hero."}), "text/event-stream"};
    const Outcome out = run(server.url());
    EXPECT(out.text && *out.text == "# My Plan\n\n## Premise\nA hero.");
    EXPECT(!out.truncated && !out.error);
    EXPECT((out.updates == QStringList{"# My ", "# My Plan\n\n", "# My Plan\n\n## Premise\nA hero."}));

    // The request: OpenAI-compatible, streamed, with the chosen model and the prompt.
    const QJsonObject request = QJsonDocument::fromJson(server.lastRequestBody).object();
    EXPECT(server.requested == QStringList{"/v1/chat/completions"});
    EXPECT(request.value("model").toString() == "test-model");
    EXPECT(request.value("stream").toBool());
    EXPECT(qAbs(request.value("temperature").toDouble() - 0.6) < 1e-9);
    EXPECT(request.value("messages").toArray().size() == 2);
    EXPECT(request.value("messages").toArray().at(1).toObject().value("content").toString().contains("A synopsis."));
}

// @relation(AI-5, scope=function)
void testReasoningIsHidden()
{
    FakeServer server;
    // Separate reasoning channel (LM Studio) plus an inline <think> block split across chunks.
    server.routes["/v1/chat/completions"] = {
        200, sseStream({"<thi", "nk>hmm, acts...</th", "ink>\n\n# Plan", "\nDone."}, "stop", true, {"Let me think"}),
        "text/event-stream"};
    const Outcome out = run(server.url());
    EXPECT(out.text && *out.text == "# Plan\nDone.");
    EXPECT(out.thinking >= 2);
    for (const QString &update : out.updates) {
        EXPECT(!update.contains("think") && !update.contains("hmm"));
    }
}

// @relation(AI-9, scope=function)
void testTruncated()
{
    FakeServer server;
    server.routes["/v1/chat/completions"] = {200, sseStream({"# Plan\n### Act 1"}, "length"), "text/event-stream"};
    const Outcome out = run(server.url());
    EXPECT(out.text && *out.text == "# Plan\n### Act 1");
    EXPECT(out.truncated);
}

// @relation(AI-9, scope=function)
void testErrors()
{
    {
        FakeServer server; // LM Studio's answer for an unknown model
        server.routes["/v1/chat/completions"] = {404, R"({"error":{"message":"Model not found: test-model"}})"};
        const Outcome out = run(server.url());
        EXPECT(out.error && out.error->contains("Model not found") && out.error->contains("404"));
    }
    {
        FakeServer server; // error event inside the stream
        server.routes["/v1/chat/completions"] = {200, "data: {\"error\":{\"message\":\"Context overflow\"}}\n\n",
                                                 "text/event-stream"};
        const Outcome out = run(server.url());
        EXPECT(out.error && out.error->contains("Context overflow"));
    }
    {
        QTcpServer probe; // connection refused
        probe.listen(QHostAddress::LocalHost);
        const quint16 port = probe.serverPort();
        probe.close();
        const Outcome out = run(QUrl(QStringLiteral("http://127.0.0.1:%1").arg(port)));
        EXPECT(out.error.has_value() && !out.text);
    }
}

// @relation(NFR-3, scope=function)
void testAbort()
{
    FakeServer server;
    server.routes["/v1/chat/completions"] = {200, sseStream({"# Partial"}, "", false), "text/event-stream", true};
    ChatCompletion chat(server.url(), "test-model", PlanPrompt::messages("x"), 0.6);
    QString latest;
    bool ended = false;
    QObject::connect(&chat, &ChatCompletion::textChanged, [&](const QString &t) { latest = t; });
    QObject::connect(&chat, &ChatCompletion::finished, [&] { ended = true; });
    QObject::connect(&chat, &ChatCompletion::failed, [&] { ended = true; });
    chat.start();
    QEventLoop loop;
    QTimer::singleShot(1500, &loop, &QEventLoop::quit);
    loop.exec();
    EXPECT(latest == "# Partial" && chat.isRunning());
    chat.abort();
    QTimer::singleShot(300, &loop, &QEventLoop::quit);
    loop.exec();
    EXPECT(!ended && !chat.isRunning()); // nothing emitted after abort()
}

// Optional: generate a real plan. Set NOVELWRITER_LIVE_LMSTUDIO (server) and NOVELWRITER_LIVE_PLANNER_MODEL;
// the plan is written to NOVELWRITER_LIVE_PLAN_OUT (or printed). With NOVELWRITER_LIVE_REFINE_PLAN set to a plan
// file, the plan is instead refined through the plan chat with NOVELWRITER_LIVE_REFINE_REQUEST.
// @relation(NFR-12, scope=function)
void livePlan()
{
    const QString server = qEnvironmentVariable("NOVELWRITER_LIVE_LMSTUDIO");
    const QString model = qEnvironmentVariable("NOVELWRITER_LIVE_PLANNER_MODEL");
    if (server.isEmpty() || model.isEmpty()) {
        return;
    }
    const QString synopsis = QStringLiteral(
        "After her brother drowns, Mira Holt takes over his post as keeper of the lighthouse on Skarra, a "
        "storm-battered island off the northern coast. In his room she finds an unsent letter addressed to a "
        "woman in Tromsø, confessing that the wreck of the trawler Albatross twelve years ago was no accident: "
        "the lamp was put out on purpose, and the island's council covered it up. Mira, who has never left the "
        "island, must decide whether to deliver the letter. As she starts asking questions, the islanders she "
        "grew up with close ranks, the council leader offers her the keeper's post for life if she burns the "
        "letter, and she learns that her own father kept the light the night the Albatross sank. During the "
        "winter's worst storm Mira sails north alone in her brother's boat. She delivers the letter to the widow "
        "of the Albatross's captain, testifies at the reopened inquiry, and returns to Skarra knowing she will "
        "never be welcome there again, but relighting the lamp one last time before handing over the keys.");
    // NOVELWRITER_LIVE_SYNOPSIS_FILE replaces the sample synopsis; NOVELWRITER_LIVE_TEMPLATE picks the template.
    QString chosenSynopsis = synopsis;
    if (const QString file = qEnvironmentVariable("NOVELWRITER_LIVE_SYNOPSIS_FILE"); !file.isEmpty()) {
        QFile in(file);
        EXPECT(in.open(QIODevice::ReadOnly));
        chosenSynopsis = QString::fromUtf8(in.readAll());
    }
    const QString templateId = PlanPrompt::templateById(qEnvironmentVariable("NOVELWRITER_LIVE_TEMPLATE")).id;
    std::printf("Template: %s\n", qPrintable(templateId));
    QList<ChatMessage> messages = PlanPrompt::messages(chosenSynopsis, templateId);
    double temperature = PlanPrompt::kTemperature;
    const QString refinePath = qEnvironmentVariable("NOVELWRITER_LIVE_REFINE_PLAN");
    if (!refinePath.isEmpty()) {
        QFile file(refinePath);
        EXPECT(file.open(QIODevice::ReadOnly));
        const QString request = qEnvironmentVariable("NOVELWRITER_LIVE_REFINE_REQUEST");
        messages = PlanChat::messages(chosenSynopsis, QString::fromUtf8(file.readAll()), {}, request);
        temperature = PlanChat::kTemperature;
        std::printf("Refining %s: \"%s\"\n", qPrintable(refinePath), qPrintable(request));
    }
    std::printf("Live plan with %s at %s...\n", qPrintable(model), qPrintable(server));
    QElapsedTimer timer;
    timer.start();
    ChatCompletion chat(QUrl(server), model, messages, temperature);
    std::optional<QString> text, error;
    bool truncated = false;
    int thinking = 0;
    QEventLoop loop;
    QObject::connect(&chat, &ChatCompletion::thinking, [&] { ++thinking; });
    QObject::connect(&chat, &ChatCompletion::finished, &loop, [&](const QString &t, bool cut) {
        text = t;
        truncated = cut;
        loop.quit();
    });
    QObject::connect(&chat, &ChatCompletion::failed, &loop, [&](const QString &e) {
        error = e;
        loop.quit();
    });
    QTimer::singleShot(20 * 60 * 1000, &loop, &QEventLoop::quit);
    chat.start();
    loop.exec();
    std::printf("Finished in %.0f s; thinking events: %d; truncated: %s\n", timer.elapsed() / 1000.0, thinking,
                truncated ? "yes" : "no");
    EXPECT(text.has_value() && !error);
    if (error) {
        std::printf("Error: %s\n", qPrintable(*error));
    }
    if (text && !refinePath.isEmpty()) {
        const PlanChat::Reply reply = PlanChat::parseReply(*text);
        std::printf("Reply message: %s\nRevised plan: %s%s\n", qPrintable(reply.message),
                    reply.plan ? "yes" : "no", reply.planCutOff ? " (cut off)" : "");
        EXPECT(reply.plan.has_value());
        if (reply.plan) {
            *text = *reply.plan;
        }
    }
    if (text) {
        EXPECT(!text->contains("<think>"));
        if (const QString out = qEnvironmentVariable("NOVELWRITER_LIVE_PLAN_OUT"); !out.isEmpty()) {
            QFile file(out);
            if (file.open(QIODevice::WriteOnly)) {
                file.write(text->toUtf8());
            }
            std::printf("Plan (%lld characters) written to %s\n", static_cast<long long>(text->size()),
                        qPrintable(out));
        } else {
            std::printf("%s\n", qPrintable(*text));
        }
    }
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    livePlan();
    testStripReasoning();
    testPrompt();
    testPlanTemplates();
    testPlanChat();
    testStreaming();
    testReasoningIsHidden();
    testTruncated();
    testErrors();
    testAbort();
    if (g_failures == 0) {
        std::puts("All ChatCompletion tests passed.");
    }
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
