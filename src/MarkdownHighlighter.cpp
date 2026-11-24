#include "MarkdownHighlighter.h"

#include <QFontDatabase>

MarkdownHighlighter::MarkdownHighlighter(QTextDocument *parent) : QSyntaxHighlighter(parent)
{
    const qreal headingScale[6] = {1.8, 1.5, 1.3, 1.15, 1.05, 1.0};
    for (int i = 0; i < 6; ++i) {
        m_headingFormats[i].setFontWeight(QFont::Bold);
        m_headingFormats[i].setFontPointSize(parent->defaultFont().pointSizeF() * headingScale[i]);
        m_headingFormats[i].setForeground(QColor(0x2a, 0x5d, 0x9f));
    }

    m_quoteFormat.setForeground(QColor(0x6a, 0x73, 0x7d));
    m_quoteFormat.setFontItalic(true);

    m_listFormat.setForeground(QColor(0xb0, 0x5a, 0x00));
    m_listFormat.setFontWeight(QFont::Bold);

    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_codeFormat.setFontFamilies(mono.families());
    m_codeFormat.setForeground(QColor(0x8a, 0x2b, 0x8a));

    m_ruleFormat.setForeground(QColor(0x99, 0x99, 0x99));

    QTextCharFormat bold;
    bold.setFontWeight(QFont::Bold);
    QTextCharFormat italic;
    italic.setFontItalic(true);
    QTextCharFormat strike;
    strike.setFontStrikeOut(true);
    QTextCharFormat link;
    link.setForeground(QColor(0x1a, 0x73, 0xe8));
    link.setFontUnderline(true);

    m_inlineRules = {
        {QRegularExpression(R"((\*\*|__)(?=\S)(.+?)(?<=\S)\1)"), bold},
        {QRegularExpression(R"((?<![\*_])([\*_])(?=\S)(.+?)(?<=\S)\1(?![\*_]))"), italic},
        {QRegularExpression(R"(~~(?=\S)(.+?)(?<=\S)~~)"), strike},
        {QRegularExpression(R"(`[^`]+`)"), m_codeFormat},
        {QRegularExpression(R"(\[[^\]]*\]\([^\)]*\))"), link},
    };
}

void MarkdownHighlighter::highlightBlock(const QString &text)
{
    static const QRegularExpression fence(R"(^\s*(```|~~~))");
    static const QRegularExpression heading(R"(^(#{1,6})\s)");
    static const QRegularExpression quote(R"(^\s*>)");
    static const QRegularExpression list(R"(^\s*([-*+]|\d+[.)])\s)");
    static const QRegularExpression hrule(R"(^\s*([-*_])(\s*\1){2,}\s*$)");

    // Fenced code blocks span multiple lines; track them via block state.
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
