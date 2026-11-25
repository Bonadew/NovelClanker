#pragma once

#include "LmStudioClient.h"
#include "NovelStore.h"

#include <QMainWindow>
#include <QPointer>

#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>

class QAction;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class QTreeWidget;
class QTimer;
class QTreeWidgetItem;
class ChatCompletion;
class MarkdownHighlighter;
class NovelTree;
class PlanChatPanel;

class MainWindow : public QMainWindow
{
    Q_OBJECT

  public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    // Opens the novel at `dir`; returns false (after telling the user) on failure.
    bool openNovel(const QString &dir, NovelStore::Mode mode);

  protected:
    void closeEvent(QCloseEvent *event) override;

  private:
    using Id = NovelStore::Id;

    // File menu
    void newNovel();
    void openNovelDialog();
    void exportMarkdown();

    // Structure editing
    void addAct();
    void addChapter();
    void addScene();
    void deleteCurrent();
    void renameCurrent();
    void moveCurrent(int delta);
    void onMoveRequested(QTreeWidgetItem *item, QTreeWidgetItem *newParent, int position);
    void onItemRenamed(QTreeWidgetItem *item);
    std::optional<Id> contextAct() const;
    std::optional<Id> contextChapter() const;

    // Scene editing
    void showItem(QTreeWidgetItem *item);
    bool saveCurrentScene();
    void onTextChanged();

    // Synopsis and plan views
    QWidget *buildSynopsisView();
    QWidget *buildPlanView();
    void loadSynopsisAndPlan();
    bool saveSynopsis();
    bool savePlan();
    void refreshPlanList(std::optional<Id> select);
    void showPlan(QTreeWidgetItem *item);
    void newPlan();
    void deletePlan();
    void onPlanRenamed(QTreeWidgetItem *item, int column);
    QString nextPlanTitle() const;
    void finishGeneratedPlan(); // common cleanup after a generation ends
    void lockPlanEditing(bool locked);
    void createStructureFromPlan();
    bool manuscriptIsUntouched() const; // empty, or only a new novel's untouched first scene

    // Chat about the selected plan
    QString chatModel() const; // the model that generated the plan, else this novel's planner model
    void updateChatState();
    void sendChatMessage(const QString &message);
    void onChatReply(const QString &message, const QString &reply, bool truncated);
    void clearChat();
    bool saveAll(); // scene, synopsis and plan
    void generatePlan();
    void stopPlanGeneration();
    void onPlanFinished(const QString &plan, bool truncated);
    void onPlanFailed(const QString &error);
    void showPlanText(const QString &text);
    void setPlanGenerating(bool generating);
    void updatePlannerStatus();
    void openSettings();
    void refreshPlanModels();      // fetch the server's models for the planner dropdown
    void populatePlanModelCombo(); // "Settings default" + server models, selecting the novel's choice
    void onPlanModelChosen();
    void showPlanTemplate();     // select the novel's plan template in the dropdown
    void onPlanTemplateChosen();
    QString effectivePlanModel() const; // the novel's choice, else the planner model from Settings
    QPlainTextEdit *focusedEditor() const;

    // Tree and status
    void rebuildTree(std::optional<Id> select);
    QTreeWidgetItem *findItem(Id id) const;
    qsizetype wordsIn(Id id) const;
    void updateActions();
    void updateStatus();
    void updateTitle();
    // Runs a store operation; shows `failure` with the error and returns false if it throws.
    template <typename F> bool attempt(const QString &failure, F &&operation);

    std::unique_ptr<NovelStore> m_store;
    std::optional<Id> m_currentScene;
    std::unordered_map<Id, qsizetype> m_sceneWords;
    std::unordered_set<Id> m_collapsed;
    bool m_loading = false;

    NovelTree *m_tree = nullptr;
    QPlainTextEdit *m_editor = nullptr;
    QLabel *m_briefLabel = nullptr; // brief of the selected act/chapter/scene
    MarkdownHighlighter *m_highlighter = nullptr;
    QTimer *m_autosaveTimer = nullptr;

    QTabWidget *m_tabs = nullptr;
    QPlainTextEdit *m_synopsisEdit = nullptr;
    QTimer *m_synopsisTimer = nullptr;
    QLabel *m_synopsisStatus = nullptr;
    QPushButton *m_generateButton = nullptr;
    QComboBox *m_planModelCombo = nullptr;
    QComboBox *m_planTemplateCombo = nullptr;
    QLabel *m_planTemplateHint = nullptr;
    QPushButton *m_refreshModelsButton = nullptr;
    LmStudioClient *m_modelClient = nullptr;
    QList<LmModel> m_serverModels;
    bool m_modelsFetched = false;
    QString m_modelsError;
    QWidget *m_planView = nullptr;
    QTreeWidget *m_planList = nullptr;
    QPushButton *m_newPlanButton = nullptr;
    QPushButton *m_deletePlanButton = nullptr;
    QPushButton *m_createStructureButton = nullptr;
    QLabel *m_planOrigin = nullptr;
    QPlainTextEdit *m_planEdit = nullptr;
    PlanChatPanel *m_chat = nullptr;
    std::optional<Id> m_chatPlan; // plan the running chat request is about
    QTimer *m_planTimer = nullptr;
    QLabel *m_planStatus = nullptr;
    QPushButton *m_stopButton = nullptr;
    QTimer *m_planRefresh = nullptr; // throttles redraws while a plan streams in
    QString m_pendingPlanText;
    QPointer<ChatCompletion> m_planRequest;
    std::optional<Id> m_currentPlan;        // plan shown in the editor
    std::optional<Id> m_generatingPlan;     // plan being written by the planner model
    std::optional<Id> m_planBeforeGeneration; // selected again if generation produces nothing
    QString m_planModel;
    bool m_synopsisDirty = false; // unsaved changes
    bool m_planDirty = false;

    QAction *m_saveAction = nullptr;
    QAction *m_exportAction = nullptr;
    QAction *m_addActAction = nullptr;
    QAction *m_addChapterAction = nullptr;
    QAction *m_addSceneAction = nullptr;
    QAction *m_renameAction = nullptr;
    QAction *m_deleteAction = nullptr;
    QAction *m_moveUpAction = nullptr;
    QAction *m_moveDownAction = nullptr;
};
