#pragma once

#include "LmChat.h"
#include "PlanChat.h"

#include <QPointer>
#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTextBrowser;

// Chat box under the plan: shows the conversation about the selected plan, takes the author's messages and
// streams the model's reply. What to do with a finished reply (apply a revised plan, store the history) is
// up to the owner, which also builds the request.
class PlanChatPanel : public QWidget
{
    Q_OBJECT

  public:
    explicit PlanChatPanel(QWidget *parent = nullptr);

    void setHistory(const QList<PlanChat::Turn> &history);
    void setHeader(const QString &text);
    // Whether the author can send messages (a plan is selected, a model is known, nothing else is running).
    void setAvailable(bool available, const QString &reason = {});
    // A transient note shown after the conversation (not part of the history), e.g. an error.
    void showNote(const QString &note);

    void start(const QUrl &server, const QString &model, const QList<ChatMessage> &messages,
               const QString &userMessage);
    void stop(); // aborts; the message goes back into the input box
    bool isBusy() const
    {
        return m_request != nullptr;
    }

  signals:
    void submitted(const QString &message);
    void replied(const QString &userMessage, const QString &reply, bool truncated);
    void busyChanged(bool busy);
    void clearRequested();

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    void submit();
    void endRequest();
    void render();

    QLabel *m_header = nullptr;
    QTextBrowser *m_transcript = nullptr;
    QPlainTextEdit *m_input = nullptr;
    QPushButton *m_sendButton = nullptr;
    QPushButton *m_stopButton = nullptr;
    QPushButton *m_clearButton = nullptr;

    QList<PlanChat::Turn> m_history;
    QPointer<ChatCompletion> m_request;
    QString m_pendingMessage; // the author's message being answered
    QString m_streamed;       // the reply so far
    bool m_thinking = false;
    QString m_note;
    bool m_available = false;
    QString m_model;
};
