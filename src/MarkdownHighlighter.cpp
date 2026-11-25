#include "MarkdownHighlighter.h"

#include "Theme.h"

#include <QApplication>
#include <QEvent>
#include <QFontDatabase>

namespace {
// Indexes into m_inlineRules for the rules whose colour depends on the theme.
constexpr std::size_t kCodeRule = 3;
constexpr std::size_t kLinkRule = 4;
} // namespace

MarkdownHighlighter::MarkdownHighlighter(QTextDocument *parent) : QSyntaxHighlighter(parent)
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
        {QRegularExpression(R"(`[^`]+`)"), m_codeFormat},       // kCodeRule
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
