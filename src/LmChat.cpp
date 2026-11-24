#include "LmChat.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>

namespace {

// Generous inactivity timeout: LM Studio may first have to load the model, which can take minutes.
constexpr int kInactivityTimeoutMs = 5 * 60 * 1000;

QString errorMessage(const QJsonValue &error)
{
    if (error.isString()) {
        return error.toString();
    }
    const QJsonObject obj = error.toObject();
    return obj.value("message").toString(QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
}

} // namespace

QString stripReasoning(const QString &text)
{
    static const QString openTag = QStringLiteral("<think>");
    static const QString closeTag = QStringLiteral("</think>");
    static const QRegularExpression block(QStringLiteral("<think>.*?</think>"),
                                          QRegularExpression::DotMatchesEverythingOption);
    QString out = text;
    out.remove(block);
    // Some chat templates open the think block in the prompt, so the output starts with reasoning and only
    // contains the closing tag.
    if (const qsizetype close = out.indexOf(closeTag); close >= 0) {
        out.remove(0, close + closeTag.size());
    }
    if (const qsizetype open = out.indexOf(openTag); open >= 0) {
        out.truncate(open); // still thinking
    }
    for (qsizetype n = openTag.size() - 1; n > 0; --n) {
        if (out.endsWith(QStringView(openTag).left(n))) {
            out.chop(n); // tag still arriving
            break;
        }
    }
    qsizetype start = 0;
    while (start < out.size() && out.at(start).isSpace()) {
        ++start;
    }
    return out.mid(start);
}

ChatCompletion::ChatCompletion(const QUrl &server, const QString &model, const QList<ChatMessage> &messages,
                               double temperature, QObject *parent)
    : QObject(parent), m_network(new QNetworkAccessManager(this)), m_server(server), m_model(model),
      m_messages(messages), m_temperature(temperature)
{
}

void ChatCompletion::start()
{
    QJsonArray messages;
    for (const ChatMessage &m : m_messages) {
        messages.append(QJsonObject{{"role", m.role}, {"content", m.content}});
    }
    const QJsonObject body{
        {"model", m_model}, {"messages", messages}, {"temperature", m_temperature}, {"stream", true}};

    QUrl url = m_server;
    url.setPath(m_server.path() + "/v1/chat/completions");
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Accept", "text/event-stream");
    request.setTransferTimeout(kInactivityTimeoutMs);

    m_reply = m_network->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(m_reply, &QNetworkReply::readyRead, this, &ChatCompletion::onReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &ChatCompletion::onFinished);
}

void ChatCompletion::abort()
{
    if (QNetworkReply *reply = m_reply) {
        m_reply = nullptr; // stale: onFinished() will ignore it
        reply->abort();
        reply->deleteLater();
    }
}

void ChatCompletion::onReadyRead()
{
    if (!m_reply) {
        return;
    }
    m_buffer += m_reply->readAll();
    if (m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
        return; // keep the body for the error message
    }
    // Server-sent events: "data: {...}" lines separated by blank lines.
    qsizetype newline;
    while ((newline = m_buffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_buffer.left(newline).trimmed();
        m_buffer.remove(0, newline + 1);
        if (line.startsWith("data:")) {
            handleEvent(line.mid(5).trimmed());
        }
    }
}

void ChatCompletion::handleEvent(const QByteArray &data)
{
    if (data == "[DONE]") {
        m_done = true;
        return;
    }
    const QJsonObject event = QJsonDocument::fromJson(data).object();
    if (event.contains("error")) {
        m_error = errorMessage(event.value("error"));
        return;
    }
    const QJsonObject choice = event.value("choices").toArray().at(0).toObject();
    const QJsonObject delta = choice.value("delta").toObject();
    if (const QJsonValue reason = choice.value("finish_reason"); reason.isString()) {
        m_finishReason = reason.toString();
    }
    // LM Studio can deliver reasoning separately from the answer.
    if (!delta.value("reasoning_content").toString().isEmpty() || !delta.value("reasoning").toString().isEmpty()) {
        emit thinking();
    }
    const QString content = delta.value("content").toString();
    if (content.isEmpty()) {
        return;
    }
    const QString before = stripReasoning(m_raw);
    m_raw += content;
    const QString after = stripReasoning(m_raw);
    if (after != before) {
        emit textChanged(after);
    } else if (after.isEmpty()) {
        emit thinking(); // inline <think> block
    }
}

void ChatCompletion::onFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    if (!reply || reply != m_reply) {
        return; // aborted
    }
    reply->deleteLater();
    onReadyRead(); // anything still buffered
    m_reply = nullptr;

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 0) {
        emit failed(reply->error() == QNetworkReply::OperationCanceledError
                        ? tr("The server stopped responding.")
                        : reply->errorString());
        return;
    }
    if (status != 200) {
        const QJsonObject body = QJsonDocument::fromJson(m_buffer).object();
        const QString detail = body.contains("error") ? errorMessage(body.value("error"))
                                                      : QString::fromUtf8(m_buffer.left(300)).trimmed();
        emit failed(tr("The server answered with HTTP status %1: %2").arg(status).arg(detail));
        return;
    }
    // A last event without a trailing newline.
    if (const QByteArray line = m_buffer.trimmed(); line.startsWith("data:")) {
        handleEvent(line.mid(5).trimmed());
    }
    if (!m_error.isEmpty()) {
        emit failed(m_error);
        return;
    }
    if (reply->error() != QNetworkReply::NoError && !m_done && m_finishReason.isEmpty()) {
        emit failed(tr("The connection was lost: %1").arg(reply->errorString()));
        return;
    }
    QString text = stripReasoning(m_raw);
    while (!text.isEmpty() && text.back().isSpace()) {
        text.chop(1);
    }
    emit finished(text, m_finishReason == "length");
}
