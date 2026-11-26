#include "ProsePrompt.h"

#include <QRegularExpression>
#include <QSet>
#include <QStringList>

#include <algorithm>

namespace ProsePrompt {

namespace {

const QString kGuidelines = QStringLiteral(
    R"PROMPT(Guidelines:
- Write finished prose for the novel, not an outline or notes. Show, don't tell: concrete sensory detail, action, dialogue and the point-of-view character's thoughts.
- Keep the point of view, tense, voice and style of the surrounding text. If there is none, write in close third person, past tense.
- Stay faithful to the brief and the synopsis. Don't add major events, characters or revelations they don't call for, and don't resolve what later scenes are meant to resolve.
- Follow the instructions closely and stop once they are covered. Don't continue past what was asked for, and don't end with foreshadowing, a summary, a reflection or an imagined ending beyond it.
- Write in the same language as the text, the brief and the synopsis.
- Write plain text only: no Markdown, HTML or code blocks (even where the brief or the plan uses them), no title or heading, no introduction, and no comments or notes after the prose.)PROMPT");

QString section(const QString &tag, const QString &content)
{
  return content.trimmed().isEmpty() ? QString()
                                     : "<" + tag + ">\n" + content.trimmed() + "\n</" + tag + ">\n\n";
}

QString titled(const QString &title, const QString &brief)
{
  return brief.trimmed().isEmpty() ? title.trimmed() : title.trimmed() + "\n" + brief.trimmed();
}

// Everything the model should know about where the scene sits.
QString contextBlock(const SceneContext &c)
{
  return section(QStringLiteral("synopsis"), c.synopsis) +
         section(QStringLiteral("act"), titled(c.actTitle, c.actBrief)) +
         section(QStringLiteral("chapter"), titled(c.chapterTitle, c.chapterBrief)) +
         section(QStringLiteral("scene"), titled(c.sceneTitle, c.sceneBrief)) +
         section(QStringLiteral("previous_text"), c.previousText) +
         section(QStringLiteral("next_scene_brief"), c.nextBrief);
}

// Rough paragraph guidance helps models hit a length they can't count.
QString lengthHint(int words)
{
  const int paragraphs = std::max(1, words / 120);
  return paragraphs == 1
             ? QStringLiteral("about %1 words (one or two paragraphs)").arg(words)
             : QStringLiteral("about %1 words (roughly %2 paragraphs)").arg(words).arg(paragraphs);
}

const QRegularExpression &wordPattern()
{
  static const QRegularExpression re(QStringLiteral(R"(\S+)"));
  return re;
}

// A sentence end: . ! ? or … possibly followed by closing quotes or brackets.
const QString kSentenceEnd = QStringLiteral(R"([.!?…]["'”’»)\]]*)");

QString firstSentence(const QString &text)
{
  static const QRegularExpression re(QStringLiteral(R"(^\s*(.+?%1)(?=\s|$))").arg(kSentenceEnd),
                                     QRegularExpression::DotMatchesEverythingOption);
  const auto m = re.match(text);
  return (m.hasMatch() ? m.captured(1) : text).simplified();
}

// Words for comparing texts: lowercase letters and digits, with their positions in the text.
struct Word {
  QString text;
  qsizetype start;
};

QList<Word> comparableWords(const QString &text)
{
  static const QRegularExpression re(QStringLiteral(R"([\p{L}\p{N}]+)"),
                                     QRegularExpression::UseUnicodePropertiesOption);
  QList<Word> words;
  for (auto it = re.globalMatch(text); it.hasNext();) {
    const auto m = it.next();
    words.append({m.captured().toLower(), m.capturedStart()});
  }
  return words;
}

} // namespace

QString systemPrompt()
{
  return QStringLiteral(
      "You are a skilled novelist writing a novel together with its author. Your sole purpose is "
      "to write punchy, engaging prose that follows the author's instructions exactly and stays "
      "consistent with the novel's context, the story so far and the voice of what is already "
      "written.");
}

QList<ChatMessage> sceneMessages(const SceneContext &context, int words)
{
  QString user = contextBlock(context);
  user += QStringLiteral("Write the scene described in <scene>. ");
  if (!context.previousText.trimmed().isEmpty()) {
    user += QStringLiteral("It follows directly after <previous_text>; continue the story from there without "
                           "repeating it. ");
  }
  if (!context.nextBrief.trimmed().isEmpty()) {
    user += QStringLiteral("End the scene before the events in <next_scene_brief>. ");
  }
  user += "\n\n" + kGuidelines;
  user +=
      QStringLiteral("\n\nLength: %1, no more. Stop as soon as the scene in <scene> is covered; you don't "
                     "need to reach the word count if the scene is already complete.")
          .arg(lengthHint(words));
  return {{QStringLiteral("system"), systemPrompt()}, {QStringLiteral("user"), user}};
}

QList<ChatMessage> expandMessages(const SceneContext &context, const QString &before, const QString &passage,
                                  const QString &after, int words)
{
  QString user = contextBlock(context);
  user += section(QStringLiteral("text_before"), before) + section(QStringLiteral("passage"), passage) +
          section(QStringLiteral("text_after"), after);
  user += QStringLiteral(
      "Rewrite <passage> as a longer, fuller passage. Keep everything that happens in it, in the "
      "same order, and keep the author's lines of dialogue and best phrases. Expand it with "
      "detail, dialogue, action and the character's inner life, not with new plot: the "
      "passage covers only the moment it describes, so end where it ends and never continue "
      "the story beyond it.");
  if (!before.trimmed().isEmpty()) {
    user += QStringLiteral(" It must follow on from <text_before> without repeating it.");
  }
  if (!after.trimmed().isEmpty()) {
    user +=
        QStringLiteral(" <text_after> is already written and comes right after your passage; it stays as it "
                       "is. Never rewrite, summarise or include any part of it, and don't write any of the "
                       "events it describes: your passage must end before its first sentence, \"%1\"")
            .arg(firstSentence(after));
  }
  user += "\n\n" + kGuidelines;
  user += QStringLiteral("\n\nLength: %1, no more. Stop as soon as the passage's last moment is written.")
              .arg(lengthHint(words));
  return {{QStringLiteral("system"), systemPrompt()}, {QStringLiteral("user"), user}};
}

QString lastWords(const QString &text, int words)
{
  QList<qsizetype> starts;
  for (auto it = wordPattern().globalMatch(text); it.hasNext();) {
    starts.append(it.next().capturedStart());
  }
  if (words <= 0) {
    return {};
  }
  if (starts.size() <= words) {
    return text.trimmed();
  }
  return text.mid(starts[starts.size() - words]).trimmed();
}

QString firstWords(const QString &text, int words)
{
  if (words <= 0) {
    return {};
  }
  int n = 0;
  for (auto it = wordPattern().globalMatch(text); it.hasNext();) {
    const auto m = it.next();
    if (++n == words) {
      return text.left(m.capturedEnd()).trimmed();
    }
  }
  return text.trimmed();
}

int wordLimit(int words)
{
  return words + std::max(words / 4, 50);
}

QString trimToWords(const QString &text, int maxWords)
{
  qsizetype cut = -1;
  int n = 0;
  for (auto it = wordPattern().globalMatch(text); it.hasNext();) {
    const auto m = it.next();
    if (++n == maxWords) {
      cut = m.capturedEnd();
    } else if (n > maxWords) {
      break;
    }
  }
  if (n <= maxWords || cut < 0) {
    return text;
  }
  const QString head = text.left(cut);
  // The last sentence end: . ! ? or … possibly followed by closing quotes or brackets.
  static const QRegularExpression sentenceEnd(QStringLiteral(R"([.!?…]["'”’»)\]]*(?=\s|$))"));
  qsizetype end = -1;
  for (auto it = sentenceEnd.globalMatch(head); it.hasNext();) {
    end = it.next().capturedEnd();
  }
  return (end > head.size() / 2 ? head.left(end) : head).trimmed();
}

QString removeEcho(const QString &text, const QString &after)
{
  if (after.trimmed().isEmpty()) {
    return text;
  }
  // A rewrite of the following text: from the sentence where a run of words from it first appears.
  constexpr int kRun = 5;
  const QList<Word> afterWords = comparableWords(after);
  QSet<QString> runs;
  const auto runAt = [](const QList<Word> &words, qsizetype i) {
    QStringList parts;
    for (qsizetype k = i; k < i + kRun; ++k) {
      parts << words[k].text;
    }
    return parts.join(' ');
  };
  for (qsizetype i = 0; i + kRun <= afterWords.size(); ++i) {
    runs.insert(runAt(afterWords, i));
  }
  const QList<Word> words = comparableWords(text);
  for (qsizetype i = 0; i + kRun <= words.size(); ++i) {
    if (!runs.contains(runAt(words, i))) {
      continue;
    }
    // Back to the start of the sentence (or paragraph) the run is in.
    static const QRegularExpression boundary(QStringLiteral(R"((?:%1\s|\n))").arg(kSentenceEnd));
    qsizetype cut = 0;
    for (auto it = boundary.globalMatch(text.left(words[i].start)); it.hasNext();) {
      cut = it.next().capturedEnd();
    }
    const QString kept = text.left(cut).trimmed();
    return kept.isEmpty() ? text : kept; // never throw away the whole answer
  }

  // The following text's first sentence copied at the end, too short for a run of words: "The phone rang."
  const QString sentence = firstSentence(after);
  const QString trimmed = text.trimmed();
  if (sentence.size() < 8 || !trimmed.endsWith(sentence) || trimmed.size() == sentence.size()) {
    return text; // short sentences could match by coincidence
  }
  return trimmed.chopped(sentence.size()).trimmed();
}

QString cleanProse(const QString &reply)
{
  QString text = reply;
  // Leading blank lines; trailing whitespace (also while streaming: the next token brings it back if needed).
  static const QRegularExpression leadingBlank(QStringLiteral(R"(^(?:[ \t]*\r?\n)+)"));
  text.remove(leadingBlank);
  while (!text.isEmpty() && text.back().isSpace()) {
    text.chop(1);
  }

  static const QRegularExpression fenceOpen(QStringLiteral(R"(^```[A-Za-z]*[ \t]*(?:\r?\n|$))"));
  static const QRegularExpression fenceClose(QStringLiteral(R"((?:^|\n)```\s*$)"));
  static const QRegularExpression preamble(
      QStringLiteral(
          R"(^(?:here is|here's|here are|sure|certainly|of course|below is|okay|ok)\b[^\n]*:[ \t]*(?:\r?\n|$))"),
      QRegularExpression::CaseInsensitiveOption);
  static const QRegularExpression heading(QStringLiteral(R"(^#{1,6}\s[^\n]*(?:\r?\n|$))"));
  static const QRegularExpression boldTitle(QStringLiteral(R"(^\*\*[^*\n]{1,80}\*\*[ \t]*\r?\n)"));
  const QString tags = QStringLiteral(
      "</?(?:synopsis|act|chapter|scene|previous_text|next_scene_brief|text_before|passage|text_after)>");
  static const QRegularExpression leadingTag(QStringLiteral(R"(^[ \t]*%1[ \t]*(?:\r?\n|$))").arg(tags));
  static const QRegularExpression laterTag(tags);
  static const QRegularExpression partialTag(QStringLiteral(R"(<\/?[a-z_]*$)"));
  bool changed = true;
  while (changed) {
    const QString before = text;
    if (text.contains(fenceOpen)) {
      text.remove(fenceOpen);
      text.remove(fenceClose);
    }
    text.remove(preamble);
    text.remove(heading);
    text.remove(boldTitle);
    text.remove(leadingTag);
    text.remove(leadingBlank);
    changed = text != before;
  }
  // A section tag from the prompt later in the answer ("</passage>", "<text_after>") means the model has
  // moved on to copying the prompt: everything from there is dropped. A tag still arriving at the end is
  // hidden.
  if (const auto m = laterTag.match(text); m.hasMatch()) {
    text.truncate(m.capturedStart());
  }
  text.remove(partialTag);
  while (!text.isEmpty() && text.back().isSpace()) {
    text.chop(1);
  }
  return text;
}

} // namespace ProsePrompt
