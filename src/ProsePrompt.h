#pragma once

#include "LmChat.h"

#include <QList>
#include <QString>

// Prompts for the prose writer model: writing a whole scene from its brief, and expanding a passage the
// author wrote (a sketch, a paragraph, a scene draft) into a longer one of a chosen length.
namespace ProsePrompt {

// More creative than planning: prose benefits from varied word choice.
inline constexpr double kTemperature = 0.8;

// The lengths offered for writing and expanding, in words.
inline constexpr int kLengths[] = {200, 500, 1000, 2000};
inline constexpr int kDefaultLength = 500;

// Where the scene sits in the novel. Every field may be empty.
struct SceneContext {
  QString synopsis;
  QString actTitle;
  QString actBrief;
  QString chapterTitle;
  QString chapterBrief;
  QString sceneTitle;
  QString sceneBrief;
  QString previousText; // the end of the text that comes before the scene (the previous scene)
  QString nextBrief;    // the brief of the scene that follows, so the model does not run ahead into it
};

QString systemPrompt();

// Writes the scene from its brief, about `words` words long.
QList<ChatMessage> sceneMessages(const SceneContext &context, int words);

// Rewrites `passage` as a longer passage of about `words` words. `before` and `after` are the scene's text
// around it, so the result connects to both.
QList<ChatMessage> expandMessages(const SceneContext &context, const QString &before, const QString &passage,
                                  const QString &after, int words);

// The last / first `words` words of `text`, cut at a word boundary (the whole text if it is shorter).
QString lastWords(const QString &text, int words);
QString firstWords(const QString &text, int words);

// The most words accepted for a requested length. Models often overshoot, and some never stop; the app ends
// the request once the text passes this limit.
int wordLimit(int words);

// `text` cut to at most `maxWords` words, at the end of the last whole sentence (or at a word if no sentence
// ends in the second half of the allowed text). Unchanged if it is short enough.
QString trimToWords(const QString &text, int maxWords);

// Removes a repeat of the following text from an expanded passage. Models sometimes go on to rewrite
// <text_after> (or copy its first sentence), which would then appear twice: the answer is cut at the start of
// the sentence where a run of five words from `after` first appears, or a copy of its first sentence is
// removed from the end. The whole answer is never removed. Returns `text` unchanged if there is no repeat.
QString removeEcho(const QString &text, const QString &after);

// Removes what models wrap around prose: a code fence, a leading "Here is the scene:" line and a leading
// "# Scene title" heading. Works on partial (still streaming) replies too.
QString cleanProse(const QString &reply);

} // namespace ProsePrompt
