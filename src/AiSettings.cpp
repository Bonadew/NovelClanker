#include "AiSettings.h"

#include <QSettings>

namespace {
constexpr auto kServerUrlKey = "lmStudio/serverUrl";
constexpr auto kPlannerModelKey = "lmStudio/plannerModel";
constexpr auto kProseModelKey = "lmStudio/proseModel";
} // namespace

AiSettings AiSettings::load()
{
    QSettings settings;
    AiSettings result;
    result.serverUrl = settings.value(kServerUrlKey, kDefaultServerUrl).toString();
    result.plannerModel = settings.value(kPlannerModelKey).toString();
    result.proseModel = settings.value(kProseModelKey).toString();
    return result;
}

void AiSettings::save() const
{
    QSettings settings;
    settings.setValue(kServerUrlKey, serverUrl);
    settings.setValue(kPlannerModelKey, plannerModel);
    settings.setValue(kProseModelKey, proseModel);
}
