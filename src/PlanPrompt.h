#pragma once

#include "LmChat.h"

#include <QList>
#include <QString>

// Prompts for turning a synopsis into a novel plan with the planner model.
//
// A plan follows one of several templates (the structural model the novel is planned in). Every template
// produces Markdown with a fixed shape whose "## Structure" section uses "#### Chapter N: ..." headings and
// "- **Scene N: ...**" lines under act-level parts ("### Act N: ..." or "### Stage N: ..."), so the app can
// build the manuscript's acts -> chapters -> scenes from any of them.
namespace PlanPrompt {

// Moderate creativity: inventive enough to fill gaps, disciplined enough to follow the format.
inline constexpr double kTemperature = 0.6;

struct Template {
    QString id;          // stable identifier stored with novels and plans
    QString name;        // shown in the template dropdown
    QString description; // one or two sentences: what kind of novel it suits
};

inline const QString kThreeActTemplate = QStringLiteral("three-act");
inline const QString kInnerStagesTemplate = QStringLiteral("inner-stages");

// All templates, in the order they are offered. The first is the default.
const QList<Template> &templates();
// The template with this id, or the default one for an unknown or empty id.
const Template &templateById(const QString &id);

QString systemPrompt();
// The user message for `templateId`; the synopsis is embedded verbatim.
QString userPrompt(const QString &synopsis, const QString &templateId = kThreeActTemplate);
QList<ChatMessage> messages(const QString &synopsis, const QString &templateId = kThreeActTemplate);

} // namespace PlanPrompt
