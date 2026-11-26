// Tests for the prose writer prompts and reply cleanup.

#include "ProsePrompt.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>

#include <cstdio>
#include <cstdlib>

namespace {

int g_failures = 0;

void expect(bool ok, const char *what, int line)
{
  if (!ok) {
    std::fprintf(stderr, "FAIL (line %d): %s\n", line, what);
    ++g_failures;
  }
}

#define EXPECT(cond) expect((cond), #cond, __LINE__)

using namespace ProsePrompt;

SceneContext sampleContext()
{
  SceneContext c;
  c.synopsis = "Elin, a retired cartographer, slowly stops leaving her harbour town.";
  c.actTitle = "Act 1: Harbour Lights";
  c.chapterTitle = "Chapter 2: Tide Tables";
  c.chapterBrief = "Elin's routines tighten.";
  c.sceneTitle = "Ink";
  c.sceneBrief = "POV: Elin. She redraws the harbour map from memory and finds a street missing.";
  c.previousText = "The kettle had gone cold again.";
  c.nextBrief = "Her daughter calls about a visit.";
  return c;
}

// @relation(FR-PRS-3, AI-11, AI-12, AI-13, scope=function)
void testSceneMessages()
{
  const auto messages = sceneMessages(sampleContext(), 1000);
  EXPECT(messages.size() == 2);
  EXPECT(messages[0].role == "system" && messages[1].role == "user");
  const QString &user = messages[1].content;
  EXPECT(user.contains("<synopsis>\nElin, a retired cartographer"));
  EXPECT(user.contains("<chapter>\nChapter 2: Tide Tables\nElin's routines tighten.\n</chapter>"));
  EXPECT(user.contains("<scene>\nInk\nPOV: Elin. She redraws the harbour map"));
  EXPECT(user.contains("<previous_text>\nThe kettle had gone cold again.\n</previous_text>"));
  EXPECT(user.contains("<next_scene_brief>\nHer daughter calls about a visit."));
  EXPECT(user.contains("about 1000 words"));
  EXPECT(user.contains("continue the story from there"));
  EXPECT(user.contains("End the scene before the events in <next_scene_brief>"));
  EXPECT(user.contains("plain text only") && user.contains("don't end with foreshadowing"));
  EXPECT(user.contains("you don't need to reach the word count"));

  // Empty parts are left out entirely.
  SceneContext bare;
  bare.sceneBrief = "She leaves.";
  const QString minimal = sceneMessages(bare, 200)[1].content;
  EXPECT(!minimal.contains("<synopsis>") && !minimal.contains("<previous_text>") &&
         !minimal.contains("<next_scene_brief>") && !minimal.contains("continue the story"));
  EXPECT(minimal.contains("<scene>\nShe leaves.\n</scene>"));
  EXPECT(minimal.contains("about 200 words (one or two paragraphs)"));
}

// @relation(FR-PRS-6, AI-11, scope=function)
void testExpandMessages()
{
  const auto messages = expandMessages(sampleContext(), "She sat down.",
                                       "She opens the map. A street is gone.", "Then the phone rang.", 500);
  const QString &user = messages[1].content;
  EXPECT(user.contains("<text_before>\nShe sat down.\n</text_before>"));
  EXPECT(user.contains("<passage>\nShe opens the map. A street is gone.\n</passage>"));
  EXPECT(user.contains("<text_after>\nThen the phone rang.\n</text_after>"));
  EXPECT(user.contains("about 500 words"));
  EXPECT(user.contains("follow on from <text_before>") && user.contains("<text_after> is already written"));

  const QString alone = expandMessages({}, "", "A sketch.", "", 200)[1].content;
  EXPECT(!alone.contains("<text_before>") && !alone.contains("<text_after>"));
  EXPECT(!alone.contains("follow on from") && !alone.contains("lead into"));
}

// @relation(FR-PRS-3, FR-PRS-6, scope=function)
void testWords()
{
  const QString text = "one two  three\nfour five";
  EXPECT(lastWords(text, 2) == "four five");
  EXPECT(lastWords(text, 3) == "three\nfour five");
  EXPECT(lastWords(text, 10) == text);
  EXPECT(lastWords(text, 0).isEmpty());
  EXPECT(firstWords(text, 3) == "one two  three");
  EXPECT(firstWords(text, 10) == text);
  EXPECT(firstWords("  ", 3).isEmpty());
}

// @relation(FR-PRS-10, AI-13, scope=function)
void testLengthLimit()
{
  EXPECT(wordLimit(200) == 250);
  EXPECT(wordLimit(500) == 625);
  EXPECT(wordLimit(2000) == 2500);

  // The length comes last in the prompt, where small models heed it most.
  const QString expand = expandMessages({}, "", "A sketch.", "", 200)[1].content;
  EXPECT(expand.trimmed().endsWith("Stop as soon as the passage's last moment is written."));
  EXPECT(expand.contains("Length: about 200 words (one or two paragraphs), no more."));
  EXPECT(expand.contains("never continue the story beyond it"));
  const QString scene = sceneMessages(sampleContext(), 500)[1].content;
  EXPECT(scene.contains("Length: about 500 words (roughly 4 paragraphs), no more."));

  EXPECT(trimToWords("One two three.", 5) == "One two three.");
  // Cut at the last whole sentence within the limit.
  EXPECT(trimToWords("One two three. Four five six. Seven eight nine.", 7) ==
         "One two three. Four five six.");
  EXPECT(trimToWords("He said \"stop.\" Then he ran off into the night.", 5) == "He said \"stop.\"");
  // No sentence ends in the second half: cut at the word.
  EXPECT(trimToWords("Hi. one two three four five six seven", 6) == "Hi. one two three four five");
}

// @relation(FR-PRS-6, FR-PRS-11, scope=function)
void testRemoveEcho()
{
  const QString after = "The phone rang in the hall. Nobody answered.";
  EXPECT(removeEcho("She put the pen down.\n\nThe phone rang in the hall.", after) ==
         "She put the pen down.");
  EXPECT(removeEcho("She put the pen down.", after) == "She put the pen down.");
  EXPECT(removeEcho("She put the pen down.", "") == "She put the pen down.");
  EXPECT(removeEcho("Then: Go.", "Go.") == "Then: Go."); // too short to be sure

  // The model expands the passage and then goes on to rewrite the paragraph that follows it.
  const QString next =
      "In the hallway, Elin pulled on her coat like a sleepwalker. The front door stuck as it always "
      "did, and she shouldered it open. Outside, the gulls were already at the bins.";
  const QString expanded =
      "Elin woke at six, as she had for forty years. Grey light crept over the quilt.\n\n"
      "In the hallway, Elin pulled on her coat with slow care. The front door stuck again.";
  EXPECT(removeEcho(expanded, next) ==
         "Elin woke at six, as she had for forty years. Grey light crept over the quilt.");
  // From the sentence where the repeat starts, also inside a paragraph.
  EXPECT(removeEcho("Elin woke at six. She sat up. The front door stuck as it always did, of course.",
                    next) == "Elin woke at six. She sat up.");
  // Case and punctuation don't hide a repeat.
  EXPECT(removeEcho("Elin woke. THE FRONT DOOR -- stuck, as it always did!", next) == "Elin woke.");
  // Shared words without a run of five are not a repeat.
  EXPECT(removeEcho("Elin woke at six and pulled the quilt up. Outside, gulls cried.", next) ==
         "Elin woke at six and pulled the quilt up. Outside, gulls cried.");
  // An answer that is nothing but a repeat is left alone rather than emptied.
  EXPECT(removeEcho("In the hallway, Elin pulled on her coat.", next) ==
         "In the hallway, Elin pulled on her coat.");

  // The prompt names the sentence the passage must end before.
  const QString prompt = expandMessages({}, "", "Elin woke at six.", next, 200)[1].content;
  EXPECT(
      prompt.contains("must end before its first sentence, \"In the hallway, Elin pulled on her coat like a "
                      "sleepwalker.\""));
  EXPECT(prompt.contains("Never rewrite, summarise or include any part of it"));
}

// @relation(FR-PRS-12, scope=function)
void testCleanProse()
{
  EXPECT(cleanProse("The door creaked.\n\nShe went in.") == "The door creaked.\n\nShe went in.");
  EXPECT(cleanProse("Here is the scene:\n\nThe door creaked.") == "The door creaked.");
  EXPECT(cleanProse("Sure! Here's the expanded passage:\nThe door creaked.") == "The door creaked.");
  EXPECT(cleanProse("# Ink\n\nThe door creaked.") == "The door creaked.");
  EXPECT(cleanProse("**Scene 1: Ink**\n\nThe door creaked.") == "The door creaked.");
  EXPECT(cleanProse("```\nThe door creaked.\n```") == "The door creaked.");
  EXPECT(cleanProse("```markdown\n# Ink\nThe door creaked.") == "The door creaked."); // still streaming
  EXPECT(cleanProse("\n\nThe door creaked.  \n") == "The door creaked.");
  // Prose that merely starts like a preamble stays.
  EXPECT(cleanProse("Sure enough, the door creaked.") == "Sure enough, the door creaked.");
  EXPECT(cleanProse("She said: \"Here.\"") == "She said: \"Here.\"");
  // Emphasis inside the prose stays.
  EXPECT(cleanProse("**Bang.** The door slammed.") == "**Bang.** The door slammed.");
  // Section tags copied from the prompt: a leading one is removed, anything from a later one is dropped.
  EXPECT(cleanProse("<passage>\nThe door creaked.\n</passage>") == "The door creaked.");
  EXPECT(cleanProse("The door creaked.\n<text_after>\nIn the hallway...") == "The door creaked.");
  EXPECT(cleanProse("The door creaked.\n\n<text_af") == "The door creaked."); // still streaming
  EXPECT(cleanProse("She wrote a < b on the board.") == "She wrote a < b on the board.");
}

// Optional live check against LM Studio: NOVELCLANKER_LIVE_LMSTUDIO=<server>
// NOVELCLANKER_LIVE_PROSE_MODEL=<model> expands a one-sentence sketch to each length (and writes a scene from
// a brief) and prints the word counts.
// @relation(NFR-12, scope=function)
void liveLengths()
{
  const QString server = qEnvironmentVariable("NOVELCLANKER_LIVE_LMSTUDIO");
  const QString model = qEnvironmentVariable("NOVELCLANKER_LIVE_PROSE_MODEL");
  if (server.isEmpty() || model.isEmpty()) {
    return;
  }
  static const QRegularExpression ws(QStringLiteral(R"(\s+)"));
  const auto words = [](const QString &t) { return t.split(ws, Qt::SkipEmptyParts).size(); };
  // Like the app: a model still writing past wordLimit() is stopped (reported as "capped").
  const auto run = [&](const QList<ChatMessage> &messages, int target) {
    ChatCompletion chat(QUrl(server), model, messages, kTemperature);
    QEventLoop loop;
    QString result;
    QObject::connect(&chat, &ChatCompletion::textChanged, &loop, [&](const QString &text) {
      if (words(cleanProse(text)) > wordLimit(target)) {
        chat.abort();
        result = trimToWords(cleanProse(text), wordLimit(target));
        std::printf("  (capped: the model was still writing)\n");
        loop.quit();
      }
    });
    QObject::connect(&chat, &ChatCompletion::finished, &loop, [&](const QString &text) {
      result = text;
      loop.quit();
    });
    QObject::connect(&chat, &ChatCompletion::failed, &loop, [&](const QString &error) {
      std::fprintf(stderr, "failed: %s\n", qPrintable(error));
      loop.quit();
    });
    chat.start();
    loop.exec();
    return cleanProse(result);
  };
  SceneContext context = sampleContext();
  const QString before = "Elin sat at the kitchen table with her tea going cold.";
  const QString passage = "She unrolls the old harbour map and sees that Rope Street is missing.";
  const QString after = "The phone rang in the hall.";
  const int runs = qEnvironmentVariableIntValue("NOVELCLANKER_LIVE_RUNS") > 0
                       ? qEnvironmentVariableIntValue("NOVELCLANKER_LIVE_RUNS")
                       : 1;
  if (!qEnvironmentVariableIsSet("NOVELCLANKER_LIVE_OPENING_ONLY")) {
    for (const int target : {200, 500}) {
      for (int i = 0; i < runs; ++i) {
        const QString text =
            removeEcho(run(expandMessages(context, before, passage, after, target), target), after);
        std::printf("expand to %d: %lld words\n", target, static_cast<long long>(words(text)));
        std::fflush(stdout);
        if (qEnvironmentVariableIsSet("NOVELCLANKER_LIVE_SHOW")) {
          std::printf("---\n%s\n---\n", qPrintable(text));
        }
      }
    }
  }
  // A one-sentence opening followed by a long paragraph: models tend to go on and rewrite that paragraph.
  const QString opening = "Elin woke at six, as she always did.";
  const QString routine =
      "In the kitchen, Elin went through her morning like clockwork. The kettle rattled on the stove while "
      "she "
      "set out one cup, one saucer and the tin of oat biscuits. She ate standing at the window, watching the "
      "fishing boats nose out past the breakwater. Afterwards she unrolled yesterday's chart on the table "
      "and "
      "checked her pencil lines against the tide tables, correcting a sounding here and there. By half past "
      "seven the light had changed, and she pinned the chart flat with the brass weights her husband had "
      "made.";
  for (int i = 0; i < runs; ++i) {
    // Like the app: the prompt gets only the opening of the following text.
    const QString raw = run(expandMessages(context, "", opening, trimToWords(routine, 40), 200), 200);
    const QString text = removeEcho(raw, routine);
    std::printf(
        "expand opening to 200: %lld words%s\n", static_cast<long long>(words(text)),
        text == raw
            ? ""
            : qPrintable(QStringLiteral(" (repeat of the following text cut; %1 before)").arg(words(raw))));
    if (qEnvironmentVariableIsSet("NOVELCLANKER_LIVE_SHOW")) {
      std::printf("---\n%s\n---\n", qPrintable(raw));
    }
    std::fflush(stdout);
  }
  if (qEnvironmentVariableIsSet("NOVELCLANKER_LIVE_OPENING_ONLY")) {
    return;
  }
  const QString scene = run(sceneMessages(context, 500), 500);
  std::printf("scene of 500: %lld words\n", static_cast<long long>(words(scene)));
  std::fflush(stdout);
}

} // namespace

int main(int argc, char **argv)
{
  QCoreApplication app(argc, argv);
  testSceneMessages();
  testExpandMessages();
  testWords();
  testLengthLimit();
  testRemoveEcho();
  testCleanProse();
  liveLengths();
  if (g_failures == 0) {
    std::puts("All ProsePrompt tests passed.");
  }
  return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
