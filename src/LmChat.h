#pragma once

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

struct ChatMessage {
    QString role; // "system", "user" or "assistant"
    QString content;
};

// Removes reasoning ("thinking") from model output: complete <think>...</think> blocks, an unfinished
// <think> block at the end (the model is still thinking), and a partial "<think" tag at the very end of a
// stream. Leading whitespace left behind is trimmed.
QString stripReasoning(const QString &text);

// One streamed chat completion against an LM Studio (OpenAI-compatible) server:
// POST <server>/v1/chat/completions with "stream": true.
//
// Emits textChanged() with the full visible answer so far (reasoning removed) as tokens arrive, then exactly
// one of finished() or failed(), unless abort() is called first (then nothing more is emitted).
class ChatCompletion : public QObject
{
    Q_OBJECT

  public:
    ChatCompletion(const QUrl &server, const QString &model, const QList<ChatMessage> &messages,
                   double temperature, QObject *parent = nullptr);

    void start();
    void abort();

    bool isRunning() const
    {
        return m_reply != nullptr;
    }

  signals:
    void textChanged(const QString &visibleText);
    void thinking(); // the model is producing reasoning tokens (nothing visible yet)
    // `truncated` is true when the model stopped because it hit its token/context limit.
    void finished(const QString &text, bool truncated);
    void failed(const QString &error);

  private:
    void onReadyRead();
    void onFinished();
    void handleEvent(const QByteArray &data);

    QNetworkAccessManager *m_network;
    QPointer<QNetworkReply> m_reply;
    QUrl m_server;
    QString m_model;
    QList<ChatMessage> m_messages;
    double m_temperature;

    QByteArray m_buffer;  // unparsed stream bytes
    QString m_raw;        // all content tokens so far (may include <think> blocks)
    QString m_error;      // error reported inside the stream
    QString m_finishReason;
    bool m_done = false;  // saw "data: [DONE]"
};
