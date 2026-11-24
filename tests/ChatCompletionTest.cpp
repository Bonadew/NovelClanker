// Tests for ChatCompletion (streamed chat completions), stripReasoning() and the plan prompt.

#include "FakeLmServer.h"
#include "LmChat.h"
#include "PlanPrompt.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
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

void testTruncated()
{
    FakeServer server;
    server.routes["/v1/chat/completions"] = {200, sseStream({"# Plan\n### Act 1"}, "length"), "text/event-stream"};
    const Outcome out = run(server.url());
    EXPECT(out.text && *out.text == "# Plan\n### Act 1");
    EXPECT(out.truncated);
}

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
// the plan is written to NOVELWRITER_LIVE_PLAN_OUT (or printed).
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
    std::printf("Live plan with %s at %s...\n", qPrintable(model), qPrintable(server));
    QElapsedTimer timer;
    timer.start();
    ChatCompletion chat(QUrl(server), model, PlanPrompt::messages(synopsis), PlanPrompt::kTemperature);
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
