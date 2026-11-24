// Tests for PlanStructure::parse(). Optional: set NOVELWRITER_PARSE_PLAN=<plan file> to print what a real
// plan parses into.

#include "PlanStructure.h"

#include <QCoreApplication>
#include <QFile>

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

using namespace PlanStructure;

// "Act[Chapter(Scene,Scene)] ..." for compact comparisons.
QString outline(const QList<Act> &acts)
{
    QStringList out;
    for (const Act &act : acts) {
        QStringList chapters;
        for (const Chapter &chapter : act.chapters) {
            QStringList scenes;
            for (const Scene &scene : chapter.scenes) {
                scenes << scene.title;
            }
            chapters << chapter.title + "(" + scenes.join(',') + ")";
        }
        out << act.title + "[" + chapters.join(',') + "]";
    }
    return out.join(' ');
}

// The shape produced by the plan prompt (taken from a real generated plan).
const QString kPlan = R"(# The Keeper's Light

## Premise
Mira Holt inherits a lighthouse post.

## Characters
- **Mira Holt**: Young lighthouse keeper.

## Story spine
1. Mira finds the unsent letter in her brother's room (Act 1, inciting incident)
2. Mira sails north alone and delivers the letter (Act 3, climax)

## Structure

### Act 1: Setup
This act establishes Mira's isolation.

#### Chapter 1: The Cold Room
The purpose of the chapter is to introduce Mira's grief.
- **Scene 1: The Body**: POV: Mira. Setting: Brother's room, Skarra. Mira finds the letter.
- **Scene 2: The Reading**: POV: Mira. Setting: Brother's room. Mira reads the confession.

#### Chapter 2: The Council's Welcome
- **Scene 1: The Offer**: POV: Mira. Setting: Council hall. The Leader offers her the post.

### Act 2: Confrontation
Mira sets out.

#### Chapter 11.5: The Lost Letter
- **Scene 1: The Wave**: POV: Mira. Setting: Boat deck. A wave nearly sweeps her away.

### Act 3: Resolution

#### Chapter 12: The Return
The purpose is to bring Mira home.

## Open questions
- Scene 1: should this be a question? It must not be parsed.
)";

void testPromptFormat()
{
    const QList<Act> acts = parse(kPlan);
    EXPECT(outline(acts) == "Act 1: Setup[Chapter 1: The Cold Room(The Body,The Reading),"
                            "Chapter 2: The Council's Welcome(The Offer)] "
                            "Act 2: Confrontation[Chapter 11.5: The Lost Letter(The Wave)] "
                            "Act 3: Resolution[Chapter 12: The Return(Scene 1)]");
    EXPECT(acts[0].brief == "This act establishes Mira's isolation.");
    EXPECT(acts[0].chapters[0].brief == "The purpose of the chapter is to introduce Mira's grief.");
    EXPECT(acts[0].chapters[0].scenes[0].brief == "POV: Mira. Setting: Brother's room, Skarra. Mira finds the letter.");
    EXPECT(acts[2].brief.isEmpty());
    // A chapter without scenes gets one, briefed with the chapter's purpose.
    EXPECT(acts[2].chapters[0].scenes[0].brief == "The purpose is to bring Mira home.");
    const Counts c = count(acts);
    EXPECT(c.acts == 3 && c.chapters == 4 && c.scenes == 5);
}

void testVariations()
{
    // No bold, dashes, roman numerals, a title-less chapter, an extra non-structure heading, CRLF.
    const QString plan = "## Act I — Before\r\n"
                         "### Chapter 1 - Dawn\r\n"
                         "- Scene 1: Waking: Mira wakes up.\r\n"
                         "* Scene 2 – Breakfast — She eats.\r\n"
                         "1. Scene 3: Leaving\r\n"
                         "### Chapter 2\r\n"
                         "- **Scene 1 - The Road** - She walks.\r\n"
                         "  - She meets a stranger.\r\n"
                         "### Notes\r\n"
                         "Not a brief.\r\n"
                         "## ACT II: After\r\n";
    const QList<Act> acts = parse(plan);
    EXPECT(outline(acts) == "Act I: Before[Chapter 1: Dawn(Waking,Breakfast,Leaving),Chapter 2(The Road)] "
                            "Act II: After[]");
    EXPECT(acts[0].chapters[0].scenes[0].brief == "Mira wakes up.");
    EXPECT(acts[0].chapters[0].scenes[1].brief == "She eats.");
    EXPECT(acts[0].chapters[0].scenes[2].brief.isEmpty());
    EXPECT(acts[0].chapters[1].scenes[0].brief == "She walks. She meets a stranger."); // sub-bullet appended
    EXPECT(acts[0].chapters[1].brief.isEmpty());
}

void testImplicitParents()
{
    // Chapters with no act, scenes with no chapter, scene headings with paragraph briefs, bold-line headings.
    const QString plan = "**Chapter 1: Alone**\n"
                         "##### Scene 1: Night\n"
                         "She can't sleep.\n"
                         "Still awake.\n"
                         "\n"
                         "Unrelated paragraph.\n";
    QList<Act> acts = parse(plan);
    EXPECT(outline(acts) == "Act 1[Chapter 1: Alone(Night)]");
    EXPECT(acts[0].chapters[0].scenes[0].brief == "She can't sleep. Still awake.");

    acts = parse("- **Scene 1: Lost**: A scene without any chapter.\n");
    EXPECT(outline(acts) == "Act 1[Chapter 1(Lost)]");

    // A very long "title" is really a description.
    acts = parse("#### Chapter 1: X\n- Scene 1: " + QString(100, 'a') + "\n");
    EXPECT(acts[0].chapters[0].scenes[0].title == "Scene 1" && acts[0].chapters[0].scenes[0].brief.size() == 100);
}

void testNothingToFind()
{
    EXPECT(parse("").isEmpty());
    EXPECT(parse("# Just a title\n\nSome notes about the story. Act now!\n- a list item\n").isEmpty());
    // A Structure section with nothing usable in it.
    EXPECT(parse("## Structure\nTo be decided.\n## Act 1: Outside the section? No: after it.\n").isEmpty());
}

void printRealPlan()
{
    const QString path = qEnvironmentVariable("NOVELWRITER_PARSE_PLAN");
    if (path.isEmpty()) {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        std::printf("Cannot read %s\n", qPrintable(path));
        return;
    }
    const QList<Act> acts = parse(QString::fromUtf8(file.readAll()));
    const Counts c = count(acts);
    std::printf("%s: %d acts, %d chapters, %d scenes\n", qPrintable(path), c.acts, c.chapters, c.scenes);
    for (const Act &act : acts) {
        std::printf("%s\n", qPrintable(act.title));
        for (const Chapter &chapter : act.chapters) {
            std::printf("  %s (%lld scenes)\n", qPrintable(chapter.title), static_cast<long long>(chapter.scenes.size()));
        }
    }
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testPromptFormat();
    testVariations();
    testImplicitParents();
    testNothingToFind();
    printRealPlan();
    if (g_failures == 0) {
        std::puts("All PlanStructure tests passed.");
    }
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
