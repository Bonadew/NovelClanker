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

  protected:
    void highlightBlock(const QString &text) override;

  private:
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
