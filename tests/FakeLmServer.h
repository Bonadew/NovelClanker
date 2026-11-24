#pragma once

// Minimal fake LM Studio HTTP server for tests.

#include <QHash>
#include <QObject>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>

// Minimal HTTP/1.1 server: answers GET requests from a path -> (status, body) table, 404 otherwise.
class FakeServer : public QObject
{
  public:
    struct Response {
        int status = 200;
        QByteArray body;
    };

    QHash<QString, Response> routes;
    QStringList requested;
    bool silent = false; // accept connections but never answer (to test timeouts)

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
        if (!buffer.contains("\r\n\r\n") || silent) {
            return;
        }
        const QString path = QString::fromLatin1(buffer.split(' ').value(1));
        m_buffers.remove(socket);
        requested << path;

        const Response response = routes.value(path, Response{404, R"({"error":"Unexpected endpoint"})"});
        socket->write("HTTP/1.1 " + QByteArray::number(response.status) + " X\r\n" +
                      "Content-Type: application/json\r\nConnection: close\r\nContent-Length: " +
                      QByteArray::number(response.body.size()) + "\r\n\r\n" + response.body);
        socket->disconnectFromHost();
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
