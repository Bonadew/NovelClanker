#pragma once

#include "NovelStore.h"

#include <QMainWindow>

#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>

class QAction;
class QPlainTextEdit;
class QTimer;
class QTreeWidgetItem;
class MarkdownHighlighter;
class NovelTree;

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
    MarkdownHighlighter *m_highlighter = nullptr;
    QTimer *m_autosaveTimer = nullptr;

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
