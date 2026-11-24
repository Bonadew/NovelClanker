#include "PlanChat.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>


namespace PlanChat {

namespace {

const QString kOpen = QStringLiteral("<plan>");
const QString kClose = QStringLiteral("</plan>");

QString trimNewlines(QString text)
{
    while (!text.isEmpty() && (text.front() == '\n' || text.front() == '\r')) {
        text.remove(0, 1);
    }
    while (!text.isEmpty() && text.back().isSpace()) {
        text.chop(1);
    }
    return text;
}

// Models sometimes wrap the plan in a Markdown code fence inside the block.
QString stripCodeFence(const QString &text)
{
    static const QRegularExpression fenced(QStringLiteral(R"(^\s*```[a-zA-Z]*\n(.*)\n```\s*$)"),
                                           QRegularExpression::DotMatchesEverythingOption);
    const auto m = fenced.match(text);
    return m.hasMatch() ? m.captured(1) : text;
}

} // namespace

QString serialize(const QList<Turn> &history)
{
    if (history.isEmpty()) {
        return {};
    }
    QJsonArray array;
    for (const Turn &turn : history) {
        QJsonObject obj{{"role", turn.role}, {"content", turn.content}};
        if (turn.planApplied) {
            obj.insert("planApplied", true);
        }
        array.append(obj);
    }
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

QList<Turn> parse(const QString &json)
{
    QList<Turn> history;
    for (const QJsonValue &value : QJsonDocument::fromJson(json.toUtf8()).array()) {
        const QJsonObject obj = value.toObject();
        const QString role = obj.value("role").toString();
        if (role == "user" || role == "assistant") {
            history.append({role, obj.value("content").toString(), obj.value("planApplied").toBool()});
        }
    }
    return history;
}

QString systemPrompt(const QString &synopsis, const QString &plan)
{
    static const QString instructions = QStringLiteral(
        R"PROMPT(You are an experienced developmental editor helping an author refine the plan for their novel. The synopsis and the current plan are given below; the plan may contain the author's own edits, and it is always the latest version.

How to answer:
- When the author asks for changes to the plan, first write one or two sentences saying what you changed. Then output the complete revised plan between a line containing only <plan> and a line containing only </plan>.
- The revised plan must be the whole plan from its title to its last section, not only the changed parts. Copy every part the author did not ask to change word for word: the title, premise, characters, settings, story spine, every act, chapter and scene, and the open questions. Never shorten, summarise or skip anything, and never write "..." or "(unchanged)" in place of content.
- Keep the plan's Markdown structure and headings (### Act N: ..., #### Chapter N: ..., - **Scene N: ...**) and keep chapter numbering continuous.
- When the author asks a question or wants to discuss the plan, answer briefly and do not output a <plan> block.
- Stay consistent with the synopsis unless the author asks to depart from it.
- Write in the same language as the plan.)PROMPT");
    return instructions + "\n\n<synopsis>\n" + synopsis + "\n</synopsis>\n\n<current_plan>\n" + plan +
           "\n</current_plan>";
}

QList<ChatMessage> messages(const QString &synopsis, const QString &plan, const QList<Turn> &history,
                            const QString &userMessage)
{
    QList<ChatMessage> out{{QStringLiteral("system"), systemPrompt(synopsis, plan)}};
    for (const Turn &turn : history) {
        QString content = turn.content;
        if (turn.planApplied) {
            // The revised plan itself isn't repeated: it is the current plan in the system prompt.
            content += QStringLiteral("\n\n(I revised the plan; the current plan above includes this revision.)");
        }
        out.append({turn.role, content});
    }
    // Small models follow instructions placed close to the end much better.
    out.append({QStringLiteral("user"),
                userMessage + QStringLiteral("\n\n(If you change the plan, reply with a short summary and then the "
                                             "complete plan, every part of it, between <plan> and </plan>.)")});
    return out;
}

QString incompleteReason(const QString &current, const QString &revised)
{
    static const QRegularExpression act(QStringLiteral(R"(^###\s+Act\b)"),
                                        QRegularExpression::MultilineOption | QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression chapter(QStringLiteral(R"(^####\s+Chapter\b)"),
                                            QRegularExpression::MultilineOption |
                                                QRegularExpression::CaseInsensitiveOption);
    const auto count = [](const QRegularExpression &re, const QString &text) {
        int n = 0;
        for (auto it = re.globalMatch(text); it.hasNext(); it.next()) {
            ++n;
        }
        return n;
    };
    const int actsBefore = count(act, current), actsAfter = count(act, revised);
    const int chaptersBefore = count(chapter, current), chaptersAfter = count(chapter, revised);
    if (actsBefore > 0 && actsAfter < actsBefore) {
        return QCoreApplication::translate("PlanChat", "it contains %1 of the plan's %2 acts")
            .arg(actsAfter)
            .arg(actsBefore);
    }
    if (chaptersBefore >= 4 && chaptersAfter * 2 < chaptersBefore) {
        return QCoreApplication::translate("PlanChat", "it contains %1 of the plan's %2 chapters")
            .arg(chaptersAfter)
            .arg(chaptersBefore);
    }
    if (current.size() >= 400 && revised.size() * 2 < current.size()) {
        return QCoreApplication::translate("PlanChat", "it is less than half as long as the current plan");
    }
    return {};
}

Reply parseReply(const QString &reply)
{
    Reply result;
    const qsizetype open = reply.indexOf(kOpen);
    if (open < 0) {
        result.message = reply.trimmed();
        return result;
    }
    const qsizetype close = reply.indexOf(kClose, open + kOpen.size());
    if (close < 0) {
        result.message = reply.left(open).trimmed();
        result.planCutOff = true;
        return result;
    }
    const QString plan = trimNewlines(stripCodeFence(reply.mid(open + kOpen.size(), close - open - kOpen.size())));
    result.message = (reply.left(open).trimmed() + "\n\n" + reply.mid(close + kClose.size()).trimmed()).trimmed();
    if (!plan.trimmed().isEmpty()) {
        result.plan = plan;
    }
    return result;
}

QString streamingMessage(const QString &partialReply, int *planWords)
{
    static const QRegularExpression whitespace(QStringLiteral(R"(\s+)"));
    const qsizetype open = partialReply.indexOf(kOpen);
    if (planWords) {
        *planWords = 0;
    }
    if (open < 0) {
        // Hide a "<plan>" tag that is still arriving.
        QString text = partialReply;
        for (qsizetype n = kOpen.size() - 1; n > 0; --n) {
            if (text.endsWith(QStringView(kOpen).left(n))) {
                text.chop(n);
                break;
            }
        }
        return text.trimmed();
    }
    const qsizetype close = partialReply.indexOf(kClose, open);
    const QString plan = partialReply.mid(open + kOpen.size(), close < 0 ? -1 : close - open - kOpen.size());
    if (planWords) {
        *planWords = static_cast<int>(plan.split(whitespace, Qt::SkipEmptyParts).size());
    }
    QString message = partialReply.left(open).trimmed();
    if (close >= 0) {
        message = (message + "\n\n" + partialReply.mid(close + kClose.size()).trimmed()).trimmed();
    }
    return message;
}

} // namespace PlanChat
