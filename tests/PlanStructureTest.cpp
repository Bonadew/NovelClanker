// Tests for PlanStructure::parse(). Optional: set NOVELWRITER_PARSE_PLAN=<plan file> to print what a real
// plan parses into.

#include "NovelStore.h"
#include "PlanStructure.h"

#include <QCoreApplication>
#include <QFile>
#include <QRegularExpression>

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

// @relation(FR-IMP-1, FR-IMP-3, FR-TPL-4, scope=function)
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

// @relation(FR-IMP-2, scope=function)
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

// @relation(FR-IMP-2, scope=function)
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

// Regression: chapters written as list items (as mn-12b-mag-mell-r1 does) were not recognised, so each one was
// appended to the brief of the scene before it, and the first one in an act to the act's brief.
// @relation(FR-IMP-2, scope=function)
void testChaptersAsListItems()
{
    const QString plan = "# Harbour Lights\n\n"
                         "### Act 1: Arrival (Settling In)\n"
                         "- **Chapter 1 - Maps and Memories**\n"
                         "  - Scene 1.1: Elin unpacks her instruments in the keeper's cottage.\n"
                         "  - Scene 1.2: She finds her father's charts, annotated in a hand she knows.\n"
                         "- **Chapter 2 - The Old Lighthouse**\n"
                         "  - Scene 2.1: Elin climbs the tower and counts the missing panes.\n"
                         "\n"
                         "### Act 2: Storms\n"
                         "- **Chapter 3 - Warnings**: Elin hears the forecast.\n"
                         "  - Scene 3.1: The harbour master urges her to leave before the gale.\n"
                         "- Chapter 4: Night Watch\n"
                         "  - Scene 4.1: The lamp fails at midnight.\n"
                         "Chapter 5: Plain line\n"
                         "- **Scene 1:** POV: Elin. Setting: the lantern room, dawn.\n"
                         "- Chapter 5 is where everything turns.\n";
    const QList<Act> acts = parse(plan);
    EXPECT(outline(acts) == "Act 1: Arrival (Settling In)[Chapter 1: Maps and Memories(Scene 1,Scene 2),"
                            "Chapter 2: The Old Lighthouse(Scene 1)] "
                            "Act 2: Storms[Chapter 3: Warnings(Scene 1),Chapter 4: Night Watch(Scene 1),"
                            "Chapter 5: Plain line(Scene 1)]");
    EXPECT(acts.size() == 2 && acts[0].brief.isEmpty()); // the first chapter didn't end up in the act's brief
    // No brief contains the next chapter's heading.
    for (const Act &act : acts) {
        for (const Chapter &chapter : act.chapters) {
            for (const Scene &scene : chapter.scenes) {
                EXPECT(!scene.brief.contains(QRegularExpression(QStringLiteral(R"(Chapter \d+\s*[:\-])"))));
            }
        }
    }
    EXPECT(acts[0].chapters[0].scenes[1].brief == "She finds her father's charts, annotated in a hand she knows.");
    EXPECT(acts[0].chapters[1].scenes[0].brief == "Elin climbs the tower and counts the missing panes.");
    EXPECT(acts[1].chapters[0].brief == "Elin hears the forecast.");
    // "- Chapter 5 is where..." is a sentence, not a chapter: it continues the scene's brief.
    EXPECT(acts[1].chapters[2].scenes.size() == 1);
    EXPECT(acts[1].chapters[2].scenes[0].brief ==
           "POV: Elin. Setting: the lantern room, dawn. Chapter 5 is where everything turns.");
}

// A last scene directly followed by the next chapter or act, without a blank line, in each notation.
// @relation(FR-IMP-2, scope=function)
void testSceneBeforeNextHeading()
{
    for (const QString &next : {QString("#### Chapter 2: Next"), QString("**Chapter 2: Next**"),
                                QString("- **Chapter 2 - Next**"), QString("- Chapter 2: Next"),
                                QString("Chapter 2: Next"), QString("  * **Chapter 2: Next**: purpose")}) {
        const QString plan = "### Act 1: A\n#### Chapter 1: First\n"
                             "- **Scene 1: Last**: The last scene of chapter 1.\n" +
                             next + "\n- **Scene 1: Opening**: The first scene of chapter 2.\n";
        const QList<Act> acts = parse(plan);
        EXPECT(outline(acts) == "Act 1: A[Chapter 1: First(Last),Chapter 2: Next(Opening)]");
        EXPECT(acts[0].chapters[0].scenes[0].brief == "The last scene of chapter 1.");
    }
    const QList<Act> acts = parse("#### Chapter 1: First\n- **Scene 1: Last**: The end.\n- **Act 2: Next**\n");
    EXPECT(outline(acts) == "Act 1[Chapter 1: First(Last)] Act 2: Next[]");
    EXPECT(acts[0].chapters[0].scenes[0].brief == "The end.");
}

// Regression: gpt-oss writes narrow no-break spaces (U+202F) as in "Chapter 10" and "Scene 1". Those
// chapters were dropped (half the plan) and scene lines became briefs.
// @relation(FR-IMP-2, scope=function)
void testTypographicCharacters()
{
    const QString plan = QString::fromUtf8("\xEF\xBB\xBF" // byte-order mark
                                           "## Structure\n\n"
                                           "### Act 1: The Inheritance\n"
                                           "#### Chapter 9: Arrival  \n"
                                           "- **Scene 1**: POV Mira. Setting: Harbor at dawn.\n"
                                           "#### Chapter 10: Letter Delivered  \n"
                                           "- **Scene 1**: POV Sofie. Setting: her kitchen.\n"
                                           "- **Scene 2 ‑ The Reading**: She reads it.\n"
                                           "### Act 2: After\n"
                                           "#### Chapter​ 11‑ Home\n"
                                           "\t- Scene 1: Return ‑ She comes back.\n");
    const QList<Act> acts = parse(plan);
    EXPECT(outline(acts) == "Act 1: The Inheritance[Chapter 9: Arrival(Scene 1),"
                            "Chapter 10: Letter Delivered(Scene 1,The Reading)] Act 2: After[Chapter 11: Home(Return)]");
    EXPECT(acts.size() == 2 && acts[0].chapters.size() == 2);
    EXPECT(acts[0].chapters[0].scenes[0].brief == "POV Mira. Setting: Harbor at dawn."); // plain spaces in briefs
    EXPECT(acts[0].chapters[1].scenes[0].brief == "POV Sofie. Setting: her kitchen.");
    EXPECT(acts[0].chapters[1].brief.isEmpty()); // the scene lines didn't become the chapter's brief
    EXPECT(acts[1].chapters[0].scenes[0].brief == "She comes back.");
}

// The "Inner stages across years" template: stages are the act-level parts and keep their word.
// @relation(FR-IMP-2, FR-TPL-4, scope=function)
void testStagePlan()
{
    const QString plan = "# Tide Tables\n\n"
                         "## Stage progression\n"
                         "1. **Denial**: she insists nothing has changed. Time: her sixties. Ends with: the harbour closes.\n"
                         "2. **Bargaining**: small deals with herself. Time: her early seventies. Ends with: the last map.\n\n"
                         "## Structure\n\n"
                         "### Stage 1: Denial\n"
                         "Elin keeps redrawing a harbour that is emptying around her.\n\n"
                         "#### Chapter 1: The Map Room\n"
                         "Establish her routines.\n"
                         "- **Scene 1: Fresh Ink**: POV: Elin. Time: early spring, the year she turns 63. "
                         "Setting: the map room. She adds a jetty that was torn down years ago.\n"
                         "- **Scene 2: The Survey Boat**: POV: Elin. Time: the next morning. Setting: the quay. "
                         "A young surveyor asks for her old soundings.\n\n"
                         "### Stage 2: Bargaining\n\n"
                         "#### Chapter 2: Detours\n"
                         "- **Scene 1: The Letter**: POV: Elin. Time: her 71st birthday. Setting: home. "
                         "She answers her brother's letter with a chart instead of words.\n\n"
                         "## Open questions\n- Does the surveyor return in Stage 2?\n";
    const QList<Act> acts = parse(plan);
    EXPECT(outline(acts) == "Stage 1: Denial[Chapter 1: The Map Room(Fresh Ink,The Survey Boat)] "
                            "Stage 2: Bargaining[Chapter 2: Detours(The Letter)]");
    EXPECT(acts[0].brief == "Elin keeps redrawing a harbour that is emptying around her.");
    EXPECT(acts[0].chapters[0].scenes[0].brief.startsWith("POV: Elin. Time: early spring, the year she turns 63."));
    // Part and Book work the same way; the word is kept as written.
    EXPECT(outline(parse("## Part II: Later\n#### Chapter 3: X\n- **Scene 1: Y**: z\n## BOOK 1\n")) ==
           "Part II: Later[Chapter 3: X(Y)] Book 1[]");
}

// @relation(FR-IMP-5, scope=function)
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
            for (const Scene &scene : chapter.scenes) {
                std::printf("    %s | %s\n", qPrintable(scene.title), qPrintable(scene.brief.left(70)));
            }
        }
    }
}

// Optional: set NOVELWRITER_PARSE_NOVEL=<novel folder> to parse every plan in a novel and report briefs that
// look like they swallowed a heading (use a copy: opening a novel may write RocksDB housekeeping files).
void checkNovelPlans()
{
    const QString path = qEnvironmentVariable("NOVELWRITER_PARSE_NOVEL");
    if (path.isEmpty()) {
        return;
    }
    NovelStore store(std::filesystem::path(path.toStdU16String()), NovelStore::Mode::Open);
    static const QRegularExpression heading(QStringLiteral(R"(\b(Act|Chapter)\s+[0-9IVX]+)"),
                                            QRegularExpression::CaseInsensitiveOption);
    const QString dumpDir = qEnvironmentVariable("NOVELWRITER_DUMP_PLANS"); // optional: write each plan's text here
    for (const auto &plan : store.plans()) {
        if (!dumpDir.isEmpty()) {
            QFile out(dumpDir + "/" + QString::fromStdString(plan.title) + ".md");
            if (out.open(QIODevice::WriteOnly)) {
                out.write(store.planText(plan.id).c_str());
            }
        }
        const QList<Act> acts = parse(QString::fromStdString(store.planText(plan.id)));
        const Counts c = count(acts);
        std::printf("Plan \"%s\" (%s): %d acts, %d chapters, %d scenes\n", plan.title.c_str(),
                    plan.model.empty() ? "by hand" : plan.model.c_str(), c.acts, c.chapters, c.scenes);
        for (const Act &act : acts) {
            for (const Chapter &chapter : act.chapters) {
                for (const Scene &scene : chapter.scenes) {
                    if (heading.match(scene.brief).hasMatch()) {
                        std::printf("  SUSPICIOUS brief in %s / %s / %s:\n    %s\n", qPrintable(act.title),
                                    qPrintable(chapter.title), qPrintable(scene.title), qPrintable(scene.brief));
                    }
                }
            }
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
    testChaptersAsListItems();
    testSceneBeforeNextHeading();
    testTypographicCharacters();
    testStagePlan();
    testNothingToFind();
    printRealPlan();
    checkNovelPlans();
    if (g_failures == 0) {
        std::puts("All PlanStructure tests passed.");
    }
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
