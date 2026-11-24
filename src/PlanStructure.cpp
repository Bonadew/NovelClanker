#include "PlanStructure.h"

#include <QRegularExpression>
#include <QStringList>

namespace PlanStructure {

namespace {

using Re = QRegularExpression;
constexpr auto kCi = QRegularExpression::CaseInsensitiveOption;

// "**Title**:" -> "Title"; removes emphasis markers and trailing separators.
QString cleanTitle(QString text)
{
    text.remove(QStringLiteral("**"));
    text.remove(QStringLiteral("__"));
    text = text.trimmed();
    static const Re trailing(QStringLiteral(R"([\s:.\-–—]+$)"));
    text.remove(trailing);
    static const Re leading(QStringLiteral(R"(^[\s:.\-–—]+)"));
    text.remove(leading);
    return text.trimmed();
}

QString join(const QString &a, const QString &b)
{
    return a.isEmpty() ? b : b.isEmpty() ? a : a + " " + b;
}

struct Heading {
    int level = 0; // 0: not a heading
    QString text;
};

// Markdown headings, plus lines that are entirely bold ("**Chapter 1: Title**"), which models use too.
Heading heading(const QString &line)
{
    static const Re atx(QStringLiteral(R"(^\s{0,3}(#{1,6})\s*(.*?)\s*#*\s*$)"));
    static const Re bold(QStringLiteral(R"(^\s*\*\*([^*]+)\*\*\s*:?\s*$)"));
    if (const auto m = atx.match(line); m.hasMatch()) {
        return {static_cast<int>(m.capturedLength(1)), m.captured(2)};
    }
    if (const auto m = bold.match(line); m.hasMatch()) {
        return {7, m.captured(1)}; // below every real heading level
    }
    return {};
}

// "Act 2: Title" / "Chapter 11.5 - Title" / "Scene 1: Title" -> number and title.
bool numbered(const QString &text, const QString &word, QString &number, QString &title, QString *raw = nullptr)
{
    const Re re(QStringLiteral(R"(^\s*(?:\*\*)?\s*%1\s+([0-9IVXLCivxlc]+(?:\.[0-9]+)?)\b\s*(?:[:.\-–—]\s*)?(.*)$)")
                    .arg(word),
                kCi);
    const auto m = re.match(text);
    if (!m.hasMatch()) {
        return false;
    }
    number = m.captured(1);
    title = cleanTitle(m.captured(2));
    if (raw) {
        *raw = m.captured(2).trimmed();
    }
    return true;
}

// A list item describing a scene. Returns false if the line isn't one.
bool sceneItem(const QString &line, QString &title, QString &brief)
{
    static const Re item(QStringLiteral(R"(^\s*(?:[-*+]|\d+[.)])\s+(.*)$)"));
    const auto m = item.match(line);
    if (!m.hasMatch()) {
        return false;
    }
    const QString body = m.captured(1).trimmed();
    QString number;
    if (body.startsWith(QStringLiteral("**"))) {
        // - **Scene 1: Title**: brief
        const qsizetype close = body.indexOf(QStringLiteral("**"), 2);
        if (close < 0) {
            return false;
        }
        if (!numbered(body.mid(2, close - 2), QStringLiteral("Scene"), number, title)) {
            return false;
        }
        // The separator after the bold part (": ", " - ", ...) isn't part of the brief.
        static const Re leadingSep(QStringLiteral(R"(^[:.\-–—]\s*)"));
        brief = body.mid(close + 2).trimmed().remove(leadingSep).trimmed();
    } else {
        // - Scene 1: Title: brief   /   - Scene 1 - Title — brief
        QString cleaned, rest;
        if (!numbered(body, QStringLiteral("Scene"), number, cleaned, &rest)) {
            return false;
        }
        static const Re split(QStringLiteral(R"(^(.*?)(?:\s*[:—–]\s+|\s+-\s+)(.*)$)"));
        if (const auto s = split.match(rest); s.hasMatch()) {
            title = cleanTitle(s.captured(1));
            brief = s.captured(2).trimmed();
        } else {
            title = cleaned;
            brief.clear();
        }
    }
    if (title.size() > 80) { // not a title but a description
        brief = join(title, brief);
        title.clear();
    }
    if (title.isEmpty()) {
        title = QStringLiteral("Scene %1").arg(number);
    }
    return true;
}

} // namespace

QList<Act> parse(const QString &plan)
{
    QStringList lines = plan.split('\n');
    for (QString &line : lines) {
        if (line.endsWith('\r')) {
            line.chop(1);
        }
    }

    // Only the "Structure" section, if there is one.
    qsizetype begin = 0, end = lines.size();
    static const Re structure(QStringLiteral(R"(^\s*(?:\*\*)?\s*(?:the\s+)?structure\b)"), kCi);
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const Heading h = heading(lines[i]);
        if (h.level > 0 && h.level < 7 && structure.match(h.text).hasMatch()) {
            begin = i + 1;
            for (qsizetype j = begin; j < lines.size(); ++j) {
                const Heading next = heading(lines[j]);
                if (next.level > 0 && next.level <= h.level) {
                    end = j;
                    break;
                }
            }
            break;
        }
    }

    QList<Act> acts;
    enum class Target { None, Act, Chapter, Scene } target = Target::None;
    const auto currentAct = [&]() -> Act & {
        if (acts.isEmpty()) {
            acts.append({QStringLiteral("Act 1"), {}, {}});
        }
        return acts.last();
    };
    const auto currentChapter = [&]() -> Chapter & {
        Act &act = currentAct();
        if (act.chapters.isEmpty()) {
            int number = 1;
            for (const Act &a : acts) {
                number += static_cast<int>(a.chapters.size());
            }
            act.chapters.append({QStringLiteral("Chapter %1").arg(number), {}, {}});
        }
        return act.chapters.last();
    };

    for (qsizetype i = begin; i < end; ++i) {
        const QString &line = lines[i];
        if (line.trimmed().isEmpty()) {
            if (target == Target::Scene) {
                target = Target::None; // a scene's brief is a single paragraph
            }
            continue;
        }
        QString number, title, brief;
        if (const Heading h = heading(line); h.level > 0) {
            if (numbered(h.text, QStringLiteral("Act"), number, title)) {
                acts.append({title.isEmpty() ? QStringLiteral("Act %1").arg(number)
                                             : QStringLiteral("Act %1: %2").arg(number, title),
                             {}, {}});
                target = Target::Act;
            } else if (numbered(h.text, QStringLiteral("Chapter"), number, title)) {
                currentAct().chapters.append({title.isEmpty() ? QStringLiteral("Chapter %1").arg(number)
                                                              : QStringLiteral("Chapter %1: %2").arg(number, title),
                                              {}, {}});
                target = Target::Chapter;
            } else if (numbered(h.text, QStringLiteral("Scene"), number, title)) {
                currentChapter().scenes.append({title.isEmpty() ? QStringLiteral("Scene %1").arg(number) : title, {}});
                target = Target::Scene;
            } else {
                target = Target::None; // some other heading inside the structure
            }
            continue;
        }
        if (sceneItem(line, title, brief)) {
            currentChapter().scenes.append({title, brief});
            target = Target::Scene;
            continue;
        }
        // Plain text: the brief of the act/chapter/scene above it.
        const QString text = line.trimmed();
        switch (target) {
        case Target::Act:
            acts.last().brief = join(acts.last().brief, text);
            break;
        case Target::Chapter:
            acts.last().chapters.last().brief = join(acts.last().chapters.last().brief, text);
            break;
        case Target::Scene: {
            Scene &scene = acts.last().chapters.last().scenes.last();
            static const Re bullet(QStringLiteral(R"(^[-*+]\s+)"));
            scene.brief = join(scene.brief, QString(text).remove(bullet));
            break;
        }
        case Target::None:
            break;
        }
    }

    for (Act &act : acts) {
        for (Chapter &chapter : act.chapters) {
            if (chapter.scenes.isEmpty()) {
                chapter.scenes.append({QStringLiteral("Scene 1"), chapter.brief});
            }
        }
    }
    return acts;
}

Counts count(const QList<Act> &acts)
{
    Counts c;
    c.acts = static_cast<int>(acts.size());
    for (const Act &act : acts) {
        c.chapters += static_cast<int>(act.chapters.size());
        for (const Chapter &chapter : act.chapters) {
            c.scenes += static_cast<int>(chapter.scenes.size());
        }
    }
    return c;
}

} // namespace PlanStructure
