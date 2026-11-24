#include "PlanPrompt.h"

namespace PlanPrompt {

QString systemPrompt()
{
    return QStringLiteral(
        R"(You are an experienced developmental editor and story architect. You turn a novel synopsis into a clear, practical writing plan that an author (or a writing assistant) can follow scene by scene. You plan the novel; you do not write its prose. You follow the requested output format exactly.)");
}

QString userPrompt(const QString &synopsis)
{
    static const QString instructions = QStringLiteral(R"(Create a plan for the novel described by the synopsis at the end of this message.

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
""")");
    return instructions + "\n" + synopsis + "\n\"\"\"";
}

QList<ChatMessage> messages(const QString &synopsis)
{
    return {{QStringLiteral("system"), systemPrompt()}, {QStringLiteral("user"), userPrompt(synopsis)}};
}

} // namespace PlanPrompt
