#pragma once

// Minimal fake LM Studio HTTP server for tests.

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>

// Minimal HTTP/1.1 server: answers requests from a path -> response table, 404 otherwise.
class FakeServer : public QObject {
public:
  struct Response {
    int status = 200;
    QByteArray body;
    QByteArray contentType = "application/json";
    bool keepOpen = false; // send the body but keep the connection open (an unfinished stream)
  };

  QHash<QString, Response> routes;
  QStringList requested;      // request paths, in order
  QByteArray lastRequestBody; // body of the most recent request that had one
  bool silent = false;        // accept connections but never answer (to test timeouts)

  FakeServer()
  {
    m_server.listen(QHostAddress::LocalHost);
    connect(&m_server, &QTcpServer::newConnection, this, [this] {
      while (QTcpSocket *socket = m_server.nextPendingConnection()) {
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { serve(socket); });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
      }
    });
  }

  QUrl url(const QString &path = {}) const
  {
    return QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(m_server.serverPort()).arg(path));
  }

private:
  void serve(QTcpSocket *socket)
  {
    QByteArray &buffer = m_buffers[socket];
    buffer += socket->readAll();
    const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
    if (headerEnd < 0 || silent) {
      return;
    }
    // Wait for the whole body (Content-Length).
    qsizetype contentLength = 0;
    for (const QByteArray &line : buffer.left(headerEnd).split('\n')) {
      if (line.toLower().startsWith("content-length:")) {
        contentLength = line.mid(15).trimmed().toLongLong();
      }
    }
    if (buffer.size() < headerEnd + 4 + contentLength) {
      return;
    }
    const QString path = QString::fromLatin1(buffer.split(' ').value(1));
    if (contentLength > 0) {
      lastRequestBody = buffer.mid(headerEnd + 4, contentLength);
    }
    m_buffers.remove(socket);
    requested << path;

    const Response response = routes.value(path, Response{404, R"({"error":"Unexpected endpoint"})"});
    QByteArray head = "HTTP/1.1 " + QByteArray::number(response.status) +
                      " X\r\nContent-Type: " + response.contentType + "\r\nConnection: close\r\n";
    if (!response.keepOpen) {
      head += "Content-Length: " + QByteArray::number(response.body.size()) + "\r\n";
    }
    socket->write(head + "\r\n" + response.body);
    if (!response.keepOpen) {
      socket->disconnectFromHost();
    }
  }

  QTcpServer m_server;
  QHash<QTcpSocket *, QByteArray> m_buffers;
};

// A typical LM Studio /api/v1/models answer: two LLMs (one loaded) and an embedding model.
inline const QByteArray kSampleNativeModels = R"({"models": [
    {"type": "llm", "key": "zeta-7b", "display_name": "Zeta 7B", "params_string": "7B",
     "quantization": {"name": "Q4_K_M"}, "loaded_instances": []},
    {"type": "embedding", "key": "nomic-embed", "display_name": "Nomic Embed", "loaded_instances": []},
    {"type": "llm", "key": "alpha-27b", "display_name": "alpha 27B", "params_string": null,
     "quantization": {"name": "IQ2_S"}, "loaded_instances": [{"id": "alpha-27b"}]}
]})";

// Builds an OpenAI-style streamed chat completion (server-sent events) from content pieces.
inline QByteArray sseStream(const QStringList &pieces, const QString &finishReason = "stop", bool done = true,
                            const QStringList &reasoningPieces = {})
{
  QByteArray out;
  const auto event = [&out](const QJsonObject &delta, const QJsonValue &finish) {
    const QJsonObject choice{{"index", 0}, {"delta", delta}, {"finish_reason", finish}};
    out += "data: " +
           QJsonDocument(QJsonObject{{"object", "chat.completion.chunk"}, {"choices", QJsonArray{choice}}})
               .toJson(QJsonDocument::Compact) +
           "\n\n";
  };
  for (const QString &r : reasoningPieces) {
    event(QJsonObject{{"reasoning_content", r}}, QJsonValue::Null);
  }
  for (const QString &piece : pieces) {
    event(QJsonObject{{"content", piece}}, QJsonValue::Null);
  }
  if (!finishReason.isEmpty()) {
    event(QJsonObject{}, finishReason);
  }
  if (done) {
    out += "data: [DONE]\n\n";
  }
  return out;
}
