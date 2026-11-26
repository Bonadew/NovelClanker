#include "PlanPrompt.h"

namespace PlanPrompt {

namespace {

// The classic plot-driven model: three acts of about 25/50/25% with named turning points.
const QString kThreeActInstructions =
    QStringLiteral(R"PROMPT(Create a plan for the novel described by the synopsis at the end of this message.

Requirements:
- Stay faithful to the synopsis: keep its characters, events, tone and ending. Where it is vague, make choices that fit its genre and tone, and list every important invention under "Open questions" so the author can confirm or change it.
- Work out the story before the details: first write the "Story spine", every key event of the synopsis plus the turning points (inciting incident, first plot point, midpoint, crisis, climax, resolution) in chronological order, each labelled with its act. Then build the Structure from the spine: follow its order exactly, give each spine event its place once, and never repeat or reorder events.
- Use exactly three acts (### Act 1: setup, ### Act 2: confrontation, ### Act 3: resolution) unless the synopsis clearly calls for something else. Put about a quarter of the chapters in Act 1, half in Act 2 and a quarter in Act 3. The inciting incident and first plot point belong to Act 1, the midpoint and the crisis to Act 2 (the crisis at its end), and the climax and resolution to Act 3. Act 2 must escalate the conflict with complications and rising stakes; do not resolve the central conflict before Act 3.
- Divide each act into chapters and each chapter into scenes. Every scene must matter: a point-of-view character with a goal, an obstacle or conflict, and an outcome that changes the situation.
- Give the protagonist and other major characters arcs that develop across the acts, and keep subplots connected to the main plot.
- Choose the number of chapters from the amount of story: expand the synopsis with the complications, subplots and character moments a full novel needs (typically 15 to 30 chapters with 1 to 4 scenes each), but never pad. Every chapter must contain new events; when the story is told, end the plan instead of repeating or restating chapters.
- Write the plan in the same language as the synopsis.
- Keep each scene to one or two sentences so the whole plan fits in one answer.

Output only the plan, as Markdown, with exactly this structure:

# <Working title>

## Premise
<One or two sentences: protagonist, goal, central conflict, stakes.>

## Genre, tone and point of view
<Genre, target audience, tone, and narrative point of view and tense.>

## Themes
- <theme>

## Characters
- **<Name>**: <role>. Wants: <external goal>. Needs: <inner need>. Arc: <how they change>.

## Settings
- **<Place>**: <short description and its role in the story>.

## Story spine
1. <Key event> (Act 1, inciting incident)
2. <Key event> (Act 1)
...
<n>. <Key event> (Act 3, climax)
<n+1>. <Key event> (Act 3, resolution)

## Structure

### Act 1: <act title>
<One paragraph: what this act accomplishes and which turning points it contains.>

#### Chapter 1: <chapter title>
<One sentence: the purpose of the chapter.>
- **Scene 1: <scene title>**: POV: <character>. Setting: <place, time>. <Goal, conflict and outcome.>
- **Scene 2: <scene title>**: ...

#### Chapter 2: <chapter title>
...

### Act 2: <act title>
...

## Open questions
- <A decision the author should confirm or make.>

Number chapters continuously through the whole novel, and number scenes from 1 within each chapter.

Synopsis:
""")PROMPT");

// A character-driven model for stories told across many years: the novel follows the protagonist through
// successive inner stages (as in the stages of grief) instead of a plot with turning points. Time passes
// through concrete markers in the scenes, never through "years later" summaries, and the ending may be bleak.
const QString kInnerStagesInstructions =
    QStringLiteral(R"PROMPT(Create a plan for the novel described by the synopsis at the end of this message.

This novel is character-driven and told across many years. Plan it as the protagonist's progression through a sequence of inner stages, like the stages of grief, not as a three-act plot with turning points.

Requirements:
- Stay faithful to the synopsis: keep its characters, events, tone and ending, even when the ending is bleak or unresolved. Do not add a rescue, a twist, a climax or a happy ending the synopsis does not have. Where it is vague, make choices that fit its tone, and list every important invention under "Open questions" so the author can confirm or change it.
- Work out the progression before the details: first write the "Stage progression", the 5 to 8 inner stages the protagonist passes through, in order. Name each stage with one word for the inner state (as in the stages of grief: Denial, Anger, Bargaining, Depression, Acceptance), and give its meaning, the protagonist's age or the years it covers, and what is different in them when the stage ends. Each stage must grow out of the one before it.
- Each stage becomes one part of the Structure (### Stage N: <name>). Divide each stage into chapters and each chapter into scenes. Give stages room according to their weight; they need not be equal.
- Scenes can be quiet and introspective, but every scene must change something: the protagonist's inner state, a relationship, their circumstances, or what they believe about themselves.
- The story spans years, so show time passing inside the scenes through concrete markers: seasons and weather, birthdays and holidays, signs of ageing, changes in technology and culture, changes in the protagonist's home and surroundings, and people arriving or leaving. Never move time with summaries such as "many years later". Give every scene a time marker.
- Use flashbacks or memories only where they reveal the past or make the passage of time felt.
- Let secondary characters and subplots (friends, neighbours, family, work) mirror or contrast the protagonist's progression, and let them change over the years too.
- Choose the number of chapters from the amount of story (typically 15 to 30 chapters with 1 to 4 scenes each), but never pad. Every chapter must contain something new; when the progression is complete, end the plan.
- Write the plan in the same language as the synopsis.
- Keep each scene to one or two sentences so the whole plan fits in one answer.

Output only the plan, as Markdown, with exactly this structure:

# <Working title>

## Premise
<One or two sentences: protagonist, what they long for, what keeps them from it, and what is at stake as the years pass.>

## Genre, tone and point of view
<Genre, target audience, tone, and narrative point of view and tense.>

## Themes
- <A question the book explores?>

## Characters
- **<Name>**: <role>. Longs for: <inner desire>. Holds them back: <obstacle within or around them>. Over the years: <how they change, or fail to>.

## Settings
- **<Place>**: <short description and how it changes over the years>.

## Stage progression
1. **<Stage name>**: <the inner state>. Time: <age or years>. Ends with: <what is different afterwards>.
2. **<Stage name>**: ...

## Structure

### Stage 1: <Stage name>
<One paragraph: the protagonist's inner state in this stage, how it begins, and how it gives way to the next.>

#### Chapter 1: <chapter title>
<One sentence: the purpose of the chapter.>
- **Scene 1: <scene title>**: POV: <character>. Time: <time marker, e.g. early spring, the year she turns 60>. Setting: <place>. <What happens and what changes.>
- **Scene 2: <scene title>**: ...

#### Chapter 2: <chapter title>
...

### Stage 2: <Stage name>
...

## Open questions
- <A decision the author should confirm or make.>

Number chapters continuously through the whole novel, and number scenes from 1 within each chapter.

Synopsis:
""")PROMPT");

// A short story: one protagonist, one conflict, a short span of time and a single effect. Planned in three
// parts (beginning, middle, end) with few scenes, each with a target length, so the whole story stays within
// a short story's length. A short story has no chapters, so each part holds exactly one chapter.
const QString kShortStoryInstructions = QStringLiteral(
    R"PROMPT(Create a plan for the short story described by the synopsis at the end of this message.

A short story is not a small novel. It follows one protagonist through one central conflict over a short span of time, with few characters and few settings, and everything in it works toward a single effect: one dominant emotion or impression the reader is left with.

Requirements:
- Stay faithful to the synopsis: keep its characters, events, tone and ending, even when the ending is open, ambiguous or bleak. Where it is vague, make choices that fit its tone, and list every important invention under "Open questions" so the author can confirm or change it.
- Length: if the synopsis gives a target length, use it; otherwise aim for 3,000 to 7,500 words. Give every scene a target length, rounded to 250 words (usually 500 to 2,000), so that together they add up to the story's length.
- Work out the story before the details: first name the "Single effect", then write the "Story spine", 5 to 8 beats in chronological order, each labelled with its part: the opening (the protagonist in their situation, already under pressure), the disturbance that starts the story, the complications, the climax (the moment of greatest pressure, where the protagonist must choose or act), and the resolution (the change or revelation, and the final image). Then build the Structure from the spine: follow its order exactly, and never repeat or reorder beats.
- Start as late as possible: open close to the disturbance, and weave any backstory into the scenes instead of opening with it. Do not add flashbacks unless the synopsis needs them.
- Use exactly three parts (### Part 1: Beginning, ### Part 2: Middle, ### Part 3: End). The Beginning holds the opening and the disturbance and is short; the Middle escalates the one central conflict with one to three complications or attempts that fail or cost something; the End holds the climax and a brief resolution. No subplots.
- Each part holds exactly one chapter (#### Chapter N: <title>). The manuscript needs this level; it is not a chapter break in the story.
- Use 3 to 8 scenes in total, as few as the story needs. Every scene must change something: what the protagonist wants, knows or can do, a relationship, or their situation. Cut any scene that only explains or sets the mood.
- Keep the cast small: the protagonist and at most three other named characters. Use one point of view unless the synopsis clearly calls for more.
- End on the change or revelation and the final image. Do not explain the theme, sum up a moral or add an epilogue.
- Write the plan in the same language as the synopsis.

Output only the plan, as Markdown, with exactly this structure:

# <Working title>

## Premise
<One or two sentences: protagonist, what they want, what stands in the way, and what is at stake.>

## Genre, tone and point of view
<Genre, target audience, tone, narrative point of view and tense, and the target length in words.>

## Single effect
<One sentence: the emotion or impression the story leaves the reader with.>

## Characters
- **<Name>**: <role>. Wants: <what they want in this story>. Changes: <how they change, or what they realise>.

## Settings
- **<Place>**: <short description and its role in the story>.

## Story spine
1. <Beat> (Part 1, opening)
2. <Beat> (Part 1, disturbance)
...
<n>. <Beat> (Part 3, climax)
<n+1>. <Beat> (Part 3, resolution)

## Structure

### Part 1: Beginning
<One paragraph: what this part accomplishes and which beats it contains.>

#### Chapter 1: <chapter title>
<One sentence: the purpose of this part of the story.>
- **Scene 1: <scene title>**: POV: <character>. Setting: <place, time>. Words: <target length>. <Goal, conflict and outcome.>
- **Scene 2: <scene title>**: ...

### Part 2: Middle
<One paragraph.>

#### Chapter 2: <chapter title>
...

### Part 3: End
<One paragraph.>

#### Chapter 3: <chapter title>
...

## Open questions
- <A decision the author should confirm or make.>

Number scenes from 1 within each chapter.

Synopsis:
""")PROMPT");

} // namespace

const QList<Template> &templates()
{
  static const QList<Template> all{
      {kThreeActTemplate, QStringLiteral("Three acts (25/50/25)"),
       QStringLiteral(
           "A plot-driven novel in three acts with the classic turning points: inciting incident, first plot "
           "point, midpoint, crisis, climax and resolution."),
       kThreeActInstructions},
      {kInnerStagesTemplate, QStringLiteral("Inner stages across years"),
       QStringLiteral(
           "A character-driven novel spanning many years, planned as the protagonist's progression through "
           "inner stages (like the stages of grief). Time passes through concrete markers in the scenes; the "
           "ending may be bleak or unresolved."),
       kInnerStagesInstructions},
      {kShortStoryTemplate, QStringLiteral("Short story"),
       QStringLiteral(
           "A short story (about 3,000 to 7,500 words unless the synopsis says otherwise): one protagonist, "
           "one conflict and a single effect, in a beginning, middle and end of 3 to 8 scenes, each with a "
           "target length."),
       kShortStoryInstructions},
  };
  return all;
}

const Template &templateById(const QString &id)
{
  for (const Template &t : templates()) {
    if (t.id == id) {
      return t;
    }
  }
  return templates().front();
}

QString systemPrompt()
{
  return QStringLiteral(
      R"(You are an experienced developmental editor and story architect. You turn the synopsis of a novel or short story into a clear, practical writing plan that an author (or a writing assistant) can follow scene by scene. You plan the story; you do not write its prose. You follow the requested output format exactly.)");
}

QString userPrompt(const QString &synopsis, const QString &templateId)
{
  return templateById(templateId).instructions + "\n" + synopsis + "\n\"\"\"";
}

QList<ChatMessage> messages(const QString &synopsis, const QString &templateId)
{
  return {{QStringLiteral("system"), systemPrompt()},
          {QStringLiteral("user"), userPrompt(synopsis, templateId)}};
}

} // namespace PlanPrompt
