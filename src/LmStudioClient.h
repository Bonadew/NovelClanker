#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

struct LmModel {
  QString id;          // identifier used in API requests
  QString displayName; // human-friendly name (falls back to id)
  QString details;     // e.g. "27B · Q4_K_M"; may be empty
  bool loaded = false; // currently loaded in LM Studio's memory
};

// Turns user input such as "127.0.0.1:1234", "http://host:1234/" or "http://host:1234/v1" into the server's
// base URL. Returns an invalid QUrl if the input can't be a server address.
QUrl normalizeServerUrl(const QString &input);

// Talks to an LM Studio server.
class LmStudioClient : public QObject {
  Q_OBJECT

public:
  explicit LmStudioClient(QObject *parent = nullptr);

  // Fetches the models available for text generation (embedding models are left out), sorted by name.
  // Uses LM Studio's native REST API, falling back to older API versions and finally the OpenAI-compatible
  // endpoint. Emits exactly one of modelsFetched/fetchFailed, unless cancelled or superseded by another
  // fetch.
  void fetchModels(const QUrl &server);
  void cancel();

  bool isBusy() const
  {
    return m_reply != nullptr;
  }

signals:
  void modelsFetched(const QList<LmModel> &models);
  void fetchFailed(const QString &error);

private:
  enum class Api { NativeV1, NativeV0, OpenAi };

  void request(Api api);
  void onFinished(QNetworkReply *reply, Api api);

  QNetworkAccessManager *m_network = nullptr;
  QPointer<QNetworkReply> m_reply;
  QUrl m_server;
};
