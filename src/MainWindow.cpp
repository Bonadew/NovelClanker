#include "MainWindow.h"

#include "AiSettings.h"
#include "LmChat.h"
#include "LmStudioClient.h"
#include "MarkdownHighlighter.h"
#include "NovelTree.h"
#include "PlanPrompt.h"
#include "SettingsDialog.h"

#include <QAction>
#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QTabWidget>
#include <QCloseEvent>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

namespace {

constexpr int kAutosaveDelayMs = 1500;
constexpr auto kLastNovelKey = "lastNovel";
constexpr int kIdRole = Qt::UserRole;

using Id = NovelStore::Id;
using Kind = NovelStore::Kind;

// The editor's text exactly as shown. QPlainTextEdit::toPlainText() would replace non-breaking spaces with
// ordinary spaces; the raw text keeps them and only needs paragraph separators turned back into newlines.
QString editorText(const QPlainTextEdit *edit)
{
    QString text = edit->document()->toRawText();
    text.replace(QChar::ParagraphSeparator, u'\n');
    text.replace(QChar::LineSeparator, u'\n');
    return text;
}

QPlainTextEdit *makeTextEditor(QWidget *parent)
{
    auto *edit = new QPlainTextEdit(parent);
    QFont font = edit->font();
    font.setPointSize(12);
    edit->setFont(font);
    edit->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    edit->document()->setDocumentMargin(24);
    return edit;
}

QTimer *makeAutosaveTimer(QObject *parent)
{
    auto *timer = new QTimer(parent);
    timer->setSingleShot(true);
    timer->setInterval(kAutosaveDelayMs);
    return timer;
}

qsizetype countWords(const QString &text)
{
    static const QRegularExpression whitespace(R"(\s+)");
    return text.split(whitespace, Qt::SkipEmptyParts).size();
}

std::filesystem::path toPath(const QString &path)
{
    return std::filesystem::path(path.toStdU16String());
}

QString fromPath(const std::filesystem::path &path)
{
    return QString::fromStdU16String(path.u16string());
}

Id itemId(const QTreeWidgetItem *item)
{
    return item->data(0, kIdRole).toULongLong();
}

std::optional<Id> optionalItemId(const QTreeWidgetItem *item)
{
    return item ? std::optional(itemId(item)) : std::nullopt;
}

int indexInParent(const QTreeWidgetItem *item)
{
    return item->parent() ? item->parent()->indexOfChild(const_cast<QTreeWidgetItem *>(item))
                          : item->treeWidget()->indexOfTopLevelItem(const_cast<QTreeWidgetItem *>(item));
}

int siblingCount(const QTreeWidgetItem *item)
{
    return item->parent() ? item->parent()->childCount() : item->treeWidget()->topLevelItemCount();
}

QTreeWidgetItem *ancestorAtLevel(QTreeWidgetItem *item, int level)
{
    while (item && NovelTree::level(item) > level) {
        item = item->parent();
    }
    return item;
}

// Stored title of any node (empty if unknown).
std::string storedTitle(const NovelStore &store, Id id)
{
    for (const auto &act : store.acts()) {
        if (act.id == id) {
            return act.title;
        }
        for (const auto &chapter : act.chapters) {
            if (chapter.id == id) {
                return chapter.title;
            }
            for (const auto &scene : chapter.scenes) {
                if (scene.id == id) {
                    return scene.title;
                }
            }
        }
    }
    return {};
}

std::size_t chapterCount(const NovelStore &store)
{
    std::size_t count = 0;
    for (const auto &act : store.acts()) {
        count += act.chapters.size();
    }
    return count;
}

} // namespace

template <typename F> bool MainWindow::attempt(const QString &failure, F &&operation)
{
    try {
        operation();
        return true;
    } catch (const std::exception &e) {
        QMessageBox::critical(this, tr("NovelWriter"), failure + "\n\n" + QString::fromUtf8(e.what()));
        return false;
    }
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    // --- Actions -----------------------------------------------------------------------------------
    m_addActAction = new QAction(tr("+ Act"), this);
    m_addActAction->setToolTip(tr("Add an act after the current one"));
    m_addChapterAction = new QAction(tr("+ Chapter"), this);
    m_addChapterAction->setToolTip(tr("Add a chapter to the current act (Ctrl+Shift+N)"));
    m_addChapterAction->setShortcut(QKeySequence(tr("Ctrl+Shift+N")));
    m_addSceneAction = new QAction(tr("+ Scene"), this);
    m_addSceneAction->setToolTip(tr("Add a scene to the current chapter (Ctrl+N)"));
    m_addSceneAction->setShortcut(QKeySequence::New);
    m_renameAction = new QAction(tr("&Rename"), this);
    m_renameAction->setShortcut(QKeySequence(Qt::Key_F2));
    m_renameAction->setShortcutContext(Qt::WidgetShortcut);
    m_deleteAction = new QAction(tr("&Delete..."), this);
    m_deleteAction->setShortcut(QKeySequence::Delete);
    m_deleteAction->setShortcutContext(Qt::WidgetShortcut); // only in the tree, not while typing
    m_moveUpAction = new QAction(tr("Move &Up"), this);
    m_moveUpAction->setShortcut(QKeySequence(tr("Alt+Up")));
    m_moveDownAction = new QAction(tr("Move D&own"), this);
    m_moveDownAction->setShortcut(QKeySequence(tr("Alt+Down")));

    connect(m_addActAction, &QAction::triggered, this, &MainWindow::addAct);
    connect(m_addChapterAction, &QAction::triggered, this, &MainWindow::addChapter);
    connect(m_addSceneAction, &QAction::triggered, this, &MainWindow::addScene);
    connect(m_renameAction, &QAction::triggered, this, &MainWindow::renameCurrent);
    connect(m_deleteAction, &QAction::triggered, this, &MainWindow::deleteCurrent);
    connect(m_moveUpAction, &QAction::triggered, this, [this] { moveCurrent(-1); });
    connect(m_moveDownAction, &QAction::triggered, this, [this] { moveCurrent(1); });

    // --- Left panel: add buttons above the act/chapter/scene tree ----------------------------------
    m_tree = new NovelTree(this);
    m_tree->setContextMenuPolicy(Qt::ActionsContextMenu);
    for (QAction *action : {m_renameAction, m_deleteAction, m_moveUpAction, m_moveDownAction}) {
        m_tree->addAction(action);
    }

    auto *buttons = new QToolBar(this);
    buttons->setToolButtonStyle(Qt::ToolButtonTextOnly);
    buttons->addAction(m_addActAction);
    buttons->addAction(m_addChapterAction);
    buttons->addAction(m_addSceneAction);

    auto *leftPanel = new QWidget(this);
    auto *leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(0);
    leftLayout->addWidget(buttons);
    leftLayout->addWidget(m_tree);

    // --- Editor --------------------------------------------------------------------------------------
    m_editor = makeTextEditor(this);
    m_editor->setObjectName("sceneEdit");
    m_highlighter = new MarkdownHighlighter(m_editor->document());

    auto *splitter = new QSplitter(this);
    splitter->addWidget(leftPanel);
    splitter->addWidget(m_editor);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({260, 840});

    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName("tabs");
    m_tabs->setDocumentMode(true);
    m_tabs->addTab(splitter, tr("Manuscript"));
    m_tabs->addTab(buildSynopsisView(), tr("Synopsis"));
    m_planView = buildPlanView();
    m_tabs->addTab(m_planView, tr("Plan"));
    m_tabs->setTabVisible(m_tabs->indexOf(m_planView), false); // shown once there is a plan
    setCentralWidget(m_tabs);

    m_autosaveTimer = makeAutosaveTimer(this);
    connect(m_autosaveTimer, &QTimer::timeout, this, &MainWindow::saveCurrentScene);

    // --- Menus ---------------------------------------------------------------------------------------
    QMenu *fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(tr("&New Novel..."), this, &MainWindow::newNovel);
    fileMenu->addAction(tr("&Open Novel..."), QKeySequence::Open, this, &MainWindow::openNovelDialog);
    m_saveAction = fileMenu->addAction(tr("&Save"), QKeySequence::Save, this, &MainWindow::saveCurrentScene);
    m_exportAction = fileMenu->addAction(tr("&Export as Markdown..."), this, &MainWindow::exportMarkdown);
    fileMenu->addSeparator();
    fileMenu->addAction(tr("E&xit"), QKeySequence::Quit, this, &QWidget::close);

    QMenu *editMenu = menuBar()->addMenu(tr("&Edit"));
    const auto onEditor = [this](void (QPlainTextEdit::*method)()) {
        return [this, method] {
            if (QPlainTextEdit *edit = focusedEditor()) {
                (edit->*method)();
            }
        };
    };
    editMenu->addAction(tr("&Undo"), QKeySequence::Undo, this, onEditor(&QPlainTextEdit::undo));
    editMenu->addAction(tr("&Redo"), QKeySequence::Redo, this, onEditor(&QPlainTextEdit::redo));
    editMenu->addSeparator();
    editMenu->addAction(tr("Cu&t"), QKeySequence::Cut, this, onEditor(&QPlainTextEdit::cut));
    editMenu->addAction(tr("&Copy"), QKeySequence::Copy, this, onEditor(&QPlainTextEdit::copy));
    editMenu->addAction(tr("&Paste"), QKeySequence::Paste, this, onEditor(&QPlainTextEdit::paste));
    editMenu->addSeparator();
    QAction *settingsAction = editMenu->addAction(tr("&Settings..."), QKeySequence::Preferences, this, [this] {
        SettingsDialog dialog(this);
        dialog.exec();
        updatePlannerStatus();
    });
    settingsAction->setMenuRole(QAction::PreferencesRole); // macOS: app menu "Settings..."

    QMenu *structureMenu = menuBar()->addMenu(tr("&Structure"));
    structureMenu->addAction(m_addActAction);
    structureMenu->addAction(m_addChapterAction);
    structureMenu->addAction(m_addSceneAction);
    structureMenu->addSeparator();
    structureMenu->addAction(m_renameAction);
    structureMenu->addAction(m_deleteAction);
    structureMenu->addSeparator();
    structureMenu->addAction(m_moveUpAction);
    structureMenu->addAction(m_moveDownAction);

    // --- Signals -------------------------------------------------------------------------------------
    connect(m_tree, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem *current) { showItem(current); });
    connect(m_tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item) { onItemRenamed(item); });
    // Queued: the tree is rebuilt in response, which must not happen inside its own drop handling.
    connect(m_tree, &NovelTree::moveRequested, this, &MainWindow::onMoveRequested, Qt::QueuedConnection);
    connect(m_tree, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem *item) {
        if (!m_loading) {
            m_collapsed.insert(itemId(item));
        }
    });
    connect(m_tree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *item) {
        if (!m_loading) {
            m_collapsed.erase(itemId(item));
        }
    });
    connect(m_editor, &QPlainTextEdit::textChanged, this, &MainWindow::onTextChanged);

    showItem(nullptr);
    loadSynopsisAndPlan();
    updateTitle();
}

MainWindow::~MainWindow() = default;

// ---------------------------------------------------------------------------------------------------
// Opening, saving, exporting
// ---------------------------------------------------------------------------------------------------

bool MainWindow::openNovel(const QString &dir, NovelStore::Mode mode)
{
    if (m_store && mode == NovelStore::Mode::Open &&
        QFileInfo(dir).canonicalFilePath() == QFileInfo(fromPath(m_store->path())).canonicalFilePath()) {
        return true; // already open (RocksDB would refuse a second lock anyway)
    }
    stopPlanGeneration();
    saveAll();

    std::unique_ptr<NovelStore> store;
    std::unordered_map<Id, qsizetype> words;
    std::optional<Id> firstScene;
    const bool ok = attempt(mode == NovelStore::Mode::Create ? tr("Could not create the novel.")
                                                             : tr("Could not open the novel."),
                            [&] {
                                store = std::make_unique<NovelStore>(toPath(dir), mode);
                                if (mode == NovelStore::Mode::Create) {
                                    const Id act = store->addAct(0, tr("Act 1").toStdString());
                                    const Id chapter = store->addChapter(act, 0, tr("Chapter 1").toStdString());
                                    store->addScene(chapter, 0, tr("Scene 1").toStdString());
                                }
                                for (const auto &act : store->acts()) {
                                    for (const auto &chapter : act.chapters) {
                                        for (const auto &scene : chapter.scenes) {
                                            words[scene.id] =
                                                countWords(QString::fromStdString(store->sceneText(scene.id)));
                                            if (!firstScene) {
                                                firstScene = scene.id;
                                            }
                                        }
                                    }
                                }
                            });
    if (!ok) {
        return false;
    }

    m_autosaveTimer->stop();
    m_currentScene.reset();
    m_collapsed.clear();
    m_sceneWords = std::move(words);
    m_store = std::move(store);
    QSettings().setValue(kLastNovelKey, dir);
    rebuildTree(firstScene);
    loadSynopsisAndPlan();
    updateTitle();
    return true;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_planRequest) {
        m_planRequest->abort(); // the unfinished plan is discarded; the previous plan stays saved
        m_planRequest->deleteLater();
    }
    if (saveAll() ||
        QMessageBox::question(this, tr("NovelWriter"), tr("Some changes could not be saved. Quit anyway?")) ==
            QMessageBox::Yes) {
        event->accept();
    } else {
        event->ignore();
    }
}

void MainWindow::newNovel()
{
    const QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString dir = QFileDialog::getSaveFileName(this, tr("New Novel"), docs + "/Untitled.novel",
                                                     tr("NovelWriter novel (*.novel)"), nullptr,
                                                     QFileDialog::DontConfirmOverwrite);
    if (dir.isEmpty()) {
        return;
    }
    if (QFileInfo::exists(dir)) {
        QMessageBox::warning(this, tr("New Novel"), tr("%1 already exists. Choose a new name.").arg(dir));
        return;
    }
    openNovel(dir, NovelStore::Mode::Create);
}

void MainWindow::openNovelDialog()
{
    const QString start = m_store ? QFileInfo(fromPath(m_store->path())).path()
                                  : QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Open Novel (select the .novel folder)"), start);
    if (!dir.isEmpty()) {
        openNovel(dir, NovelStore::Mode::Open);
    }
}

void MainWindow::exportMarkdown()
{
    if (!m_store || !saveCurrentScene()) {
        return;
    }
    const QString base = QFileInfo(fromPath(m_store->path())).completeBaseName();
    const QString path =
        QFileDialog::getSaveFileName(this, tr("Export as Markdown"), base + ".md", tr("Markdown (*.md)"));
    if (path.isEmpty()) {
        return;
    }
    attempt(tr("Could not export the novel."), [&] {
        // Acts and chapters become headings; scenes are separated by a scene break.
        std::string out;
        for (const auto &act : m_store->acts()) {
            out += "# " + act.title + "\n\n";
            for (const auto &chapter : act.chapters) {
                out += "## " + chapter.title + "\n\n";
                for (std::size_t s = 0; s < chapter.scenes.size(); ++s) {
                    std::string text = m_store->sceneText(chapter.scenes[s].id);
                    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
                        text.pop_back();
                    }
                    if (s > 0) {
                        out += "* * *\n\n";
                    }
                    out += text + "\n\n";
                }
            }
        }
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(out.data(), static_cast<qint64>(out.size())) < 0 ||
            !file.commit()) {
            throw std::runtime_error(file.errorString().toStdString());
        }
        statusBar()->showMessage(tr("Exported to %1").arg(QDir::toNativeSeparators(path)), 4000);
    });
}

// ---------------------------------------------------------------------------------------------------
// Structure editing
// ---------------------------------------------------------------------------------------------------

std::optional<Id> MainWindow::contextAct() const
{
    if (QTreeWidgetItem *act = ancestorAtLevel(m_tree->currentItem(), 0)) {
        return itemId(act);
    }
    if (m_store && !m_store->acts().empty()) {
        return m_store->acts().back().id;
    }
    return std::nullopt;
}

std::optional<Id> MainWindow::contextChapter() const
{
    QTreeWidgetItem *current = m_tree->currentItem();
    if (QTreeWidgetItem *chapter = current && NovelTree::level(current) >= 1 ? ancestorAtLevel(current, 1)
                                                                             : nullptr) {
        return itemId(chapter);
    }
    // An act (or nothing) is selected: use the last chapter of that act.
    const auto act = contextAct();
    for (const auto &a : m_store->acts()) {
        if (act && a.id == *act && !a.chapters.empty()) {
            return a.chapters.back().id;
        }
    }
    return std::nullopt;
}

void MainWindow::addAct()
{
    if (!m_store || !saveCurrentScene()) {
        return;
    }
    QTreeWidgetItem *currentAct = ancestorAtLevel(m_tree->currentItem(), 0);
    const std::size_t position =
        currentAct ? static_cast<std::size_t>(indexInParent(currentAct) + 1) : m_store->acts().size();
    const QString title = tr("Act %1").arg(m_store->acts().size() + 1);
    Id id = 0;
    if (attempt(tr("Could not add an act."), [&] { id = m_store->addAct(position, title.toStdString()); })) {
        rebuildTree(id);
        m_tree->editItem(findItem(id)); // let the user name it right away
    }
}

void MainWindow::addChapter()
{
    if (!m_store || !saveCurrentScene()) {
        return;
    }
    Id id = 0;
    const bool ok = attempt(tr("Could not add a chapter."), [&] {
        std::optional<Id> act = contextAct();
        if (!act) {
            act = m_store->addAct(0, tr("Act 1").toStdString());
        }
        // After the current chapter if one (or one of its scenes) is selected in this act, else at the end.
        std::size_t position = SIZE_MAX;
        QTreeWidgetItem *current = m_tree->currentItem();
        if (current && NovelTree::level(current) >= 1 && itemId(ancestorAtLevel(current, 0)) == *act) {
            position = static_cast<std::size_t>(indexInParent(ancestorAtLevel(current, 1)) + 1);
        }
        const QString title = tr("Chapter %1").arg(chapterCount(*m_store) + 1);
        id = m_store->addChapter(*act, position, title.toStdString());
    });
    if (ok) {
        rebuildTree(id);
        m_tree->editItem(findItem(id));
    }
}

void MainWindow::addScene()
{
    if (!m_store || !saveCurrentScene()) {
        return;
    }
    Id id = 0;
    const bool ok = attempt(tr("Could not add a scene."), [&] {
        std::optional<Id> chapter = contextChapter();
        if (!chapter) {
            std::optional<Id> act = contextAct();
            if (!act) {
                act = m_store->addAct(0, tr("Act 1").toStdString());
            }
            const QString title = tr("Chapter %1").arg(chapterCount(*m_store) + 1);
            chapter = m_store->addChapter(*act, SIZE_MAX, title.toStdString());
        }
        // After the current scene if one is selected in this chapter, else at the end of the chapter.
        std::size_t position = SIZE_MAX;
        QTreeWidgetItem *current = m_tree->currentItem();
        if (current && NovelTree::level(current) == 2 && itemId(current->parent()) == *chapter) {
            position = static_cast<std::size_t>(indexInParent(current) + 1);
        }
        QTreeWidgetItem *chapterItem = findItem(*chapter);
        const QString title = tr("Scene %1").arg(chapterItem ? chapterItem->childCount() + 1 : 1);
        id = m_store->addScene(*chapter, position, title.toStdString());
    });
    if (ok) {
        rebuildTree(id);
        m_editor->setFocus();
    }
}

void MainWindow::deleteCurrent()
{
    QTreeWidgetItem *item = m_tree->currentItem();
    if (!m_store || !item) {
        return;
    }
    const Id id = itemId(item);
    QString question;
    switch (NovelTree::level(item)) {
    case 0: {
        int scenes = 0;
        for (int c = 0; c < item->childCount(); ++c) {
            scenes += item->child(c)->childCount();
        }
        question = tr("Delete the act \"%1\" with its %2 chapter(s) and %3 scene(s)?")
                       .arg(item->text(0))
                       .arg(item->childCount())
                       .arg(scenes);
        break;
    }
    case 1:
        question = tr("Delete the chapter \"%1\" and its %2 scene(s)?").arg(item->text(0)).arg(item->childCount());
        break;
    default:
        question = tr("Delete the scene \"%1\"?").arg(item->text(0));
        break;
    }

    QMessageBox box(QMessageBox::Warning, tr("Delete"), question + "\n\n" + tr("This cannot be undone."),
                    QMessageBox::Cancel, this);
    QPushButton *deleteButton = box.addButton(tr("Delete"), QMessageBox::DestructiveRole);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != deleteButton) {
        return;
    }

    // Select a neighbour afterwards: the previous sibling, else the next one, else the parent.
    const int index = indexInParent(item);
    QTreeWidgetItem *parent = item->parent();
    const auto sibling = [&](int i) { return parent ? parent->child(i) : m_tree->topLevelItem(i); };
    QTreeWidgetItem *next = index > 0 ? sibling(index - 1) : sibling(index + 1);
    const std::optional<Id> select = next ? optionalItemId(next) : optionalItemId(parent);

    // Drop pending edits if the open scene is being deleted; otherwise save them.
    bool sceneDeleted = false;
    if (m_currentScene) {
        for (QTreeWidgetItem *p = findItem(*m_currentScene); p; p = p->parent()) {
            sceneDeleted = sceneDeleted || p == item;
        }
    }
    if (sceneDeleted) {
        m_autosaveTimer->stop();
        m_currentScene.reset();
    } else if (!saveCurrentScene()) {
        return;
    }

    attempt(tr("Could not delete."), [&] { m_store->remove(id); });
    rebuildTree(select);
}

void MainWindow::renameCurrent()
{
    if (QTreeWidgetItem *item = m_tree->currentItem()) {
        m_tree->editItem(item);
    }
}

void MainWindow::moveCurrent(int delta)
{
    QTreeWidgetItem *item = m_tree->currentItem();
    if (!m_store || !item) {
        return;
    }
    const int to = indexInParent(item) + delta;
    if (to < 0 || to >= siblingCount(item) || !saveCurrentScene()) {
        return;
    }
    const Id id = itemId(item);
    const Id parent = item->parent() ? itemId(item->parent()) : 0;
    if (attempt(tr("Could not move."), [&] { m_store->move(id, parent, static_cast<std::size_t>(to)); })) {
        rebuildTree(id);
    }
}

void MainWindow::onMoveRequested(QTreeWidgetItem *item, QTreeWidgetItem *newParent, int position)
{
    if (!m_store || !saveCurrentScene()) {
        return;
    }
    const Id id = itemId(item);
    const Id parent = newParent ? itemId(newParent) : 0;
    if (newParent) {
        m_collapsed.erase(parent); // show where the item went
    }
    attempt(tr("Could not move."), [&] { m_store->move(id, parent, static_cast<std::size_t>(position)); });
    rebuildTree(id);
}

void MainWindow::onItemRenamed(QTreeWidgetItem *item)
{
    if (m_loading || !m_store) {
        return;
    }
    const Id id = itemId(item);
    const QString title = item->text(0).trimmed();
    const QString stored = QString::fromStdString(storedTitle(*m_store, id));
    if (title == stored) {
        return;
    }
    if (title.isEmpty() ||
        !attempt(tr("Could not rename."), [&] { m_store->rename(id, title.toStdString()); })) {
        m_loading = true;
        item->setText(0, stored); // revert
        m_loading = false;
        return;
    }
    if (title != item->text(0)) {
        m_loading = true;
        item->setText(0, title); // show the trimmed title
        m_loading = false;
    }
}

// ---------------------------------------------------------------------------------------------------
// Scene editing
// ---------------------------------------------------------------------------------------------------

void MainWindow::showItem(QTreeWidgetItem *item)
{
    if (m_loading) {
        return;
    }
    saveCurrentScene();
    m_autosaveTimer->stop();

    const bool isScene = item && NovelTree::level(item) == 2;
    QString text;
    bool loaded = false;
    if (isScene) {
        loaded = attempt(tr("Could not load the scene."),
                         [&] { text = QString::fromStdString(m_store->sceneText(itemId(item))); });
    }

    m_loading = true;
    m_editor->setPlainText(text); // also resets undo history, which is per scene
    m_loading = false;
    m_editor->document()->setModified(false);
    m_currentScene = loaded ? std::optional(itemId(item)) : std::nullopt;
    m_editor->setEnabled(loaded);

    if (!m_store) {
        m_editor->setPlaceholderText(tr("Create or open a novel from the File menu to start writing."));
    } else if (!isScene) {
        m_editor->setPlaceholderText(
            tr("Select a scene to write it. Acts and chapters group scenes; use + Scene to add one."));
    }
    updateActions();
    updateStatus();
}

bool MainWindow::saveCurrentScene()
{
    m_autosaveTimer->stop();
    if (!m_store || !m_currentScene || !m_editor->document()->isModified()) {
        return true;
    }
    if (!attempt(tr("Could not save the scene."),
                 [&] { m_store->setSceneText(*m_currentScene, editorText(m_editor).toStdString()); })) {
        return false;
    }
    m_editor->document()->setModified(false);
    updateStatus();
    return true;
}

void MainWindow::onTextChanged()
{
    if (m_loading || !m_currentScene) {
        return;
    }
    m_sceneWords[*m_currentScene] = countWords(editorText(m_editor));
    m_autosaveTimer->start();
    updateStatus();
}

// ---------------------------------------------------------------------------------------------------
// Synopsis and plan views
// ---------------------------------------------------------------------------------------------------

QWidget *MainWindow::buildSynopsisView()
{
    auto *view = new QWidget(this);
    auto *hint = new QLabel(tr("Write or paste the synopsis of your novel. It is saved exactly as written."), view);
    hint->setWordWrap(true);

    m_synopsisEdit = makeTextEditor(view);
    m_synopsisEdit->setObjectName("synopsisEdit");
    m_synopsisTimer = makeAutosaveTimer(this);

    m_synopsisStatus = new QLabel(view);
    m_synopsisStatus->setObjectName("synopsisStatus");
    m_synopsisStatus->setWordWrap(true);
    m_generateButton = new QPushButton(tr("Generate Plan"), view);
    m_generateButton->setObjectName("generatePlanButton");
    m_generateButton->setToolTip(tr("Use the planner model to create a plan for the novel from this synopsis"));

    auto *bottom = new QHBoxLayout;
    bottom->addWidget(m_synopsisStatus, 1);
    bottom->addWidget(m_generateButton);

    auto *layout = new QVBoxLayout(view);
    layout->addWidget(hint);
    layout->addWidget(m_synopsisEdit, 1);
    layout->addLayout(bottom);

    connect(m_synopsisEdit, &QPlainTextEdit::textChanged, this, [this] {
        if (!m_loading) {
            // Own flag: the document's modified flag is reset by setPlainText() even though the text changed.
            m_synopsisDirty = true;
            m_synopsisTimer->start();
        }
    });
    connect(m_synopsisTimer, &QTimer::timeout, this, &MainWindow::saveSynopsis);
    connect(m_generateButton, &QPushButton::clicked, this, &MainWindow::generatePlan);
    return view;
}

QWidget *MainWindow::buildPlanView()
{
    auto *view = new QWidget(this);
    m_planStatus = new QLabel(view);
    m_planStatus->setObjectName("planStatus");
    m_planStatus->setWordWrap(true);
    m_stopButton = new QPushButton(tr("Stop"), view);
    m_stopButton->setObjectName("stopPlanButton");
    m_stopButton->setToolTip(tr("Stop generating and keep what has been written so far"));
    m_stopButton->hide();

    auto *top = new QHBoxLayout;
    top->addWidget(m_planStatus, 1);
    top->addWidget(m_stopButton);

    m_planEdit = makeTextEditor(view);
    m_planEdit->setObjectName("planEdit");
    new MarkdownHighlighter(m_planEdit->document());
    m_planTimer = makeAutosaveTimer(this);

    m_planRefresh = new QTimer(this);
    m_planRefresh->setSingleShot(true);
    m_planRefresh->setInterval(100);

    auto *layout = new QVBoxLayout(view);
    layout->addLayout(top);
    layout->addWidget(m_planEdit, 1);

    connect(m_planEdit, &QPlainTextEdit::textChanged, this, [this] {
        if (!m_loading && !m_planRequest) {
            m_planDirty = true;
            m_planTimer->start();
        }
    });
    connect(m_planTimer, &QTimer::timeout, this, &MainWindow::savePlan);
    connect(m_planRefresh, &QTimer::timeout, this, [this] { showPlanText(m_pendingPlanText); });
    connect(m_stopButton, &QPushButton::clicked, this, &MainWindow::stopPlanGeneration);
    return view;
}

void MainWindow::loadSynopsisAndPlan()
{
    QString synopsis, plan;
    if (m_store) {
        attempt(tr("Could not load the synopsis and plan."), [&] {
            synopsis = QString::fromStdString(m_store->synopsis());
            plan = QString::fromStdString(m_store->plan());
        });
    }
    m_synopsisTimer->stop();
    m_planTimer->stop();
    m_loading = true;
    m_synopsisEdit->setPlainText(synopsis);
    m_planEdit->setPlainText(plan);
    m_loading = false;
    m_synopsisDirty = false;
    m_planDirty = false;

    m_synopsisEdit->setEnabled(m_store != nullptr);
    m_planEdit->setEnabled(m_store != nullptr);
    m_synopsisEdit->setPlaceholderText(m_store ? tr("Once upon a time...")
                                               : tr("Create or open a novel from the File menu first."));
    m_planStatus->clear();
    m_tabs->setTabVisible(m_tabs->indexOf(m_planView), !plan.isEmpty());
    updatePlannerStatus();
}

bool MainWindow::saveSynopsis()
{
    m_synopsisTimer->stop();
    if (!m_store || !m_synopsisDirty) {
        return true;
    }
    if (!attempt(tr("Could not save the synopsis."),
                 [&] { m_store->setSynopsis(editorText(m_synopsisEdit).toStdString()); })) {
        return false;
    }
    m_synopsisDirty = false;
    return true;
}

bool MainWindow::savePlan()
{
    m_planTimer->stop();
    if (!m_store || m_planRequest || !m_planDirty) {
        return true; // a plan being generated is saved when it is complete
    }
    if (!attempt(tr("Could not save the plan."), [&] { m_store->setPlan(editorText(m_planEdit).toStdString()); })) {
        return false;
    }
    m_planDirty = false;
    return true;
}

bool MainWindow::saveAll()
{
    const bool scene = saveCurrentScene();
    const bool synopsis = saveSynopsis();
    const bool plan = savePlan();
    return scene && synopsis && plan;
}

void MainWindow::generatePlan()
{
    if (!m_store || m_planRequest || !saveSynopsis()) {
        return;
    }
    const QString synopsis = editorText(m_synopsisEdit);
    if (synopsis.trimmed().isEmpty()) {
        QMessageBox::information(this, tr("Generate Plan"), tr("Write or paste a synopsis first."));
        return;
    }
    const AiSettings settings = AiSettings::load();
    const QUrl server = normalizeServerUrl(settings.serverUrl);
    if (settings.plannerModel.isEmpty() || !server.isValid()) {
        QMessageBox box(QMessageBox::Information, tr("Generate Plan"),
                        tr("Choose an LM Studio server and a planner model in Settings first."), QMessageBox::Cancel,
                        this);
        QPushButton *open = box.addButton(tr("Open Settings..."), QMessageBox::AcceptRole);
        box.exec();
        if (box.clickedButton() == open) {
            SettingsDialog dialog(this);
            dialog.exec();
            updatePlannerStatus();
        }
        return;
    }
    if (!editorText(m_planEdit).trimmed().isEmpty() &&
        QMessageBox::question(this, tr("Generate Plan"),
                              tr("Replace the current plan with a newly generated one?")) != QMessageBox::Yes) {
        return;
    }
    if (!savePlan()) {
        return;
    }

    m_planBeforeGeneration = editorText(m_planEdit);
    m_planModel = settings.plannerModel;
    m_planRequest = new ChatCompletion(server, m_planModel, PlanPrompt::messages(synopsis), PlanPrompt::kTemperature,
                                       this);
    connect(m_planRequest, &ChatCompletion::textChanged, this, [this](const QString &text) {
        m_pendingPlanText = text;
        if (!m_planRefresh->isActive()) {
            m_planRefresh->start();
        }
    });
    connect(m_planRequest, &ChatCompletion::thinking, this, [this] {
        if (m_pendingPlanText.isEmpty()) {
            m_planStatus->setText(tr("%1 is thinking...").arg(m_planModel));
        }
    });
    connect(m_planRequest, &ChatCompletion::finished, this, &MainWindow::onPlanFinished);
    connect(m_planRequest, &ChatCompletion::failed, this, &MainWindow::onPlanFailed);

    m_pendingPlanText.clear();
    showPlanText({});
    setPlanGenerating(true);
    m_planStatus->setText(
        tr("Generating the plan with %1... (LM Studio may need a moment to load the model)").arg(m_planModel));
    m_tabs->setTabVisible(m_tabs->indexOf(m_planView), true);
    m_tabs->setCurrentWidget(m_planView);
    m_planRequest->start();
}

void MainWindow::showPlanText(const QString &text)
{
    m_loading = true;
    m_planEdit->setPlainText(text);
    m_loading = false;
    if (m_planRequest) {
        m_planEdit->moveCursor(QTextCursor::End); // follow the text as it is written
        m_planEdit->ensureCursorVisible();
        if (!text.isEmpty()) {
            m_planStatus->setText(tr("Writing the plan with %1... %n word(s) so far", nullptr,
                                     int(countWords(text)))
                                      .arg(m_planModel));
        }
    }
}

void MainWindow::setPlanGenerating(bool generating)
{
    m_planEdit->setReadOnly(generating);
    m_stopButton->setVisible(generating);
    m_generateButton->setEnabled(!generating && m_store != nullptr);
    updatePlannerStatus();
}

void MainWindow::stopPlanGeneration()
{
    if (!m_planRequest) {
        return;
    }
    m_planRequest->abort();
    m_planRequest->deleteLater();
    m_planRequest = nullptr;
    m_planRefresh->stop();
    setPlanGenerating(false);

    if (m_pendingPlanText.trimmed().isEmpty()) {
        showPlanText(m_planBeforeGeneration); // nothing written yet: keep the previous plan
        m_planStatus->setText(tr("Stopped. The previous plan was kept."));
        return;
    }
    // Keep the partial plan; the user asked for it to stop and may want to continue by hand.
    showPlanText(m_pendingPlanText);
    m_planDirty = true;
    savePlan();
    m_planStatus->setText(tr("Stopped. The plan is incomplete."));
}

void MainWindow::onPlanFinished(const QString &plan, bool truncated)
{
    m_planRequest->deleteLater();
    m_planRequest = nullptr;
    m_planRefresh->stop();
    setPlanGenerating(false);
    if (plan.trimmed().isEmpty()) {
        // Nothing usable: keep the previous plan rather than replacing it with nothing.
        showPlanText(m_planBeforeGeneration);
        m_planDirty = false;
    } else {
        showPlanText(plan);
        m_planDirty = true;
        savePlan();
    }
    m_planEdit->moveCursor(QTextCursor::Start);

    QString status = tr("Plan generated with %1. You can edit it here.").arg(m_planModel);
    if (plan.trimmed().isEmpty() && truncated) {
        // Typical for reasoning models: all tokens went into thinking.
        status = tr("%1 reached its length limit before writing anything (it probably spent it all on "
                    "reasoning). Increase the model's context length in LM Studio, or choose a non-reasoning "
                    "planner model, and generate again.")
                     .arg(m_planModel);
    } else if (plan.trimmed().isEmpty()) {
        status = tr("%1 returned an empty answer.").arg(m_planModel);
    } else if (truncated) {
        status += " " + tr("Warning: the answer was cut off because the model reached its length limit. Increase the "
                           "model's context length in LM Studio and generate again.");
    }
    m_planStatus->setText(status);
}

void MainWindow::onPlanFailed(const QString &error)
{
    m_planRequest->deleteLater();
    m_planRequest = nullptr;
    m_planRefresh->stop();
    setPlanGenerating(false);
    showPlanText(m_planBeforeGeneration);
    m_planDirty = false;
    m_tabs->setTabVisible(m_tabs->indexOf(m_planView), !m_planBeforeGeneration.isEmpty());
    m_planStatus->setText(tr("Plan generation failed."));
    QMessageBox::warning(this, tr("Generate Plan"),
                         tr("Could not generate the plan with %1:\n\n%2").arg(m_planModel, error));
}

void MainWindow::updatePlannerStatus()
{
    m_generateButton->setEnabled(m_store != nullptr && !m_planRequest);
    if (m_planRequest) {
        m_synopsisStatus->setText(tr("Generating the plan with %1...").arg(m_planModel));
        return;
    }
    const QString model = AiSettings::load().plannerModel;
    m_synopsisStatus->setText(model.isEmpty()
                                  ? tr("No planner model chosen yet: set one in Edit > Settings.")
                                  : tr("Planner model: %1").arg(model));
}

QPlainTextEdit *MainWindow::focusedEditor() const
{
    return qobject_cast<QPlainTextEdit *>(QApplication::focusWidget());
}

// ---------------------------------------------------------------------------------------------------
// Tree and status
// ---------------------------------------------------------------------------------------------------

void MainWindow::rebuildTree(std::optional<Id> select)
{
    m_loading = true;
    m_tree->clear();
    QTreeWidgetItem *selected = nullptr;
    const auto add = [&](QTreeWidgetItem *item, Id id, const std::string &title) {
        item->setText(0, QString::fromStdString(title));
        item->setData(0, kIdRole, QVariant::fromValue<qulonglong>(id));
        item->setFlags(item->flags() | Qt::ItemIsEditable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
        if (select && *select == id) {
            selected = item;
        }
        return item;
    };

    if (m_store) {
        QFont actFont = m_tree->font();
        actFont.setBold(true);
        for (const auto &act : m_store->acts()) {
            QTreeWidgetItem *actItem = add(new QTreeWidgetItem(m_tree), act.id, act.title);
            actItem->setFont(0, actFont);
            for (const auto &chapter : act.chapters) {
                QTreeWidgetItem *chapterItem = add(new QTreeWidgetItem(actItem), chapter.id, chapter.title);
                for (const auto &scene : chapter.scenes) {
                    add(new QTreeWidgetItem(chapterItem), scene.id, scene.title);
                }
                chapterItem->setExpanded(!m_collapsed.contains(chapter.id));
            }
            actItem->setExpanded(!m_collapsed.contains(act.id));
        }
    }
    m_loading = false;

    // Pending scene edits were saved (or deliberately dropped) before any structural change.
    m_currentScene.reset();
    if (selected) {
        for (QTreeWidgetItem *p = selected->parent(); p; p = p->parent()) {
            p->setExpanded(true);
        }
        m_tree->setCurrentItem(selected); // triggers showItem
        m_tree->scrollToItem(selected);
    } else {
        showItem(nullptr);
    }
}

QTreeWidgetItem *MainWindow::findItem(Id id) const
{
    for (QTreeWidgetItemIterator it(m_tree); *it; ++it) {
        if (itemId(*it) == id) {
            return *it;
        }
    }
    return nullptr;
}

qsizetype MainWindow::wordsIn(Id id) const
{
    const auto sceneWords = [this](Id scene) {
        const auto it = m_sceneWords.find(scene);
        return it == m_sceneWords.end() ? qsizetype(0) : it->second;
    };
    qsizetype total = 0;
    for (const auto &act : m_store->acts()) {
        const bool inAct = id == 0 || act.id == id;
        for (const auto &chapter : act.chapters) {
            const bool inChapter = inAct || chapter.id == id;
            for (const auto &scene : chapter.scenes) {
                if (inChapter || scene.id == id) {
                    total += sceneWords(scene.id);
                }
            }
        }
    }
    return total;
}

void MainWindow::updateActions()
{
    const bool haveNovel = m_store != nullptr;
    QTreeWidgetItem *item = m_tree->currentItem();
    m_saveAction->setEnabled(haveNovel);
    m_exportAction->setEnabled(haveNovel);
    m_addActAction->setEnabled(haveNovel);
    m_addChapterAction->setEnabled(haveNovel);
    m_addSceneAction->setEnabled(haveNovel);
    m_renameAction->setEnabled(item != nullptr);
    m_deleteAction->setEnabled(item != nullptr);
    m_moveUpAction->setEnabled(item && indexInParent(item) > 0);
    m_moveDownAction->setEnabled(item && indexInParent(item) + 1 < siblingCount(item));
}

void MainWindow::updateStatus()
{
    if (!m_store) {
        statusBar()->clearMessage();
        return;
    }
    const QString novel = tr("Novel: %1 words").arg(wordsIn(0));
    QTreeWidgetItem *item = m_tree->currentItem();
    if (!item) {
        statusBar()->showMessage(novel);
        return;
    }
    QStringList parts;
    switch (NovelTree::level(item)) {
    case 2:
        parts << tr("Scene: %1 words").arg(wordsIn(itemId(item)))
              << tr("Chapter: %1").arg(wordsIn(itemId(item->parent())));
        break;
    case 1:
        parts << tr("Chapter: %1 words").arg(wordsIn(itemId(item)));
        break;
    default:
        parts << tr("Act: %1 words").arg(wordsIn(itemId(item)));
        break;
    }
    parts << novel;
    if (m_currentScene) {
        parts << (m_editor->document()->isModified() ? tr("Editing") : tr("Saved"));
    }
    statusBar()->showMessage(parts.join(QStringLiteral("  ·  ")));
}

void MainWindow::updateTitle()
{
    setWindowTitle(m_store ? QFileInfo(fromPath(m_store->path())).completeBaseName() + " - NovelWriter"
                           : QStringLiteral("NovelWriter"));
}
