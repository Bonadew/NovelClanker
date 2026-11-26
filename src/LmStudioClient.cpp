#include "LmStudioClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>

#include <algorithm>
#include <optional>

namespace {

constexpr int kTimeoutMs = 5000;

// Response formats:
//   GET /api/v1/models  {"models": [{"type": "llm"|"embedding", "key", "display_name", "params_string",
//                                    "quantization": {"name"}, "loaded_instances": [...]}, ...]}
//   GET /api/v0/models  {"data": [{"id", "type": "llm"|"vlm"|"embeddings", "quantization", "state"}, ...]}
//   GET /v1/models      {"data": [{"id"}, ...]}   (OpenAI-compatible; no type information)
std::optional<QList<LmModel>> parseNativeV1(const QJsonObject &root)
{
  if (!root.value("models").isArray()) {
    return std::nullopt;
  }
  QList<LmModel> models;
  for (const QJsonValue &value : root.value("models").toArray()) {
    const QJsonObject m = value.toObject();
    const QString id = m.value("key").toString();
    if (id.isEmpty() || m.value("type").toString() == "embedding") {
      continue;
    }
    QStringList details;
    if (const QString params = m.value("params_string").toString(); !params.isEmpty()) {
      details << params;
    }
    if (const QString quant = m.value("quantization").toObject().value("name").toString(); !quant.isEmpty()) {
      details << quant;
    }
    const QString name = m.value("display_name").toString();
    models.append({id, name.isEmpty() ? id : name, details.join(QStringLiteral(" · ")),
                   !m.value("loaded_instances").toArray().isEmpty()});
  }
  return models;
}

std::optional<QList<LmModel>> parseNativeV0(const QJsonObject &root)
{
  if (!root.value("data").isArray()) {
    return std::nullopt;
  }
  QList<LmModel> models;
  for (const QJsonValue &value : root.value("data").toArray()) {
    const QJsonObject m = value.toObject();
    const QString id = m.value("id").toString();
    if (id.isEmpty() || m.value("type").toString() == "embeddings") {
      continue;
    }
    models.append({id, id, m.value("quantization").toString(), m.value("state").toString() == "loaded"});
  }
  return models;
}

std::optional<QList<LmModel>> parseOpenAi(const QJsonObject &root)
{
  if (!root.value("data").isArray()) {
    return std::nullopt;
  }
  QList<LmModel> models;
  for (const QJsonValue &value : root.value("data").toArray()) {
    const QString id = value.toObject().value("id").toString();
    if (!id.isEmpty()) {
      models.append({id, id, {}, false});
    }
  }
  return models;
}

} // namespace

QUrl normalizeServerUrl(const QString &input)
{
  const QString trimmed = input.trimmed();
  if (trimmed.isEmpty()) {
    return {};
  }
  // fromUserInput() would read "127.0.0.1:1234" as scheme "127.0.0.1"; add a scheme if there is none.
  static const QRegularExpression hasScheme(R"(^[a-zA-Z][a-zA-Z0-9+.-]*://)");
  QUrl url(hasScheme.match(trimmed).hasMatch() ? trimmed : "http://" + trimmed, QUrl::StrictMode);
  if (!url.isValid() || url.host().isEmpty() || (url.scheme() != "http" && url.scheme() != "https") ||
      url.hasQuery() || url.hasFragment()) {
    return {};
  }
  // Accept addresses copied with an API path ("/v1", "/api/v1", ...) and trailing slashes.
  QString path = url.path();
  static const QRegularExpression apiSuffix(R"((/api)?/v\d+/?$)");
  path.remove(apiSuffix);
  while (path.endsWith('/')) {
    path.chop(1);
  }
  url.setPath(path);
  return url;
}

LmStudioClient::LmStudioClient(QObject *parent) : QObject(parent), m_network(new QNetworkAccessManager(this))
{
}

void LmStudioClient::fetchModels(const QUrl &server)
{
  cancel();
  m_server = server;
  request(Api::NativeV1);
}

void LmStudioClient::cancel()
{
  if (QNetworkReply *reply = m_reply) {
    m_reply = nullptr; // mark as stale before abort() emits finished()
    reply->abort();
  }
}

void LmStudioClient::request(Api api)
{
  static constexpr const char *kPaths[] = {"/api/v1/models", "/api/v0/models", "/v1/models"};
  QUrl url = m_server;
  url.setPath(m_server.path() + kPaths[static_cast<int>(api)]);

  QNetworkRequest request(url);
  request.setRawHeader("Accept", "application/json");
  request.setTransferTimeout(kTimeoutMs);
  QNetworkReply *reply = m_network->get(request);
  m_reply = reply;
  connect(reply, &QNetworkReply::finished, this, [this, reply, api] { onFinished(reply, api); });
}

void LmStudioClient::onFinished(QNetworkReply *reply, Api api)
{
  reply->deleteLater();
  if (reply != m_reply) {
    return; // cancelled or superseded
  }
  m_reply = nullptr;

  const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  if (status == 0) { // no HTTP response at all: connection refused, unknown host, timeout, ...
    emit fetchFailed(reply->error() == QNetworkReply::OperationCanceledError
                         ? tr("The server did not respond within %1 seconds.").arg(kTimeoutMs / 1000)
                         : reply->errorString());
    return;
  }
  if (status == 404 && api != Api::OpenAi) {
    request(api == Api::NativeV1 ? Api::NativeV0 : Api::OpenAi); // older LM Studio (or another server)
    return;
  }
  if (status != 200) {
    emit fetchFailed(tr("The server answered with HTTP status %1.").arg(status));
    return;
  }

  const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
  std::optional<QList<LmModel>> models;
  if (doc.isObject()) {
    switch (api) {
    case Api::NativeV1:
      models = parseNativeV1(doc.object());
      break;
    case Api::NativeV0:
      models = parseNativeV0(doc.object());
      break;
    case Api::OpenAi:
      models = parseOpenAi(doc.object());
      break;
    }
  }
  if (!models) {
    emit fetchFailed(tr("Unexpected response from the server. Is this an LM Studio server?"));
    return;
  }
  std::sort(models->begin(), models->end(), [](const LmModel &a, const LmModel &b) {
    return QString::compare(a.displayName, b.displayName, Qt::CaseInsensitive) < 0;
  });
  emit modelsFetched(*models);
}
