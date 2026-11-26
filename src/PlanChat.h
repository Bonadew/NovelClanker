#pragma once

#include "LmChat.h"

#include <QList>
#include <QString>

#include <optional>

// Conversation with a model about refining a plan.
//
// Each request sends the synopsis and the *current* plan (including the author's own edits) in the system
// prompt, followed by the conversation. When asked for changes the model answers with a short summary and the
// complete revised plan between "<plan>" and "</plan>" lines; questions get a plain answer.
namespace PlanChat {

inline constexpr double kTemperature = 0.5;

struct Turn {
  QString role;             // "user" or "assistant"
  QString content;          // for the assistant: the message without the revised plan
  bool planApplied = false; // the assistant's revised plan was applied to the plan
};

// History persistence (JSON array). Invalid input yields an empty history.
QString serialize(const QList<Turn> &history);
QList<Turn> parse(const QString &json);

QString systemPrompt(const QString &synopsis, const QString &plan);
QList<ChatMessage> messages(const QString &synopsis, const QString &plan, const QList<Turn> &history,
                            const QString &userMessage);

struct Reply {
  QString message;             // the reply without the plan block
  std::optional<QString> plan; // complete revised plan, if the reply contained one
  bool planCutOff = false;     // a plan block was started but not finished (not usable)
};

Reply parseReply(const QString &reply);

// Models sometimes return only the changed part of a plan despite being asked for all of it. Returns why
// `revised` looks like a partial copy of `current` (fewer acts, far fewer chapters, much shorter), or an
// empty string if it looks complete.
QString incompleteReason(const QString &current, const QString &revised);

// What to show while a reply streams in: the message so far, without a (partial) plan block.
// `planWords` receives the number of words of the plan block written so far (0 if none started).
QString streamingMessage(const QString &partialReply, int *planWords = nullptr);

} // namespace PlanChat
