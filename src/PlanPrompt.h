#pragma once

#include "LmChat.h"

#include <QList>
#include <QString>

// Prompt for turning a synopsis into a novel plan with the planner model.
//
// The plan is Markdown with a fixed shape: premise, characters, settings, then Structure with
// "### Act N: ...", "#### Chapter N: ..." headings and one "- **Scene N: ...**" bullet per scene, matching the
// app's acts -> chapters -> scenes outline so it can later drive outlining and the prose writer.
namespace PlanPrompt {

// Moderate creativity: inventive enough to fill gaps, disciplined enough to follow the format.
inline constexpr double kTemperature = 0.6;

QString systemPrompt();
// The user message; the synopsis is embedded verbatim.
QString userPrompt(const QString &synopsis);
QList<ChatMessage> messages(const QString &synopsis);

} // namespace PlanPrompt
