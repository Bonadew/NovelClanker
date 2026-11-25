#pragma once

#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <vector>

// Highlights Markdown syntax in place while keeping the document plain text.
class MarkdownHighlighter : public QSyntaxHighlighter
{
    Q_OBJECT

  public:
    explicit MarkdownHighlighter(QTextDocument *parent);

    // Sizes headings from the document's current default font again (call after changing the editor's font).
    void refreshFonts();

  protected:
    void highlightBlock(const QString &text) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    // Colours that read well on the current palette's background (light or dark).
    void updateColors();
    void setHeadingSizes(); // relative to the document's default font

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
};
