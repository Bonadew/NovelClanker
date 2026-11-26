#include "AiSettings.h"

#include <QSettings>

namespace {
constexpr auto kServerUrlKey = "lmStudio/serverUrl";
constexpr auto kPlannerModelKey = "lmStudio/plannerModel";
constexpr auto kProseModelKey = "lmStudio/proseModel";
constexpr auto kReasoningKey = "lmStudio/reasoning"; // "default", "low" or "off"
} // namespace

AiSettings AiSettings::load()
{
    QSettings settings;
    AiSettings result;
    result.serverUrl = settings.value(kServerUrlKey, kDefaultServerUrl).toString();
    result.plannerModel = settings.value(kPlannerModelKey).toString();
    result.proseModel = settings.value(kProseModelKey).toString();
    const QString reasoning = settings.value(kReasoningKey).toString();
    result.reasoning = reasoning == QLatin1String("low")   ? Reasoning::Low
                       : reasoning == QLatin1String("off") ? Reasoning::Off
                                                           : Reasoning::Default;
    return result;
}

void AiSettings::save() const
{
    QSettings settings;
    settings.setValue(kServerUrlKey, serverUrl);
    settings.setValue(kPlannerModelKey, plannerModel);
    settings.setValue(kProseModelKey, proseModel);
    settings.setValue(kReasoningKey, reasoning == Reasoning::Low   ? "low"
                                     : reasoning == Reasoning::Off ? "off"
                                                                   : "default");
}
