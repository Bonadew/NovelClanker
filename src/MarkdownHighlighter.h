#pragma once

#include "SpellChecker.h"

#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <optional>
#include <vector>

class QTextBlock;

// Highlights Markdown syntax in place while keeping the document plain text, and underlines misspelled words when
// a spell checker is set. With `markdown` false it only checks spelling (for plain text such as the synopsis).
class MarkdownHighlighter : public QSyntaxHighlighter
{
    Q_OBJECT

  public:
    explicit MarkdownHighlighter(QTextDocument *parent, bool markdown = true);

    // The spell checker to underline misspelled words with (shared, owned elsewhere); nullptr: no checking.
    // Highlights again; call refreshSpelling() after the checker's dictionary or words change.
    void setSpellChecker(const SpellChecker *checker);
    void refreshSpelling();
    // The misspelled word at `position` (in the block's text) of `block`, if any. Words in code, link targets
    // and URLs are never misspelled.
    std::optional<SpellChecker::Word> misspelledWordAt(const QTextBlock &block, qsizetype position) const;

    // Sizes headings from the document's current default font again (call after changing the editor's font).
    void refreshFonts();

  protected:
    void highlightBlock(const QString &text) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    // Colours that read well on the current palette's background (light or dark).
    void updateColors();
    void setHeadingSizes(); // relative to the document's default font
    void highlightMarkdown(const QString &text);
    // Misspelled words of a line (outside code fences), skipping code spans, link targets, URLs and e-mail addresses.
    QList<SpellChecker::Word> misspelledWords(const QString &text) const;

    struct Rule {
        QRegularExpression pattern;
        QTextCharFormat format;
    };

    enum BlockState { Normal = 0, InCodeFence = 1 };

    std::vector<Rule> m_inlineRules;
    QTextCharFormat m_headingFormats[6];
    QTextCharFormat m_quoteFormat;
    QTextCharFormat m_listFormat;
    QTextCharFormat m_codeFormat;
    QTextCharFormat m_ruleFormat;
    QTextCharFormat m_misspelledFormat;
    const SpellChecker *m_spell = nullptr;
    bool m_markdown = true;
};
