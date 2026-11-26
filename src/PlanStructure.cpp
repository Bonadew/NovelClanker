#include "PlanStructure.h"

#include <QRegularExpression>
#include <QStringList>

namespace PlanStructure {

namespace {

using Re = QRegularExpression;
constexpr auto kCi = QRegularExpression::CaseInsensitiveOption;

// "**Title**:" -> "Title"; removes emphasis markers and surrounding separators.
QString cleanTitle(QString text)
{
    text.remove(QStringLiteral("**"));
    text.remove(QStringLiteral("__"));
    text = text.trimmed();
    static const Re trailing(QStringLiteral(R"([\s:.\-–—]+$)"));
    text.remove(trailing);
    static const Re leading(QStringLiteral(R"(^[\s:.\-–—]+)"));
    text.remove(leading);
    text = text.trimmed();
    // Models often quote titles: "The Storm", “The Storm”, «The Storm». Only a matching pair around the whole
    // title is removed, so "'Twas Night" or “Run,” She Said keep theirs.
    static const QStringList pairs{QStringLiteral("\"\""), QStringLiteral("''"), QStringLiteral("“”"),
                                   QStringLiteral("‘’"), QStringLiteral("„“"), QStringLiteral("„”"),
                                   QStringLiteral("«»"), QStringLiteral("»«")};
    for (const QString &pair : pairs) {
        if (text.size() > 2 && text.front() == pair[0] && text.back() == pair[1] &&
            !text.mid(1, text.size() - 2).contains(pair[1])) {
            return text.mid(1, text.size() - 2).trimmed();
        }
    }
    return text;
}

// Removes a leading separator (": ", " - ", "— ") from text that follows a title.
QString afterSeparator(QString text)
{
    static const Re leading(QStringLiteral(R"(^\s*(?:\*\*)?\s*[:.\-–—]?\s*)"));
    return text.remove(leading).trimmed();
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

enum class Kind { None, Act, Chapter, Scene };

// A line that starts an act, chapter or scene.
struct Marker {
    Kind kind = Kind::None;
    QString word;   // act-level parts keep their own word: "Act", "Stage", "Part", "Book"
    QString number; // as written: "2", "IV", "11.5", "1.2"
    QString title;  // may be empty
    QString brief;  // text on the same line after the title; may be empty
};

// "Act 2", "Chapter 11.5", "Scene 1.2" at the start of `text` (optionally bold). Captures the kind word, the
// number and the remainder.
const Re &markerStart()
{
    static const Re re(
        QStringLiteral(R"(^\s*(\*\*)?\s*(Act|Part|Stage|Book|Chapter|Scene)\s+([0-9IVXLCivxlc]+(?:\.[0-9]+)*)\b(.*)$)"), kCi);
    return re;
}

// Splits "Title: what happens" / "Title - what happens" / "Title — what happens" into title and brief. A
// remainder that reads like a description rather than a title becomes the brief.
void splitTitle(const QString &rest, QString &title, QString &brief)
{
    static const Re split(QStringLiteral(R"(^(.*?)(?:\s*[:—–]\s+|\s+-\s+)(.*)$)"));
    if (const auto s = split.match(rest); s.hasMatch()) {
        title = cleanTitle(s.captured(1));
        brief = s.captured(2).trimmed();
    } else {
        title = cleanTitle(rest);
        brief.clear();
    }
    // "Scene 1.1: The keeper climbs the tower at dusk." has no title, only a description.
    static const Re whitespace(QStringLiteral(R"(\s+)"));
    const QString whole = rest.trimmed();
    const bool sentence = whole.endsWith('.') || whole.endsWith('!') || whole.endsWith('?');
    const qsizetype words = title.split(whitespace, Qt::SkipEmptyParts).size();
    if (title.size() > 80 || (brief.isEmpty() && (sentence || words > 8))) {
        brief = whole;
        title.clear();
    }
}

// Recognises an act/chapter/scene marker written as a heading ("### Act 1: Title"), a bold line, a list item
// ("- **Chapter 2 - Title**", "  - Scene 1.1: what happens", "1. Scene 3: Title: what happens") or a plain line
// ("Chapter 2: Title").
Marker marker(const QString &line)
{
    QString body;
    bool fromHeading = false;
    static const Re listItem(QStringLiteral(R"(^\s*(?:[-*+]|\d+[.)])\s+(.*)$)"));
    if (const Heading h = heading(line); h.level > 0) {
        body = h.text;
        fromHeading = true;
    } else if (const auto m = listItem.match(line); m.hasMatch()) {
        body = m.captured(1);
    } else {
        body = line;
    }

    const auto m = markerStart().match(body);
    if (!m.hasMatch()) {
        return {};
    }
    Marker result;
    const QString word = m.captured(2).toLower();
    result.kind = word == "chapter" ? Kind::Chapter : word == "scene" ? Kind::Scene : Kind::Act;
    result.word = word.left(1).toUpper() + word.mid(1);
    result.number = m.captured(3);
    QString rest = m.captured(4);

    if (!m.captured(1).isEmpty()) {
        // Bold: "**Scene 1: Title**: brief" / "**Chapter 2 - Title** - brief" / "**Scene 1:** brief"
        const qsizetype close = rest.indexOf(QStringLiteral("**"));
        const QString inside = close < 0 ? rest : rest.left(close);
        const QString after = close < 0 ? QString() : afterSeparator(rest.mid(close + 2));
        result.title = cleanTitle(inside);
        result.brief = after;
        if (result.title.size() > 80) {
            result.brief = join(result.title, result.brief);
            result.title.clear();
        }
    } else if (fromHeading && result.kind != Kind::Scene) {
        // "### Act 1: Title" - the whole remainder is the title
        result.title = cleanTitle(rest);
    } else {
        // Without markup a marker must be followed by a separator (or end the line): "- Chapter 2: Title" or
        // "Chapter 2" is a marker, "- Chapter 2 is where it turns" is a sentence.
        static const Re separatorNext(QStringLiteral(R"(^\s*([:.\-–—]|$))"));
        if (!fromHeading && !separatorNext.match(rest).hasMatch()) {
            return {};
        }
        splitTitle(afterSeparator(rest), result.title, result.brief);
    }
    if (result.title.size() > 80) {
        result.brief = join(result.title, result.brief);
        result.title.clear();
    }
    return result;
}

// Models (gpt-oss in particular) write typographic characters that the patterns would not match:
// "Chapter 10", "Scene 1", "non‑breaking hyphens". Map them to plain equivalents; for briefs
// this makes no visible difference.
QString normalize(const QString &text)
{
    QString out;
    out.reserve(text.size());
    for (const QChar ch : text) {
        const char16_t c = ch.unicode();
        if (c == u'\n' || c == u'\r') {
            out += ch;
        } else if (c == 0x200B || c == 0x200C || c == 0x200D || c == 0x2060 || c == 0xFEFF) {
            continue; // zero-width characters and byte-order marks
        } else if (ch.isSpace()) {
            out += u' '; // tabs, no-break and narrow spaces, thin spaces, ...
        } else if (c == 0x2010 || c == 0x2011 || c == 0x2212) {
            out += u'-'; // hyphen, non-breaking hyphen, minus sign
        } else if (c == 0x2012) {
            out += u'–'; // figure dash
        } else {
            out += ch;
        }
    }
    return out;
}

// "1.2" -> "2": scene numbers are often written as chapter.scene.
QString lastNumber(const QString &number)
{
    return number.section('.', -1);
}

} // namespace

QList<Act> parse(const QString &plan)
{
    QStringList lines = normalize(plan).split('\n');
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
    const auto chapterCount = [&] {
        int n = 0;
        for (const Act &a : acts) {
            n += static_cast<int>(a.chapters.size());
        }
        return n;
    };
    const auto currentChapter = [&]() -> Chapter & {
        Act &act = currentAct();
        if (act.chapters.isEmpty()) {
            act.chapters.append({QStringLiteral("Chapter %1").arg(chapterCount() + 1), {}, {}});
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
        const Marker m = marker(line);
        switch (m.kind) {
        case Kind::Act:
            acts.append({m.title.isEmpty() ? QStringLiteral("%1 %2").arg(m.word, m.number)
                                           : QStringLiteral("%1 %2: %3").arg(m.word, m.number, m.title),
                         m.brief, {}});
            target = Target::Act;
            continue;
        case Kind::Chapter:
            currentAct().chapters.append({m.title.isEmpty() ? QStringLiteral("Chapter %1").arg(m.number)
                                                            : QStringLiteral("Chapter %1: %2").arg(m.number, m.title),
                                          m.brief, {}});
            target = Target::Chapter;
            continue;
        case Kind::Scene:
            currentChapter().scenes.append(
                {m.title.isEmpty() ? QStringLiteral("Scene %1").arg(lastNumber(m.number)) : m.title, m.brief});
            target = Target::Scene;
            continue;
        case Kind::None:
            break;
        }
        if (heading(line).level > 0) {
            target = Target::None; // some other heading inside the structure
            continue;
        }
        // Plain text: the brief of the act/chapter/scene above it.
        static const Re bullet(QStringLiteral(R"(^[-*+]\s+)"));
        const QString text = line.trimmed().remove(bullet);
        switch (target) {
        case Target::Act:
            acts.last().brief = join(acts.last().brief, text);
            break;
        case Target::Chapter:
            acts.last().chapters.last().brief = join(acts.last().chapters.last().brief, text);
            break;
        case Target::Scene: {
            Scene &scene = acts.last().chapters.last().scenes.last();
            scene.brief = join(scene.brief, text);
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
