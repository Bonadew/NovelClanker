// Tests for NovelMarkdown: exporting a novel to Markdown and importing it (or hand-written Markdown) again.
// Usage: novelmarkdown_test <scratch-dir>

#include "NovelMarkdown.h"
#include "NovelStore.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

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

using NewAct = NovelStore::NewAct;

// "Act[Chapter(Scene,Scene)] ..." for compact comparisons.
std::string outline(const std::vector<NewAct> &acts)
{
  std::string out;
  for (const auto &act : acts) {
    out += (out.empty() ? "" : " ") + act.title + "[";
    for (std::size_t c = 0; c < act.chapters.size(); ++c) {
      out += (c ? "," : "") + act.chapters[c].title + "(";
      for (std::size_t s = 0; s < act.chapters[c].scenes.size(); ++s) {
        out += (s ? "," : "") + act.chapters[c].scenes[s].title;
      }
      out += ")";
    }
    out += "]";
  }
  return out;
}

const std::string &text(const std::vector<NewAct> &acts, std::size_t a, std::size_t c, std::size_t s)
{
  return acts.at(a).chapters.at(c).scenes.at(s).text;
}

// Everything in a novel (titles, order, scene text) survives export and import; briefs don't.
// @relation(FR-IMP-6, FR-IMP-7, FR-EXP-1, scope=function)
void testRoundTrip(const fs::path &root)
{
  const std::string tricky =
      "She read it twice.\n\n# Not an act\n\nA heading inside the scene.\n\n* * *\n\n"
      "After the scene's own break.\n<!-- a note of the author's -->\n    indented line";
  std::string exported;
  {
    NovelStore store(root / "original.novel", NovelStore::Mode::Create);
    const NovelStore::Id act1 = store.addAct(0, "Act 1: Home");
    const NovelStore::Id ch1 = store.addChapter(act1, 0, "Chapter 1: The Letter");
    const NovelStore::Id s1 = store.addScene(ch1, 0, "The Letter --> found", tricky);
    store.setBrief(s1, "POV: Elin. Briefs are not exported.");
    store.addScene(ch1, 1, "Empty scene");
    const NovelStore::Id s3 = store.addScene(ch1, 2, "The Council");
    store.setSceneText(s3, "The hall was cold.\n\n\n");
    store.addChapter(act1, 1, "Chapter 2: Nothing yet");
    const NovelStore::Id act2 = store.addAct(1, "Act 2: Away");
    const NovelStore::Id ch3 = store.addChapter(act2, 0, "Chapter 3: Storm");
    store.setSceneText(store.addScene(ch3, 0, "Waves"), "Salt.");
    store.addAct(2, "Act 3: Empty");
    exported = NovelMarkdown::write(store);
  }
  // Readable Markdown: headings for acts and chapters, scene breaks between scenes.
  EXPECT(exported.find("\n# Act 2: Away\n") != std::string::npos);
  EXPECT(exported.find("\n## Chapter 3: Storm\n") != std::string::npos);
  EXPECT(exported.find("Briefs are not exported") == std::string::npos);

  const auto acts = NovelMarkdown::read(exported);
  EXPECT(outline(acts) == "Act 1: Home[Chapter 1: The Letter(The Letter --> found,Empty scene,The Council),"
                          "Chapter 2: Nothing yet()] Act 2: Away[Chapter 3: Storm(Waves)] Act 3: Empty[]");
  EXPECT(text(acts, 0, 0, 0) == tricky);
  EXPECT(text(acts, 0, 0, 1).empty());
  EXPECT(text(acts, 0, 0, 2) == "The hall was cold."); // trailing blank lines aren't kept
  EXPECT(text(acts, 1, 0, 0) == "Salt.");
  EXPECT(acts[0].brief.empty() && acts[0].chapters[0].scenes[0].brief.empty());

  // Imported into a new novel, and exported again: the same file.
  {
    NovelStore copy(root / "copy.novel", NovelStore::Mode::Create);
    copy.importStructure(acts, true);
    EXPECT(copy.acts().size() == 3 && copy.acts()[0].chapters[0].scenes.size() == 3);
    EXPECT(copy.sceneText(copy.acts()[0].chapters[0].scenes[0].id) == tricky);
    EXPECT(NovelMarkdown::write(copy) == exported);
  }
  // Windows line ends and a byte order mark don't matter.
  std::string crlf = "\xEF\xBB\xBF";
  for (const char c : exported) {
    crlf += c == '\n' ? std::string("\r\n") : std::string(1, c);
  }
  EXPECT(outline(NovelMarkdown::read(crlf)) == outline(acts));
  EXPECT(text(NovelMarkdown::read(crlf), 0, 0, 0) == tricky);
}

// Markdown without NovelClanker's comments: an older export, or written by hand.
// @relation(FR-IMP-7, scope=function)
void testPlainMarkdown()
{
  const auto acts = NovelMarkdown::read(
      "A prologue before any heading.\n\n"
      "# Part One\n\n## The Harbour\n\nFirst scene.\n\n### A sub-heading\n\nStill first.\n\n"
      "* * *\n\nSecond scene.\n\n***\n\nThird scene.\n\n"
      "## The Map\n\n---\n\nFourth.\n\n# Part Two\n\nNo chapter heading here.\n");
  EXPECT(outline(acts) == "Act 1[Chapter 1(Scene 1)] Part One[The Harbour(Scene 1,Scene 2,Scene 3),"
                          "The Map(Scene 1)] Part Two[Chapter 4(Scene 1)]");
  EXPECT(text(acts, 0, 0, 0) == "A prologue before any heading.");
  EXPECT(text(acts, 1, 0, 0) == "First scene.\n\n### A sub-heading\n\nStill first.");
  EXPECT(text(acts, 1, 0, 1) == "Second scene.");
  EXPECT(text(acts, 1, 0, 2) == "Third scene.");
  EXPECT(text(acts, 1, 1, 0) == "Fourth.");
  EXPECT(text(acts, 2, 0, 0) == "No chapter heading here.");

  // The export format before scene titles were written: "* * *" between scenes, no comments.
  const auto old = NovelMarkdown::read("# Act 1\n\n## Chapter 1\n\nOne.\n\n* * *\n\nTwo.\n\n");
  EXPECT(outline(old) == "Act 1[Chapter 1(Scene 1,Scene 2)]");
  EXPECT(text(old, 0, 0, 1) == "Two.");

  // Nothing to import.
  EXPECT(NovelMarkdown::read("").empty());
  EXPECT(NovelMarkdown::read("\n\n   \n").empty());
  // Headings only: the structure without scenes.
  EXPECT(outline(NovelMarkdown::read("# A\n## B\n## C\n")) == "A[B(),C()]");
}

} // namespace

int main(int argc, char **argv)
{
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <scratch-dir>\n", argv[0]);
    return 2;
  }
  const fs::path root = argv[1];
  fs::remove_all(root);
  fs::create_directories(root);

  testRoundTrip(root);
  testPlainMarkdown();

  if (g_failures == 0) {
    std::puts("All NovelMarkdown tests passed.");
  }
  return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
