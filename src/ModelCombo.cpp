#include "ModelCombo.h"

#include <QComboBox>
#include <QCoreApplication>

namespace ModelCombo {

namespace {
QString tr(const char *text)
{
    return QCoreApplication::translate("ModelCombo", text);
}
} // namespace

void setUp(QComboBox *combo)
{
    combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    combo->setMinimumContentsLength(40);
    combo->setPlaceholderText(tr("Select a model"));
}

void populate(QComboBox *combo, const QList<LmModel> &models, const QString &selected, bool fetched)
{
    combo->clear();
    bool found = false;
    for (const LmModel &model : models) {
        QString text = model.displayName;
        if (!model.details.isEmpty()) {
            text += QStringLiteral("  (%1)").arg(model.details);
        }
        if (model.loaded) {
            text += tr("  · loaded");
        }
        combo->addItem(text, model.id);
        combo->setItemData(combo->count() - 1, model.id, Qt::ToolTipRole);
        found = found || model.id == selected;
    }
    if (!selected.isEmpty() && !found) {
        // Keep the saved choice rather than silently dropping it (e.g. while the server is unreachable).
        combo->insertItem(0, fetched ? tr("%1  (not on this server)").arg(selected) : selected, selected);
    }
    combo->setCurrentIndex(selected.isEmpty() ? -1 : combo->findData(selected));
}

QString selectedId(const QComboBox *combo)
{
    return combo->currentIndex() >= 0 ? combo->currentData().toString() : QString();
}

} // namespace ModelCombo
