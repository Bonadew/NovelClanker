#pragma once

#include "AiSettings.h"
#include "LmStudioClient.h"

#include <QDialog>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

// Settings view: LM Studio server address and the models used for planning and for prose.
// Fetches the server's models when opened and whenever the address is changed or refreshed.
class SettingsDialog : public QDialog
{
    Q_OBJECT

  public:
    explicit SettingsDialog(QWidget *parent = nullptr);

    void accept() override; // validates and saves

  private:
    void refreshModels();
    void onModelsFetched(const QList<LmModel> &models);
    void onFetchFailed(const QString &error);
    // Fills a model dropdown, keeping `selected` (a model id) selected even if the server doesn't list it.
    // `initial` is true before anything was fetched, so a missing model isn't flagged as unavailable yet.
    static void populate(QComboBox *combo, const QList<LmModel> &models, const QString &selected, bool initial);
    static QString selectedModel(const QComboBox *combo);

    LmStudioClient *m_client = nullptr;
    QLineEdit *m_serverEdit = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QLabel *m_statusLabel = nullptr;
    QComboBox *m_plannerCombo = nullptr;
    QComboBox *m_proseCombo = nullptr;
};
