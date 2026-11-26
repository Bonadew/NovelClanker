#pragma once

#include "LmStudioClient.h"
#include "NovelStore.h"
#include "ProsePrompt.h"
#include "SpellChecker.h"

#include <QMainWindow>
#include <QPointer>

#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>

class QAction;
class QActionGroup;
class QMenu;
class QPoint;
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

    // Creates a new novel at `novelDir` from the Markdown file `markdownFile` (an export, or written by hand),
    // rebuilding acts, chapters and scenes with their text, and opens it. Returns false (after telling the user)
    // on failure; nothing is left behind at `novelDir` then.
    bool importMarkdown(const QString &markdownFile, const QString &novelDir);

    // Writes the open novel as an EPUB eBook to `path`, with the book details from Novel Setup. Returns false
    // (after telling the user) on failure.
    bool exportEpub(const QString &path);

  protected:
    void closeEvent(QCloseEvent *event) override;

  private:
    using Id = NovelStore::Id;

    // File menu
    void newNovel();
    void openNovelDialog();
    void exportMarkdown();
    void exportEpubDialog();
    void openNovelSetup(); // File > Novel Setup: title, author and other book details
    void importMarkdownDialog();

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

    // Prose writer: write the scene from its brief, or expand a passage, with the prose model
    QWidget *buildProseBar();
    void writeScene();
    void expandPassage();
    // Streams the prose model's answer into the editor, replacing the text in [start, start + length).
    // `after` is the text that follows the range (to remove a repeat of it from the answer).
    void startProse(const QList<ChatMessage> &messages, int start, int length, const QString &after = {});
    void showProseText(const QString &text);
    void stopProse();
    void onProseFinished(const QString &text, bool truncated);
    void onProseFailed(const QString &error);
    void finishProse();
    void updateProseActions();
    int proseLength() const; // the chosen length in words
    QString proseModel() const; // the Model dropdown's choice, else the prose writer model from Settings
    bool checkProseModel(const QString &title); // false (after telling the user) if no prose model is set
    ProsePrompt::SceneContext sceneContext(Id scene) const;

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
    void refreshPlanModels();  // fetch the server's models for the planner and prose writer dropdowns
    void populateModelCombos(); // "Settings default" + server models for the planner and prose dropdowns
    void onPlanModelChosen();
    void showPlanTemplate();     // select the novel's plan template in the dropdown
    void onPlanTemplateChosen();
    QString effectivePlanModel() const; // the novel's choice, else the planner model from Settings
    QPlainTextEdit *focusedEditor() const;

    // Text size of the writing editors (View menu, Ctrl+Plus / Ctrl+Minus / Ctrl+0); saved per user.
    void setTextSize(int points, bool remember = true);
    void changeTextSize(int steps); // +1 larger, -1 smaller

    // Spelling (Edit > Spelling): the dictionary is saved with the novel; misspellings are underlined in the
    // scene, synopsis and plan editors, whose context menus offer suggestions, Add to Dictionary and Ignore All.
    QString spellingDictionaryCode() const; // this novel's dictionary, else the last chosen, else by region
    void chooseDictionary(const QString &code);
    void applySpelling();
    void showEditorMenu(QPlainTextEdit *edit, MarkdownHighlighter *highlighter, const QPoint &pos);

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
    MarkdownHighlighter *m_planHighlighter = nullptr;
    MarkdownHighlighter *m_synopsisHighlighter = nullptr;
    std::unique_ptr<SpellChecker> m_spell = std::make_unique<SpellChecker>();
    QMenu *m_spellingMenu = nullptr;
    QAction *m_checkSpellingAction = nullptr;
    QActionGroup *m_dictionaryGroup = nullptr;
    int m_textSize = 0; // points, for the scene, synopsis and plan editors
    QTimer *m_autosaveTimer = nullptr;

    QPushButton *m_writeSceneButton = nullptr;
    QPushButton *m_expandButton = nullptr;
    QComboBox *m_proseLengthCombo = nullptr;
    QPushButton *m_proseStopButton = nullptr;
    QLabel *m_proseStatus = nullptr;
    QTimer *m_proseRefresh = nullptr; // throttles editor updates while prose streams in
    QPointer<ChatCompletion> m_proseRequest;
    QString m_proseModelInUse;
    QString m_pendingProse;   // latest cleaned answer
    QString m_proseOriginal;  // the text being replaced, restored if the request fails
    int m_proseStart = 0;     // editor range holding the generated text
    int m_proseLength = 0;
    QString m_proseAfter;     // the text after the range
    int m_proseWordLimit = 0; // the request is ended once the text is longer than this
    bool m_proseEdited = false; // the generated text has been put into the editor at least once

    QTabWidget *m_tabs = nullptr;
    QPlainTextEdit *m_synopsisEdit = nullptr;
    QTimer *m_synopsisTimer = nullptr;
    QLabel *m_synopsisStatus = nullptr;
    QPushButton *m_generateButton = nullptr;
    QComboBox *m_planModelCombo = nullptr;
    QComboBox *m_proseModelCombo = nullptr;
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
    QAction *m_exportEpubAction = nullptr;
    QAction *m_setupAction = nullptr;
    QAction *m_addActAction = nullptr;
    QAction *m_addChapterAction = nullptr;
    QAction *m_addSceneAction = nullptr;
    QAction *m_renameAction = nullptr;
    QAction *m_deleteAction = nullptr;
    QAction *m_moveUpAction = nullptr;
    QAction *m_moveDownAction = nullptr;
};
