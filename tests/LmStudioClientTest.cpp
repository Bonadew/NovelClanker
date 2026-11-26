// Tests for LmStudioClient against a fake HTTP server, plus normalizeServerUrl().

#include "LmStudioClient.h"
#include "FakeLmServer.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTcpServer>
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

struct Result {
  std::optional<QList<LmModel>> models;
  std::optional<QString> error;
};

Result fetch(const QUrl &server, int timeoutMs = 10000)
{
  LmStudioClient client;
  Result result;
  QEventLoop loop;
  QObject::connect(&client, &LmStudioClient::modelsFetched, &loop, [&](const QList<LmModel> &models) {
    result.models = models;
    loop.quit();
  });
  QObject::connect(&client, &LmStudioClient::fetchFailed, &loop, [&](const QString &error) {
    result.error = error;
    loop.quit();
  });
  QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
  client.fetchModels(server);
  loop.exec();
  return result;
}

QStringList ids(const QList<LmModel> &models)
{
  QStringList out;
  for (const LmModel &m : models) {
    out << m.id;
  }
  return out;
}

// @relation(AI-1, FR-SET-2, scope=function)
void testNativeV1()
{
  FakeServer server;
  server.routes["/api/v1/models"] = {200, kSampleNativeModels};
  const Result r = fetch(server.url());
  EXPECT(r.models.has_value());
  if (r.models) {
    EXPECT((ids(*r.models) == QStringList{"alpha-27b", "zeta-7b"})); // embeddings dropped, sorted by name
    EXPECT(r.models->at(0).displayName == "alpha 27B");
    EXPECT(r.models->at(0).details == "IQ2_S");
    EXPECT(r.models->at(0).loaded);
    EXPECT(r.models->at(1).details == "7B · Q4_K_M");
    EXPECT(!r.models->at(1).loaded);
  }
  EXPECT((server.requested == QStringList{"/api/v1/models"}));
}

// @relation(AI-1, scope=function)
void testFallbackToNativeV0()
{
  FakeServer server;
  server.routes["/api/v0/models"] = {200, R"({"data": [
        {"id": "b-model", "type": "vlm", "quantization": "Q8_0", "state": "loaded"},
        {"id": "embed", "type": "embeddings", "state": "not-loaded"},
        {"id": "a-model", "type": "llm", "state": "not-loaded"}
    ]})"};
  const Result r = fetch(server.url());
  EXPECT(r.models && (ids(*r.models) == QStringList{"a-model", "b-model"}));
  EXPECT(r.models && r.models->at(1).loaded && r.models->at(1).details == "Q8_0");
  EXPECT((server.requested == QStringList{"/api/v1/models", "/api/v0/models"}));
}

// @relation(AI-1, scope=function)
void testFallbackToOpenAi()
{
  FakeServer server;
  server.routes["/v1/models"] = {200, R"({"data": [{"id": "m2"}, {"id": "m1"}], "object": "list"})"};
  const Result r = fetch(server.url());
  EXPECT(r.models && (ids(*r.models) == QStringList{"m1", "m2"}));
  EXPECT((server.requested == QStringList{"/api/v1/models", "/api/v0/models", "/v1/models"}));
}

// @relation(FR-SET-1, scope=function)
void testServerPathPrefix()
{
  // A server behind a reverse proxy at a sub-path.
  FakeServer server;
  server.routes["/lmstudio/api/v1/models"] = {200, R"({"models": []})"};
  const Result r = fetch(server.url("/lmstudio"));
  EXPECT(r.models && r.models->isEmpty());
}

// @relation(AI-1, AI-2, scope=function)
void testErrors()
{
  {
    FakeServer server;
    server.routes["/api/v1/models"] = {200, "this is not json"};
    const Result r = fetch(server.url());
    EXPECT(r.error.has_value());
  }
  {
    FakeServer server;
    server.routes["/api/v1/models"] = {500, R"({"error":"boom"})"};
    const Result r = fetch(server.url());
    EXPECT(r.error && r.error->contains("500"));
    EXPECT((server.requested == QStringList{"/api/v1/models"})); // no fallback on server errors
  }
  {
    FakeServer server; // nothing routed: every endpoint 404s
    const Result r = fetch(server.url());
    EXPECT(r.error.has_value());
  }
  {
    // Connection refused: grab a free port, then close it.
    QTcpServer probe;
    probe.listen(QHostAddress::LocalHost);
    const quint16 port = probe.serverPort();
    probe.close();
    const Result r = fetch(QUrl(QStringLiteral("http://127.0.0.1:%1").arg(port)));
    EXPECT(r.error.has_value() && !r.models);
  }
  {
    FakeServer server;
    server.silent = true;
    const Result r = fetch(server.url(), 15000);
    EXPECT(r.error && r.error->contains("respond"));
  }
}

// @relation(FR-SET-1, scope=function)
void testNormalizeServerUrl()
{
  const auto norm = [](const char *in) { return normalizeServerUrl(QString::fromUtf8(in)).toString(); };
  EXPECT(norm("http://127.0.0.1:1234") == "http://127.0.0.1:1234");
  EXPECT(norm("  127.0.0.1:1234  ") == "http://127.0.0.1:1234");
  EXPECT(norm("localhost:1234/") == "http://localhost:1234");
  EXPECT(norm("http://127.0.0.1:1234/v1") == "http://127.0.0.1:1234");
  EXPECT(norm("http://127.0.0.1:1234/api/v1/") == "http://127.0.0.1:1234");
  EXPECT(norm("https://gpu-box.lan/lmstudio/v1") == "https://gpu-box.lan/lmstudio");
  EXPECT(norm("gpu-box") == "http://gpu-box");
  EXPECT(norm("").isEmpty());
  EXPECT(norm("ftp://127.0.0.1:1234").isEmpty());
  EXPECT(norm("http://").isEmpty());
  EXPECT(norm("http://127.0.0.1:1234/?x=1").isEmpty());
}

} // namespace

int main(int argc, char **argv)
{
  QCoreApplication app(argc, argv);
  testNormalizeServerUrl();
  testNativeV1();
  testFallbackToNativeV0();
  testFallbackToOpenAi();
  testServerPathPrefix();
  testErrors();
  if (g_failures == 0) {
    std::puts("All LmStudioClient tests passed.");
  }
  return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
