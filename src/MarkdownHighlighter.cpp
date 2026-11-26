#include "MarkdownHighlighter.h"

#include "Theme.h"

#include <QApplication>
#include <QEvent>
#include <QFontDatabase>
#include <QTextBlock>

#include <algorithm>

namespace {
// Indexes into m_inlineRules for the rules whose colour depends on the theme.
constexpr std::size_t kCodeRule = 3;
constexpr std::size_t kLinkRule = 4;
} // namespace

MarkdownHighlighter::MarkdownHighlighter(QTextDocument *parent, bool markdown)
    : QSyntaxHighlighter(parent), m_markdown(markdown)
{
  for (QTextCharFormat &format : m_headingFormats) {
    format.setFontWeight(QFont::Bold);
  }
  setHeadingSizes();

  m_quoteFormat.setFontItalic(true);
  m_listFormat.setFontWeight(QFont::Bold);
  const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
  m_codeFormat.setFontFamilies(mono.families());

  QTextCharFormat bold;
  bold.setFontWeight(QFont::Bold);
  QTextCharFormat italic;
  italic.setFontItalic(true);
  QTextCharFormat strike;
  strike.setFontStrikeOut(true);
  QTextCharFormat link;
  link.setFontUnderline(true);

  m_inlineRules = {
      {QRegularExpression(R"((\*\*|__)(?=\S)(.+?)(?<=\S)\1)"), bold},
      {QRegularExpression(R"((?<![\*_])([\*_])(?=\S)(.+?)(?<=\S)\1(?![\*_]))"), italic},
      {QRegularExpression(R"(~~(?=\S)(.+?)(?<=\S)~~)"), strike},
      {QRegularExpression(R"(`[^`]+`)"), m_codeFormat},      // kCodeRule
      {QRegularExpression(R"(\[[^\]]*\]\([^\)]*\))"), link}, // kLinkRule
  };
  updateColors();
  qApp->installEventFilter(this); // follow switches between light and dark mode
}

void MarkdownHighlighter::setHeadingSizes()
{
  const qreal headingScale[6] = {1.8, 1.5, 1.3, 1.15, 1.05, 1.0};
  const qreal base = document()->defaultFont().pointSizeF();
  for (int i = 0; i < 6; ++i) {
    m_headingFormats[i].setFontPointSize(base * headingScale[i]);
  }
}

void MarkdownHighlighter::refreshFonts()
{
  setHeadingSizes();
  rehighlight();
}

void MarkdownHighlighter::updateColors()
{
  const bool dark = Theme::isDark(QApplication::palette());
  const QColor heading = dark ? QColor(0x7a, 0xb4, 0xff) : QColor(0x2a, 0x5d, 0x9f);
  const QColor quote = dark ? QColor(0xa0, 0xa8, 0xb0) : QColor(0x6a, 0x73, 0x7d);
  const QColor list = dark ? QColor(0xf0, 0xa0, 0x50) : QColor(0xb0, 0x5a, 0x00);
  const QColor code = dark ? QColor(0xd8, 0x9a, 0xe8) : QColor(0x8a, 0x2b, 0x8a);
  const QColor rule = dark ? QColor(0x80, 0x80, 0x80) : QColor(0x99, 0x99, 0x99);
  const QColor link = dark ? QColor(0x6c, 0xb0, 0xff) : QColor(0x1a, 0x73, 0xe8);
  for (QTextCharFormat &format : m_headingFormats) {
    format.setForeground(heading);
  }
  m_quoteFormat.setForeground(quote);
  m_listFormat.setForeground(list);
  m_codeFormat.setForeground(code);
  m_ruleFormat.setForeground(rule);
  m_inlineRules[kCodeRule].format.setForeground(code);
  m_inlineRules[kLinkRule].format.setForeground(link);
  m_misspelledFormat.setUnderlineStyle(QTextCharFormat::SpellCheckUnderline);
  m_misspelledFormat.setUnderlineColor(dark ? QColor(0xff, 0x6b, 0x6b) : QColor(0xd0, 0x10, 0x10));
}

bool MarkdownHighlighter::eventFilter(QObject *watched, QEvent *event)
{
  if (watched == qApp && event->type() == QEvent::ApplicationPaletteChange) {
    updateColors();
    rehighlight();
  }
  return QSyntaxHighlighter::eventFilter(watched, event);
}

void MarkdownHighlighter::highlightBlock(const QString &text)
{
  static const QRegularExpression fence(R"(^\s*(```|~~~))");

  if (m_markdown) {
    // Fenced code blocks span multiple lines; track them via block state. Code isn't spell checked.
    const bool fenceLine = fence.match(text).hasMatch();
    if (previousBlockState() == InCodeFence) {
      setFormat(0, text.length(), m_codeFormat);
      setCurrentBlockState(fenceLine ? Normal : InCodeFence);
      return;
    }
    if (fenceLine) {
      setFormat(0, text.length(), m_codeFormat);
      setCurrentBlockState(InCodeFence);
      return;
    }
    setCurrentBlockState(Normal);
    highlightMarkdown(text);
  }

  // Misspelled words: a wavy underline on top of whatever style the word has.
  for (const SpellChecker::Word &word : misspelledWords(text)) {
    for (qsizetype i = word.start; i < word.start + word.length; ++i) {
      QTextCharFormat f = format(static_cast<int>(i));
      f.merge(m_misspelledFormat);
      setFormat(static_cast<int>(i), 1, f);
    }
  }
}

void MarkdownHighlighter::highlightMarkdown(const QString &text)
{
  static const QRegularExpression heading(R"(^(#{1,6})\s)");
  static const QRegularExpression quote(R"(^\s*>)");
  static const QRegularExpression list(R"(^\s*([-*+]|\d+[.)])\s)");
  static const QRegularExpression hrule(R"(^\s*([-*_])(\s*\1){2,}\s*$)");

  if (auto m = heading.match(text); m.hasMatch()) {
    setFormat(0, text.length(), m_headingFormats[m.capturedLength(1) - 1]);
    return;
  }
  if (hrule.match(text).hasMatch()) {
    setFormat(0, text.length(), m_ruleFormat);
    return;
  }
  if (quote.match(text).hasMatch()) {
    setFormat(0, text.length(), m_quoteFormat);
  }
  if (auto m = list.match(text); m.hasMatch()) {
    setFormat(m.capturedStart(1), m.capturedLength(1), m_listFormat);
  }

  for (const Rule &rule : m_inlineRules) {
    auto it = rule.pattern.globalMatch(text);
    while (it.hasNext()) {
      const auto m = it.next();
      // Merge so inline styles layer on top of block styles (e.g. bold inside a quote).
      for (int i = m.capturedStart(); i < m.capturedEnd(); ++i) {
        QTextCharFormat f = format(i);
        f.merge(rule.format);
        setFormat(i, 1, f);
      }
    }
  }
}

QList<SpellChecker::Word> MarkdownHighlighter::misspelledWords(const QString &text) const
{
  QList<SpellChecker::Word> out;
  if (!m_spell || !m_spell->isLoaded()) {
    return out;
  }
  // Text that isn't prose: URLs and e-mail addresses, and in Markdown also code spans, link targets and tags.
  static const QRegularExpression plainSkip(QStringLiteral(R"((?:https?://|www\.)\S+|\S+@\S+\.\S+)"));
  static const QRegularExpression markdownSkip(
      QStringLiteral(R"(`[^`]*`|\]\([^)]*\)|<[^>\s][^>]*>|(?:https?://|www\.)\S+|\S+@\S+\.\S+)"));
  QList<QPair<qsizetype, qsizetype>> skipped;
  for (auto it = (m_markdown ? markdownSkip : plainSkip).globalMatch(text); it.hasNext();) {
    const auto m = it.next();
    skipped.append({m.capturedStart(), m.capturedEnd()});
  }
  for (const SpellChecker::Word &word : SpellChecker::words(text)) {
    const bool inSkipped = std::any_of(skipped.begin(), skipped.end(), [&](const auto &range) {
      return word.start < range.second && word.start + word.length > range.first;
    });
    if (!inSkipped && !m_spell->isCorrect(text.mid(word.start, word.length))) {
      out.append(word);
    }
  }
  return out;
}

void MarkdownHighlighter::setSpellChecker(const SpellChecker *checker)
{
  m_spell = checker;
  rehighlight();
}

void MarkdownHighlighter::refreshSpelling()
{
  rehighlight();
}

std::optional<SpellChecker::Word> MarkdownHighlighter::misspelledWordAt(const QTextBlock &block,
                                                                        qsizetype position) const
{
  if (!block.isValid() || (m_markdown && block.userState() == InCodeFence) ||
      (m_markdown && block.previous().isValid() && block.previous().userState() == InCodeFence)) {
    return std::nullopt; // code (a fence line, or inside a fenced block)
  }
  for (const SpellChecker::Word &word : misspelledWords(block.text())) {
    if (position >= word.start && position <= word.start + word.length) {
      return word;
    }
  }
  return std::nullopt;
}
