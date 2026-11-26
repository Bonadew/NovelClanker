#pragma once

#include <QList>
#include <QString>

// Reads the manuscript structure (acts -> chapters -> scenes) out of a plan.
//
// Expects the plan prompt's Markdown shape but tolerates common variations:
//   ### Act 1: Setup                      (also Stage, Part or Book; any heading level; ":", "-", "–" or "—"
//                                         separators; roman numerals)
//   <paragraph>                           -> the act's brief
//   #### Chapter 3: The Storm             ("Chapter 11.5", no title, ...)
//   <paragraph>                           -> the chapter's brief
//   - **Scene 1: The Wave**: POV: ...     (bold optional; the text after the title is the scene's brief)
// Only the "Structure" section is read when the plan has one, so the story spine, characters and open
// questions are ignored. Chapters before any act get an implicit act; scenes before any chapter get an
// implicit chapter; a chapter without scenes gets one scene briefed with the chapter's purpose.
namespace PlanStructure {

struct Scene {
  QString title;
  QString brief;
};

struct Chapter {
  QString title;
  QString brief;
  QList<Scene> scenes;
};

struct Act {
  QString title;
  QString brief;
  QList<Chapter> chapters;
};

QList<Act> parse(const QString &plan);

struct Counts {
  int acts = 0, chapters = 0, scenes = 0;
};

Counts count(const QList<Act> &acts);

} // namespace PlanStructure
