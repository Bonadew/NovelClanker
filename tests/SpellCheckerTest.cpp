// Tests for spell checking: SpellChecker (Hunspell with the bundled dictionaries) and the highlighter's
// underlining. Needs NOVELCLANKER_DICTIONARIES=<repository>/dictionaries (set by CTest). Runs headless.

#include "SpellChecker.h"
#include "MarkdownHighlighter.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>

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

SpellChecker::Dictionary dictionary(const QString &code)
{
  for (const auto &d : SpellChecker::available()) {
    if (d.code == code) {
      return d;
    }
  }
  return {};
}

// "word word" for the words SpellChecker::words finds.
QString wordList(const QString &text)
{
  QStringList out;
  for (const auto &w : SpellChecker::words(text)) {
    out << text.mid(w.start, w.length);
  }
  return out.join(' ');
}

// The underlined stretches of text in the first block of `doc`, in order, separated by spaces. (A word can be
// split over several format ranges, e.g. when part of it is bold, so marks are collected per character.)
QString underlined(QTextDocument &doc)
{
  const QTextBlock block = doc.firstBlock();
  const QString text = block.text();
  QList<bool> marked(text.size(), false);
  for (const auto &range : block.layout()->formats()) {
    if (range.format.underlineStyle() == QTextCharFormat::SpellCheckUnderline) {
      for (int i = range.start; i < range.start + range.length && i < text.size(); ++i) {
        marked[i] = true;
      }
    }
  }
  QStringList out;
  for (qsizetype i = 0; i < text.size(); ++i) {
    if (marked[i] && (i == 0 || !marked[i - 1])) {
      out << QString();
    }
    if (marked[i]) {
      out.last() += text[i];
    }
  }
  return out.join(' ');
}

// @relation(FR-SPL-1, scope=function)
void testDictionaries()
{
  const auto all = SpellChecker::available();
  QStringList names;
  for (const auto &d : all) {
    names << d.code + "=" + d.name;
  }
  EXPECT(names.join(' ') == "en_GB=English (UK) en_US=English (US)");

  SpellChecker spell;
  EXPECT(!spell.isLoaded() && spell.isCorrect("anythingwhatsoever")); // no dictionary: nothing is wrong
  EXPECT(spell.load(dictionary("en_US")) && spell.code() == "en_US");
  EXPECT(spell.isCorrect("color") && !spell.isCorrect("colour"));
  EXPECT(spell.isCorrect("harbor") && spell.isCorrect("The") && spell.isCorrect("cartographer"));
  EXPECT(!spell.isCorrect("speling"));
  EXPECT(spell.suggestions("speling").contains("spelling"));
  EXPECT(spell.suggestions("speling", 2).size() <= 2);

  EXPECT(spell.load(dictionary("en_GB")) && spell.code() == "en_GB");
  EXPECT(spell.isCorrect("colour") && spell.isCorrect("harbour"));
  EXPECT(!spell.isCorrect("harbor"));

  EXPECT(!spell.load({"xx", "Nothing", "missing.dic", "missing.aff"}) && !spell.isLoaded());
}

// @relation(FR-SPL-2, FR-SPL-3, scope=function)
void testWordsAndPersonalDictionary()
{
  EXPECT(wordList("Don't stop: Elin’s naïve x2 file_name well-known 42") ==
         "Don't stop Elin’s naïve well known");

  QTemporaryDir dir;
  const QString personal = dir.filePath("personal-dictionary.txt");
  {
    SpellChecker spell;
    spell.setPersonalDictionaryPath(personal);
    EXPECT(spell.load(dictionary("en_US")));
    EXPECT(spell.isCorrect("don't") && spell.isCorrect("don’t")); // curly apostrophes too
    EXPECT(!spell.isCorrect("Lindqvist") && !spell.isCorrect("Lindqvist's"));
    spell.addToPersonalDictionary("Lindqvist");
    EXPECT(spell.isCorrect("Lindqvist") && spell.isCorrect("Lindqvist's") && spell.isCorrect("Lindqvist’s"));
    EXPECT(spell.suggestions("Lindqvst").contains("Lindqvist"));
    spell.ignore("Elinor");
    EXPECT(spell.isCorrect("Elinor"));
    EXPECT(spell.suggestions("dont").contains("don't"));
    EXPECT(spell.suggestions("don’tt").join(' ').contains("don’t")); // the author's apostrophes are kept
  }
  QFile file(personal);
  EXPECT(file.open(QIODevice::ReadOnly) && file.readAll() == "Lindqvist\n");
  SpellChecker again; // the personal dictionary is kept; ignored words are not
  again.setPersonalDictionaryPath(personal);
  EXPECT(again.load(dictionary("en_GB")));
  EXPECT(again.isCorrect("Lindqvist") && !again.isCorrect("Elinor"));
}

// @relation(FR-SPL-1, scope=function)
void testNonAsciiFolder()
{
  QTemporaryDir dir;
  const QString folder = dir.filePath(QString::fromUtf8("Ordböcker 小説"));
  QDir().mkpath(folder);
  const auto us = dictionary("en_US");
  EXPECT(QFile::copy(us.dicPath, folder + "/en_US.dic") && QFile::copy(us.affPath, folder + "/en_US.aff"));
  const auto found = SpellChecker::available({folder});
  EXPECT(found.size() == 1 && found[0].code == "en_US");
  SpellChecker spell;
  EXPECT(!found.isEmpty() && spell.load(found[0]) && spell.isCorrect("color") && !spell.isCorrect("colour"));
}

// @relation(FR-SPL-4, scope=function)
void testHighlighter()
{
  SpellChecker spell;
  QTemporaryDir dir;
  spell.setPersonalDictionaryPath(dir.filePath("p.txt"));
  EXPECT(spell.load(dictionary("en_US")));

  const auto check = [&](const QString &text, bool markdown) {
    QTextDocument doc;
    doc.setPlainText(text);
    MarkdownHighlighter highlighter(&doc, markdown);
    highlighter.setSpellChecker(&spell);
    return underlined(doc);
  };
  EXPECT(check("The mapp was **bigg** and *olde*.", true) == "mapp bigg olde");
  EXPECT(check("# A hedding", true) == "hedding");
  // Code, link targets, URLs, e-mail addresses and tags aren't prose.
  EXPECT(
      check("Run `fooo` see [the mapp](http://exampl.com/xyzz) or www.exampl.org, mail me@exampl.com <brr>",
            true) == "mapp");
  EXPECT(check("In plain text `fooo` is a word, but http://exampl.com isn't.", false) == "fooo");
  EXPECT(check("All correct here.", true).isEmpty());

  QTextDocument doc;
  doc.setPlainText("Plain line.\n```\nint fooo;\n```\nAfter the fense.");
  MarkdownHighlighter highlighter(&doc);
  highlighter.setSpellChecker(&spell);
  const QTextBlock code = doc.findBlockByNumber(2), after = doc.findBlockByNumber(4);
  EXPECT(!highlighter.misspelledWordAt(code, 6).has_value()); // inside a code fence
  const auto word = highlighter.misspelledWordAt(after, 12);
  EXPECT(word && after.text().mid(word->start, word->length) == "fense");
  EXPECT(!highlighter.misspelledWordAt(after, 2).has_value());

  highlighter.setSpellChecker(nullptr); // checking off
  EXPECT(!highlighter.misspelledWordAt(after, 12).has_value());
}

} // namespace

int main(int argc, char **argv)
{
  QApplication app(argc, argv);
  QStandardPaths::setTestModeEnabled(true); // never touch the user's personal dictionary
  if (qEnvironmentVariableIsEmpty("NOVELCLANKER_DICTIONARIES")) {
    std::fprintf(stderr, "set NOVELCLANKER_DICTIONARIES to the repository's dictionaries folder\n");
    return 2;
  }
  testDictionaries();
  testWordsAndPersonalDictionary();
  testNonAsciiFolder();
  testHighlighter();
  if (g_failures == 0) {
    std::puts("All spelling tests passed.");
  }
  return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
