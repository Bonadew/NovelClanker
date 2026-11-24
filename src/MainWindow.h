#pragma once

#include "NovelStore.h"

#include <QMainWindow>

#include <memory>
#include <optional>
#include <unordered_map>

class QAction;
class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QTimer;
class MarkdownHighlighter;

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
    void newNovel();
    void openNovelDialog();
    void exportMarkdown();
    void addChapter();
    void deleteChapter();
    void moveChapter(int delta);

    void showChapter(int row);
    bool saveCurrentChapter();
    void onTextChanged();
    void onRowsMoved(int from, int to);
    void reloadChapterList(int selectRow);
    void updateActions();
    void updateStatus();
    void updateTitle();
    QListWidgetItem *currentItem() const;
    void showError(const QString &what, const std::exception &e);

    std::unique_ptr<NovelStore> m_store;
    std::optional<NovelStore::ChapterId> m_currentChapter;
    std::unordered_map<NovelStore::ChapterId, qsizetype> m_wordCounts;
    bool m_loading = false;

    QListWidget *m_chapterList = nullptr;
    QPlainTextEdit *m_editor = nullptr;
    MarkdownHighlighter *m_highlighter = nullptr;
    QTimer *m_autosaveTimer = nullptr;

    QAction *m_exportAction = nullptr;
    QAction *m_saveAction = nullptr;
    QAction *m_addChapterAction = nullptr;
    QAction *m_deleteChapterAction = nullptr;
    QAction *m_moveUpAction = nullptr;
    QAction *m_moveDownAction = nullptr;
};
