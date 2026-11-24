#include "PlanChatPanel.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace {

QString html(const QString &text)
{
    return text.toHtmlEscaped().replace('\n', QStringLiteral("<br>"));
}

} // namespace

PlanChatPanel::PlanChatPanel(QWidget *parent) : QWidget(parent)
{
    m_header = new QLabel(this);
    m_header->setObjectName("chatHeader");
    m_header->setWordWrap(true);
    m_clearButton = new QPushButton(tr("Clear"), this);
    m_clearButton->setObjectName("chatClearButton");
    m_clearButton->setToolTip(tr("Forget this plan's conversation"));

    m_transcript = new QTextBrowser(this);
    m_transcript->setObjectName("chatTranscript");
    m_transcript->setOpenLinks(false);

    m_input = new QPlainTextEdit(this);
    m_input->setObjectName("chatInput");
    m_input->setPlaceholderText(tr("Ask for changes, e.g. \"Give Act 2 a stronger midpoint\" (Enter sends, "
                                   "Shift+Enter for a new line)"));
    m_input->setMaximumHeight(m_input->fontMetrics().lineSpacing() * 4 + 12);
    m_input->installEventFilter(this);
    m_sendButton = new QPushButton(tr("Send"), this);
    m_sendButton->setObjectName("chatSendButton");
    m_stopButton = new QPushButton(tr("Stop"), this);
    m_stopButton->setObjectName("chatStopButton");
    m_stopButton->hide();

    auto *top = new QHBoxLayout;
    top->addWidget(m_header, 1);
    top->addWidget(m_clearButton);
    auto *inputRow = new QHBoxLayout;
    inputRow->addWidget(m_input, 1);
    auto *buttons = new QVBoxLayout;
    buttons->addWidget(m_sendButton);
    buttons->addWidget(m_stopButton);
    buttons->addStretch();
    inputRow->addLayout(buttons);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addLayout(top);
    layout->addWidget(m_transcript, 1);
    layout->addLayout(inputRow);

    connect(m_sendButton, &QPushButton::clicked, this, &PlanChatPanel::submit);
    connect(m_stopButton, &QPushButton::clicked, this, &PlanChatPanel::stop);
    connect(m_clearButton, &QPushButton::clicked, this, &PlanChatPanel::clearRequested);
    setAvailable(false);
}

bool PlanChatPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_input && event->type() == QEvent::KeyPress) {
        const auto *key = static_cast<QKeyEvent *>(event);
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) &&
            !(key->modifiers() & Qt::ShiftModifier)) {
            submit();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void PlanChatPanel::setHistory(const QList<PlanChat::Turn> &history)
{
    m_history = history;
    m_note.clear();
    render();
}

void PlanChatPanel::setHeader(const QString &text)
{
    m_header->setText(text);
}

void PlanChatPanel::setAvailable(bool available, const QString &reason)
{
    m_available = available;
    const bool canType = available && !isBusy();
    m_input->setEnabled(canType);
    m_sendButton->setEnabled(canType);
    m_clearButton->setEnabled(canType && !m_history.isEmpty());
    if (!available && !reason.isEmpty()) {
        m_input->setPlaceholderText(reason);
    } else {
        m_input->setPlaceholderText(tr("Ask for changes, e.g. \"Give Act 2 a stronger midpoint\" (Enter sends, "
                                       "Shift+Enter for a new line)"));
    }
}

void PlanChatPanel::showNote(const QString &note)
{
    m_note = note;
    render();
}

void PlanChatPanel::submit()
{
    const QString message = m_input->toPlainText().trimmed();
    if (message.isEmpty() || !m_available || isBusy()) {
        return;
    }
    emit submitted(message);
}

void PlanChatPanel::start(const QUrl &server, const QString &model, const QList<ChatMessage> &messages,
                          const QString &userMessage)
{
    m_model = model;
    m_pendingMessage = userMessage;
    m_streamed.clear();
    m_thinking = false;
    m_note.clear();
    m_input->clear();

    m_request = new ChatCompletion(server, model, messages, PlanChat::kTemperature, this);
    connect(m_request, &ChatCompletion::textChanged, this, [this](const QString &text) {
        m_streamed = text;
        m_thinking = false;
        render();
    });
    connect(m_request, &ChatCompletion::thinking, this, [this] {
        if (m_streamed.isEmpty() && !m_thinking) {
            m_thinking = true;
            render();
        }
    });
    connect(m_request, &ChatCompletion::finished, this, [this](const QString &reply, bool truncated) {
        const QString message = m_pendingMessage;
        endRequest();
        emit replied(message, reply, truncated); // the owner updates the history
    });
    connect(m_request, &ChatCompletion::failed, this, [this](const QString &error) {
        const QString message = m_pendingMessage;
        endRequest();
        m_input->setPlainText(message); // try again
        showNote(tr("Could not get an answer from %1: %2").arg(m_model, error));
    });

    m_stopButton->show();
    m_sendButton->hide();
    setAvailable(m_available);
    render();
    emit busyChanged(true);
    m_request->start();
}

void PlanChatPanel::stop()
{
    if (!m_request) {
        return;
    }
    m_request->abort();
    const QString message = m_pendingMessage;
    endRequest();
    m_input->setPlainText(message);
    showNote(tr("Stopped. Nothing was changed."));
}

void PlanChatPanel::endRequest()
{
    if (m_request) {
        m_request->deleteLater();
        m_request = nullptr;
    }
    m_pendingMessage.clear();
    m_streamed.clear();
    m_thinking = false;
    m_stopButton->hide();
    m_sendButton->show();
    setAvailable(m_available);
    render();
    emit busyChanged(false);
}

void PlanChatPanel::render()
{
    const QString you = tr("You");
    const QString model = tr("Model");
    QString out;
    const auto bubble = [&](const QString &who, const QString &body, const QString &extra = {}) {
        out += QStringLiteral("<p><b>%1:</b><br>%2%3</p>").arg(who.toHtmlEscaped(), body, extra);
    };
    for (const PlanChat::Turn &turn : m_history) {
        if (turn.role == "user") {
            bubble(you, html(turn.content));
        } else {
            QString body = html(turn.content);
            if (turn.planApplied) {
                body += (body.isEmpty() ? QString() : QStringLiteral("<br>")) +
                        QStringLiteral("<i>%1</i>").arg(tr("Plan updated. Ctrl+Z in the plan undoes it."));
            }
            bubble(model, body);
        }
    }
    if (isBusy()) {
        bubble(you, html(m_pendingMessage));
        int planWords = 0;
        const QString message = PlanChat::streamingMessage(m_streamed, &planWords);
        QString body = html(message);
        if (planWords > 0) {
            body += (body.isEmpty() ? QString() : QStringLiteral("<br>")) +
                    QStringLiteral("<i>%1</i>").arg(tr("Revising the plan... %n word(s) so far", nullptr, planWords));
        } else if (m_streamed.isEmpty()) {
            body = QStringLiteral("<i>%1</i>").arg(m_thinking ? tr("Thinking...") : tr("Waiting for the model..."));
        }
        bubble(m_model, body);
    }
    if (!m_note.isEmpty()) {
        out += QStringLiteral("<p><i>%1</i></p>").arg(html(m_note));
    }
    if (out.isEmpty()) {
        out = QStringLiteral("<p><i>%1</i></p>")
                  .arg(tr("Talk to the model about this plan: ask it to change, extend or tighten the plan, or "
                          "ask questions about it."));
    }
    m_transcript->setHtml(out);
    m_transcript->verticalScrollBar()->setValue(m_transcript->verticalScrollBar()->maximum());
}
