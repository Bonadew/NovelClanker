#pragma once

#include "LmStudioClient.h"

#include <QList>
#include <QString>

class QComboBox;

// Helpers for model dropdowns (Settings view, Synopsis view). Item data holds the model id.
namespace ModelCombo {

// Configures a combo box for listing models (width, placeholder).
void setUp(QComboBox *combo);

// Fills the combo with `models`, keeping `selected` (a model id) selected even when the server doesn't list
// it. Before anything was fetched (`fetched` false) a missing model is shown plainly; afterwards it is
// flagged as not available on the server.
void populate(QComboBox *combo, const QList<LmModel> &models, const QString &selected, bool fetched);

// Id of the selected model; empty if nothing (or an entry without a model id) is selected.
QString selectedId(const QComboBox *combo);

} // namespace ModelCombo
