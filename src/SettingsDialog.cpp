#include "SettingsDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

namespace {

QComboBox *makeModelCombo(QWidget *parent)
{
    auto *combo = new QComboBox(parent);
    combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    combo->setMinimumContentsLength(40);
    combo->setPlaceholderText(QObject::tr("Select a model"));
    return combo;
}

QLabel *makeHint(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setEnabled(false); // greyed-out helper text
    return label;
}

} // namespace

SettingsDialog::SettingsDialog(QWidget *parent) : QDialog(parent), m_client(new LmStudioClient(this))
{
    setWindowTitle(tr("Settings"));
    const AiSettings settings = AiSettings::load();

    // --- LM Studio server ------------------------------------------------------------------------------
    m_serverEdit = new QLineEdit(settings.serverUrl, this);
    m_serverEdit->setPlaceholderText(AiSettings::kDefaultServerUrl);
    m_serverEdit->setToolTip(tr("Address of the LM Studio server. Leave empty for %1.")
                                 .arg(AiSettings::kDefaultServerUrl));
    m_refreshButton = new QPushButton(tr("Refresh"), this);
    m_refreshButton->setToolTip(tr("Connect to the server and fetch its models"));
    m_refreshButton->setAutoDefault(false);
    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_serverEdit->setObjectName("serverEdit");
    m_refreshButton->setObjectName("refreshButton");
    m_statusLabel->setObjectName("statusLabel");

    auto *serverRow = new QHBoxLayout;
    serverRow->addWidget(m_serverEdit, 1);
    serverRow->addWidget(m_refreshButton);

    auto *serverBox = new QGroupBox(tr("LM Studio server"), this);
    auto *serverForm = new QFormLayout(serverBox);
    serverForm->addRow(tr("Address:"), serverRow);
    serverForm->addRow(QString(), m_statusLabel);

    // --- Models ----------------------------------------------------------------------------------------
    m_plannerCombo = makeModelCombo(this);
    m_proseCombo = makeModelCombo(this);
    m_plannerCombo->setObjectName("plannerCombo");
    m_proseCombo->setObjectName("proseCombo");
    populate(m_plannerCombo, {}, settings.plannerModel, true);
    populate(m_proseCombo, {}, settings.proseModel, true);

    auto *modelBox = new QGroupBox(tr("Models"), this);
    auto *modelForm = new QFormLayout(modelBox);
    modelForm->addRow(tr("Orchestrator / planner:"), m_plannerCombo);
    modelForm->addRow(QString(), makeHint(tr("Plans the story and coordinates the writing."), this));
    modelForm->addRow(tr("Prose writer:"), m_proseCombo);
    modelForm->addRow(QString(), makeHint(tr("Writes the scene text."), this));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &SettingsDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(serverBox);
    layout->addWidget(modelBox);
    layout->addStretch();
    layout->addWidget(buttons);

    connect(m_refreshButton, &QPushButton::clicked, this, &SettingsDialog::refreshModels);
    connect(m_serverEdit, &QLineEdit::editingFinished, this, [this] {
        if (m_serverEdit->isModified()) {
            m_serverEdit->setModified(false);
            refreshModels();
        }
    });
    connect(m_client, &LmStudioClient::modelsFetched, this, &SettingsDialog::onModelsFetched);
    connect(m_client, &LmStudioClient::fetchFailed, this, &SettingsDialog::onFetchFailed);

    resize(560, sizeHint().height());
    refreshModels();
}

void SettingsDialog::refreshModels()
{
    const QString input = m_serverEdit->text().trimmed();
    const QUrl url = normalizeServerUrl(input.isEmpty() ? AiSettings::kDefaultServerUrl : input);
    if (!url.isValid()) {
        m_client->cancel();
        m_statusLabel->setText(tr("\"%1\" is not a valid server address, e.g. %2.")
                                   .arg(input, AiSettings::kDefaultServerUrl));
        return;
    }
    m_statusLabel->setText(tr("Connecting to %1...").arg(url.toDisplayString()));
    m_client->fetchModels(url);
}

void SettingsDialog::onModelsFetched(const QList<LmModel> &models)
{
    const int loaded = static_cast<int>(std::count_if(models.begin(), models.end(), [](const LmModel &m) {
        return m.loaded;
    }));
    m_statusLabel->setText(models.isEmpty() ? tr("Connected, but the server has no models. Download one in LM Studio.")
                                            : tr("Connected: %n model(s) available", nullptr, int(models.size())) +
                                                  (loaded ? tr(", %n loaded.", nullptr, loaded) : QStringLiteral(".")));
    populate(m_plannerCombo, models, selectedModel(m_plannerCombo), false);
    populate(m_proseCombo, models, selectedModel(m_proseCombo), false);
}

void SettingsDialog::onFetchFailed(const QString &error)
{
    m_statusLabel->setText(tr("Could not get models: %1\nIs LM Studio running with its server started?").arg(error));
}

void SettingsDialog::populate(QComboBox *combo, const QList<LmModel> &models, const QString &selected,
                              bool initial)
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
        combo->insertItem(0, initial ? selected : tr("%1  (not on this server)").arg(selected), selected);
    }
    combo->setCurrentIndex(selected.isEmpty() ? -1 : combo->findData(selected));
}

QString SettingsDialog::selectedModel(const QComboBox *combo)
{
    return combo->currentIndex() >= 0 ? combo->currentData().toString() : QString();
}

void SettingsDialog::accept()
{
    const QString input = m_serverEdit->text().trimmed();
    const QUrl url = normalizeServerUrl(input.isEmpty() ? AiSettings::kDefaultServerUrl : input);
    if (!url.isValid()) {
        QMessageBox::warning(this, tr("Settings"),
                             tr("\"%1\" is not a valid server address, e.g. %2.").arg(input, AiSettings::kDefaultServerUrl));
        m_serverEdit->setFocus();
        return;
    }
    AiSettings settings;
    settings.serverUrl = url.toString();
    settings.plannerModel = selectedModel(m_plannerCombo);
    settings.proseModel = selectedModel(m_proseCombo);
    settings.save();
    QDialog::accept();
}
