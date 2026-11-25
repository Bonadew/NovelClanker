#include "MainWindow.h"

#include "AiSettings.h"
#include "LmChat.h"
#include "LmStudioClient.h"
#include "MarkdownHighlighter.h"
#include "ModelCombo.h"
#include "NovelTree.h"
#include "PlanChat.h"
#include "PlanChatPanel.h"
#include "PlanPrompt.h"
#include "PlanStructure.h"
#include "ProsePrompt.h"
#include "SettingsDialog.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSet>
#include <QTabWidget>
#include <QTextCursor>
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

#include <algorithm>
#include <utility>
#include <vector>
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
    m_tree->setObjectName("manuscriptTree");
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
    m_briefLabel = new QLabel(this);
    m_briefLabel->setObjectName("briefLabel");
    m_briefLabel->setWordWrap(true);
    m_briefLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_briefLabel->setFrameShape(QFrame::StyledPanel);
    m_briefLabel->setMargin(6);
    m_briefLabel->hide();
    auto *editorPane = new QWidget(this);
    auto *editorLayout = new QVBoxLayout(editorPane);
    editorLayout->setContentsMargins(0, 0, 0, 0);
    editorLayout->addWidget(buildProseBar());
    editorLayout->addWidget(m_briefLabel);
    editorLayout->addWidget(m_editor, 1);
    splitter->addWidget(editorPane);
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
            if (QPlainTextEdit *edit = focusedEditor(); edit && !edit->isReadOnly()) {
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
    QAction *settingsAction =
        editMenu->addAction(tr("&Settings..."), QKeySequence::Preferences, this, &MainWindow::openSettings);
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
    connect(m_editor, &QPlainTextEdit::selectionChanged, this, &MainWindow::updateProseActions);

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
    m_chat->stop();
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
    stopPlanGeneration(); // like Stop: keeps what was written so far
    stopProse();
    m_chat->stop();       // an unfinished chat answer changes nothing
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
    stopProse(); // the text written so far stays in the scene it was written for
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

    // The selected node's brief (e.g. from the plan it was created from).
    QString brief;
    if (m_store && item) {
        const Id id = itemId(item);
        for (const auto &act : m_store->acts()) {
            if (act.id == id) {
                brief = QString::fromStdString(act.brief);
            }
            for (const auto &chapter : act.chapters) {
                if (chapter.id == id) {
                    brief = QString::fromStdString(chapter.brief);
                }
                for (const auto &scene : chapter.scenes) {
                    if (scene.id == id) {
                        brief = QString::fromStdString(scene.brief);
                    }
                }
            }
        }
    }
    m_briefLabel->setText(brief.isEmpty() ? QString() : tr("Brief: %1").arg(brief));
    m_briefLabel->setVisible(!brief.isEmpty());
    m_proseStatus->clear();
    updateActions();
    updateProseActions();
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
    if (!m_proseRequest) {
        m_autosaveTimer->start(); // generated text is saved once it is complete
    }
    updateStatus();
    updateProseActions();
}

// ---------------------------------------------------------------------------------------------------
// Prose writer
// ---------------------------------------------------------------------------------------------------

namespace {
const char *const kWriteSceneTip = QT_TRANSLATE_NOOP("MainWindow", "Write the whole scene from its brief with the "
                                                                   "prose writer model.");
const char *const kExpandTip = QT_TRANSLATE_NOOP("MainWindow", "Rewrite the selected text (or the whole scene if "
                                                               "nothing is selected) as a longer passage with the "
                                                               "prose writer model.");
constexpr auto kProseLengthKey = "prose/length";
} // namespace

QWidget *MainWindow::buildProseBar()
{
    auto *bar = new QWidget(this);
    auto *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(6, 4, 6, 0);

    m_writeSceneButton = new QPushButton(tr("Write Scene"), bar);
    m_writeSceneButton->setObjectName("writeSceneButton");
    m_expandButton = new QPushButton(tr("Expand"), bar);
    m_expandButton->setObjectName("expandButton");
    m_proseLengthCombo = new QComboBox(bar);
    m_proseLengthCombo->setObjectName("proseLengthCombo");
    m_proseLengthCombo->setToolTip(tr("How long the written or expanded text should be."));
    for (const int words : ProsePrompt::kLengths) {
        m_proseLengthCombo->addItem(tr("%1 words").arg(words), words);
    }
    const int saved = QSettings().value(kProseLengthKey, ProsePrompt::kDefaultLength).toInt();
    m_proseLengthCombo->setCurrentIndex(std::max(0, m_proseLengthCombo->findData(saved)));
    m_proseStopButton = new QPushButton(tr("Stop"), bar);
    m_proseStopButton->setObjectName("proseStopButton");
    m_proseStopButton->hide();
    m_proseStatus = new QLabel(bar);
    m_proseStatus->setObjectName("proseStatus");
    m_proseStatus->setWordWrap(true);

    layout->addWidget(m_writeSceneButton);
    layout->addWidget(m_expandButton);
    layout->addWidget(new QLabel(tr("Length:"), bar));
    layout->addWidget(m_proseLengthCombo);
    layout->addWidget(m_proseStopButton);
    layout->addWidget(m_proseStatus, 1);

    m_proseRefresh = new QTimer(this);
    m_proseRefresh->setSingleShot(true);
    m_proseRefresh->setInterval(100);
    connect(m_proseRefresh, &QTimer::timeout, this, [this] { showProseText(m_pendingProse); });
    connect(m_writeSceneButton, &QPushButton::clicked, this, &MainWindow::writeScene);
    connect(m_expandButton, &QPushButton::clicked, this, &MainWindow::expandPassage);
    connect(m_proseStopButton, &QPushButton::clicked, this, [this] {
        if (m_proseRequest) {
            stopProse();
            m_proseStatus->setText(tr("Stopped. The text written so far is kept; Undo removes it."));
        }
    });
    connect(m_proseLengthCombo, &QComboBox::currentIndexChanged, this,
            [this] { QSettings().setValue(kProseLengthKey, proseLength()); });
    return bar;
}

int MainWindow::proseLength() const
{
    return m_proseLengthCombo->currentData().toInt();
}

QString MainWindow::proseModel() const
{
    return AiSettings::load().proseModel;
}

void MainWindow::updateProseActions()
{
    if (!m_writeSceneButton) {
        return; // still building the window
    }
    const bool idle = !m_proseRequest && m_currentScene.has_value();
    m_writeSceneButton->setEnabled(idle);
    m_expandButton->setEnabled(idle && !editorText(m_editor).trimmed().isEmpty());
    m_expandButton->setText(m_editor->textCursor().hasSelection() ? tr("Expand Selection") : tr("Expand"));
    m_proseLengthCombo->setEnabled(!m_proseRequest);
    m_proseStopButton->setVisible(m_proseRequest != nullptr);
    const QString model = proseModel();
    const QString uses =
        model.isEmpty() ? tr("No prose writer model is chosen in Settings.") : tr("Uses %1.").arg(model);
    m_writeSceneButton->setToolTip(tr(kWriteSceneTip) + " " + uses);
    m_expandButton->setToolTip(tr(kExpandTip) + " " + uses);
}

bool MainWindow::checkProseModel(const QString &title)
{
    if (!proseModel().isEmpty() && normalizeServerUrl(AiSettings::load().serverUrl).isValid()) {
        return true;
    }
    QMessageBox box(QMessageBox::Information, title,
                    tr("Choose a prose writer model and an LM Studio server in Settings first."), QMessageBox::Cancel,
                    this);
    QPushButton *open = box.addButton(tr("Open Settings..."), QMessageBox::AcceptRole);
    box.exec();
    if (box.clickedButton() == open) {
        openSettings();
    }
    return false;
}

ProsePrompt::SceneContext MainWindow::sceneContext(Id scene) const
{
    constexpr int kPreviousWords = 600;
    ProsePrompt::SceneContext context;
    context.synopsis = editorText(m_synopsisEdit);

    struct Place {
        const NovelStore::Act *act;
        const NovelStore::Chapter *chapter;
        const NovelStore::Scene *scene;
    };
    const auto acts = m_store->acts();
    std::vector<Place> order; // every scene in reading order
    for (const auto &act : acts) {
        for (const auto &chapter : act.chapters) {
            for (const auto &s : chapter.scenes) {
                order.push_back({&act, &chapter, &s});
            }
        }
    }
    const auto it = std::find_if(order.begin(), order.end(), [&](const Place &p) { return p.scene->id == scene; });
    if (it == order.end()) {
        return context;
    }
    context.actTitle = QString::fromStdString(it->act->title);
    context.actBrief = QString::fromStdString(it->act->brief);
    context.chapterTitle = QString::fromStdString(it->chapter->title);
    context.chapterBrief = QString::fromStdString(it->chapter->brief);
    context.sceneTitle = QString::fromStdString(it->scene->title);
    context.sceneBrief = QString::fromStdString(it->scene->brief);

    // The end of the nearest earlier scene that has text.
    for (auto prev = it; prev != order.begin();) {
        --prev;
        const QString text = QString::fromStdString(m_store->sceneText(prev->scene->id));
        if (!text.trimmed().isEmpty()) {
            context.previousText = ProsePrompt::lastWords(text, kPreviousWords);
            break;
        }
    }
    if (const auto next = std::next(it); next != order.end()) {
        const bool useChapter = next->scene->brief.empty() && next->chapter != it->chapter;
        context.nextBrief = QString::fromStdString(useChapter ? next->chapter->brief : next->scene->brief);
    }
    return context;
}

void MainWindow::writeScene()
{
    const QString title = tr("Write Scene");
    if (!m_store || !m_currentScene || m_proseRequest || !saveCurrentScene() || !checkProseModel(title)) {
        return;
    }
    const ProsePrompt::SceneContext context = sceneContext(*m_currentScene);
    if (context.sceneBrief.trimmed().isEmpty() && context.chapterBrief.trimmed().isEmpty()) {
        QMessageBox::information(this, title,
                                 tr("This scene has no brief to write it from. Create the structure from a plan to "
                                    "get briefs, or write a short draft of the scene and use Expand."));
        return;
    }
    const QString text = editorText(m_editor);
    if (!text.trimmed().isEmpty() &&
        QMessageBox::question(this, title,
                              tr("Replace the scene's text with a new scene written from the brief? Undo brings "
                                 "the current text back.")) != QMessageBox::Yes) {
        return;
    }
    startProse(ProsePrompt::sceneMessages(context, proseLength()), 0, static_cast<int>(text.size()));
}

void MainWindow::expandPassage()
{
    const QString title = tr("Expand");
    if (!m_store || !m_currentScene || m_proseRequest || !checkProseModel(title)) {
        return;
    }
    const QString text = editorText(m_editor);
    const QTextCursor cursor = m_editor->textCursor();
    int start = cursor.hasSelection() ? cursor.selectionStart() : 0;
    int end = cursor.hasSelection() ? cursor.selectionEnd() : static_cast<int>(text.size());
    // Leave the whitespace around the passage where it is.
    while (start < end && text[start].isSpace()) {
        ++start;
    }
    while (end > start && text[end - 1].isSpace()) {
        --end;
    }
    const QString passage = text.mid(start, end - start);
    if (passage.isEmpty()) {
        return;
    }
    const qsizetype words = countWords(passage);
    if (words >= proseLength()) {
        QMessageBox::information(
            this, title, tr("The passage already has %1 words. Choose a longer length to expand it to.").arg(words));
        return;
    }
    // The model sees only the opening of the text after the passage: enough to know where to stop, too little to
    // invite rewriting it. Repeats are looked for in a longer stretch.
    constexpr int kBeforeWords = 400, kAfterPromptWords = 40, kAfterCheckWords = 300;
    const QString after = ProsePrompt::firstWords(text.mid(end), kAfterCheckWords);
    const auto messages = ProsePrompt::expandMessages(
        sceneContext(*m_currentScene), ProsePrompt::lastWords(text.left(start), kBeforeWords), passage,
        ProsePrompt::trimToWords(after, kAfterPromptWords), proseLength());
    startProse(messages, start, end - start, after);
}

void MainWindow::startProse(const QList<ChatMessage> &messages, int start, int length, const QString &after)
{
    m_proseModelInUse = proseModel();
    m_proseStart = start;
    m_proseLength = length;
    m_proseOriginal = editorText(m_editor).mid(start, length);
    m_proseAfter = after;
    m_proseWordLimit = ProsePrompt::wordLimit(proseLength());
    m_proseEdited = false;
    m_pendingProse.clear();

    m_proseRequest = new ChatCompletion(normalizeServerUrl(AiSettings::load().serverUrl), m_proseModelInUse,
                                        messages, ProsePrompt::kTemperature, this);
    connect(m_proseRequest, &ChatCompletion::textChanged, this, [this](const QString &text) {
        m_pendingProse = ProsePrompt::cleanProse(text);
        if (const QString cut = ProsePrompt::removeEcho(m_pendingProse, m_proseAfter); cut != m_pendingProse) {
            // The model has started rewriting the text that follows the passage: what comes now is a duplicate.
            m_proseRequest->abort();
            m_pendingProse = cut;
            onProseFinished(m_pendingProse, false);
            m_proseStatus->setText(m_proseStatus->text() + " " +
                                   tr("%1 began rewriting the text after the passage, so that part was left out.")
                                       .arg(m_proseModelInUse));
            return;
        }
        if (countWords(m_pendingProse) > m_proseWordLimit) {
            // Past the length the author asked for (some models never stop): end at the last whole sentence.
            m_proseRequest->abort();
            m_pendingProse = ProsePrompt::trimToWords(m_pendingProse, m_proseWordLimit);
            onProseFinished(m_pendingProse, false);
            m_proseStatus->setText(m_proseStatus->text() + " " +
                                   tr("%1 kept writing past the chosen length, so it was stopped at the end of a "
                                      "sentence.")
                                       .arg(m_proseModelInUse));
            return;
        }
        if (!m_proseRefresh->isActive()) {
            m_proseRefresh->start();
        }
    });
    connect(m_proseRequest, &ChatCompletion::thinking, this, [this] {
        if (m_pendingProse.isEmpty()) {
            m_proseStatus->setText(tr("%1 is thinking...").arg(m_proseModelInUse));
        }
    });
    connect(m_proseRequest, &ChatCompletion::finished, this, &MainWindow::onProseFinished);
    connect(m_proseRequest, &ChatCompletion::failed, this, &MainWindow::onProseFailed);

    m_editor->setReadOnly(true); // the model's text must not clash with edits
    m_tree->setEnabled(false);
    updateProseActions();
    m_proseStatus->setText(
        tr("Writing with %1... (LM Studio may need a moment to load the model)").arg(m_proseModelInUse));
    m_proseRequest->start();
}

// Puts `text` into the editor in place of the previously generated text. All updates of one request form a single
// undo step, so Undo brings back exactly the text from before.
void MainWindow::showProseText(const QString &text)
{
    if (!m_proseEdited && text.isEmpty()) {
        return; // keep the original text until the model writes something
    }
    if (m_proseEdited && text == editorText(m_editor).mid(m_proseStart, m_proseLength)) {
        return;
    }
    QTextCursor cursor(m_editor->document());
    if (m_proseEdited) {
        cursor.joinPreviousEditBlock();
    } else {
        cursor.beginEditBlock();
    }
    cursor.setPosition(m_proseStart);
    cursor.setPosition(m_proseStart + m_proseLength, QTextCursor::KeepAnchor);
    cursor.insertText(text);
    cursor.endEditBlock();
    m_proseEdited = true;
    m_proseLength = static_cast<int>(text.size());
    m_editor->setTextCursor(cursor); // follow the text as it is written
    m_editor->ensureCursorVisible();
    if (m_proseRequest && !text.isEmpty()) {
        m_proseStatus->setText(
            tr("Writing with %1... %n word(s) so far", nullptr, int(countWords(text))).arg(m_proseModelInUse));
    }
}

void MainWindow::finishProse()
{
    if (m_proseRequest) {
        m_proseRequest->disconnect(this); // nothing more from it, even if it was ended from one of its signals
        m_proseRequest->deleteLater();
        m_proseRequest = nullptr;
    }
    m_proseRefresh->stop();
    m_editor->setReadOnly(false);
    m_tree->setEnabled(true);
    updateProseActions();
    saveCurrentScene();
}

void MainWindow::stopProse()
{
    if (!m_proseRequest) {
        return;
    }
    m_proseRequest->abort();
    showProseText(m_pendingProse); // keep what was written so far
    finishProse();
}

void MainWindow::onProseFinished(const QString &text, bool truncated)
{
    m_pendingProse = ProsePrompt::trimToWords(ProsePrompt::removeEcho(ProsePrompt::cleanProse(text), m_proseAfter),
                                              m_proseWordLimit);
    showProseText(m_pendingProse);
    finishProse();
    const QString model = m_proseModelInUse;
    if (m_pendingProse.trimmed().isEmpty()) {
        m_proseStatus->setText(truncated ? tr("%1 reached its length limit before writing anything. Increase the "
                                              "model's context length in LM Studio, or choose a non-reasoning model.")
                                               .arg(model)
                                         : tr("%1 returned an empty answer.").arg(model));
        return;
    }
    QString status =
        tr("Done: %n word(s). Undo brings back the previous text.", nullptr, int(countWords(m_pendingProse)));
    if (truncated) {
        status += " " + tr("Warning: the text was cut off because the model reached its length limit.");
    }
    m_proseStatus->setText(status);
}

void MainWindow::onProseFailed(const QString &error)
{
    // A failed request changes nothing: put the original text back.
    if (m_proseEdited) {
        showProseText(m_proseOriginal);
    }
    finishProse();
    m_proseStatus->setText(tr("Writing failed."));
    QMessageBox::warning(this, tr("Prose Writer"), tr("Could not write with %1:\n\n%2").arg(m_proseModelInUse, error));
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

    m_planModelCombo = new QComboBox(view);
    m_planModelCombo->setObjectName("planModelCombo");
    ModelCombo::setUp(m_planModelCombo);
    m_planModelCombo->setToolTip(tr("Model used by Generate Plan for this novel. \"Settings default\" uses the "
                                    "planner model from Edit > Settings."));
    m_refreshModelsButton = new QPushButton(tr("Refresh"), view);
    m_refreshModelsButton->setObjectName("refreshPlanModelsButton");
    m_refreshModelsButton->setToolTip(tr("Fetch the models from the LM Studio server again"));
    m_modelClient = new LmStudioClient(this);

    m_planTemplateCombo = new QComboBox(view);
    m_planTemplateCombo->setObjectName("planTemplateCombo");
    for (const PlanPrompt::Template &t : PlanPrompt::templates()) {
        m_planTemplateCombo->addItem(t.name, t.id);
        m_planTemplateCombo->setItemData(m_planTemplateCombo->count() - 1, t.description, Qt::ToolTipRole);
    }
    m_planTemplateHint = new QLabel(view);
    m_planTemplateHint->setObjectName("planTemplateHint");
    m_planTemplateHint->setWordWrap(true);
    m_planTemplateHint->setEnabled(false); // greyed-out helper text

    auto *templateRow = new QHBoxLayout;
    templateRow->addWidget(new QLabel(tr("Plan template:"), view));
    templateRow->addWidget(m_planTemplateCombo, 1);

    auto *modelRow = new QHBoxLayout;
    modelRow->addWidget(new QLabel(tr("Planner model:"), view));
    modelRow->addWidget(m_planModelCombo, 1);
    modelRow->addWidget(m_refreshModelsButton);
    modelRow->addSpacing(12);
    modelRow->addWidget(m_generateButton);

    auto *bottom = new QVBoxLayout;
    bottom->addLayout(templateRow);
    bottom->addWidget(m_planTemplateHint);
    bottom->addLayout(modelRow);
    bottom->addWidget(m_synopsisStatus);

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
    connect(m_refreshModelsButton, &QPushButton::clicked, this, &MainWindow::refreshPlanModels);
    connect(m_planModelCombo, &QComboBox::currentIndexChanged, this, &MainWindow::onPlanModelChosen);
    connect(m_planTemplateCombo, &QComboBox::currentIndexChanged, this, &MainWindow::onPlanTemplateChosen);
    connect(m_modelClient, &LmStudioClient::modelsFetched, this, [this](const QList<LmModel> &models) {
        m_serverModels = models;
        m_modelsFetched = true;
        m_modelsError.clear();
        populatePlanModelCombo();
    });
    connect(m_modelClient, &LmStudioClient::fetchFailed, this, [this](const QString &error) {
        m_modelsError = error;
        updatePlannerStatus();
    });
    return view;
}

QWidget *MainWindow::buildPlanView()
{
    auto *view = new QWidget(this);

    // --- Left: the novel's plans ------------------------------------------------------------------------
    m_planList = new QTreeWidget(view);
    m_planList->setObjectName("planList");
    m_planList->setColumnCount(2);
    m_planList->setHeaderLabels({tr("Plan"), tr("Generated with")});
    m_planList->setRootIsDecorated(false);
    m_planList->setEditTriggers(QAbstractItemView::NoEditTriggers); // titles are renamed by double-click
    m_planList->setToolTip(tr("Double-click a plan's name to rename it"));
    m_newPlanButton = new QPushButton(tr("+ New Plan"), view);
    m_newPlanButton->setObjectName("newPlanButton");
    m_newPlanButton->setToolTip(tr("Add an empty plan to write by hand"));
    m_deletePlanButton = new QPushButton(tr("Delete"), view);
    m_deletePlanButton->setObjectName("deletePlanButton");

    auto *listButtons = new QHBoxLayout;
    listButtons->addWidget(m_newPlanButton);
    listButtons->addWidget(m_deletePlanButton);
    listButtons->addStretch();
    auto *left = new QWidget(view);
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->addLayout(listButtons);
    leftLayout->addWidget(m_planList, 1);

    // --- Right: the selected plan ---------------------------------------------------------------------
    m_planOrigin = new QLabel(view);
    m_planOrigin->setObjectName("planOrigin");
    m_planStatus = new QLabel(view);
    m_planStatus->setObjectName("planStatus");
    m_planStatus->setWordWrap(true);
    m_stopButton = new QPushButton(tr("Stop"), view);
    m_stopButton->setObjectName("stopPlanButton");
    m_stopButton->setToolTip(tr("Stop generating and keep what has been written so far"));
    m_stopButton->hide();

    auto *top = new QHBoxLayout;
    top->addWidget(m_planOrigin);
    top->addWidget(m_planStatus, 1);
    top->addWidget(m_stopButton);
    m_createStructureButton = new QPushButton(tr("Create Structure"), view);
    m_createStructureButton->setObjectName("createStructureButton");
    m_createStructureButton->setToolTip(tr("Create the manuscript's acts, chapters and scenes from this plan"));
    top->addWidget(m_createStructureButton);

    m_planEdit = makeTextEditor(view);
    m_planEdit->setObjectName("planEdit");
    new MarkdownHighlighter(m_planEdit->document());
    m_planTimer = makeAutosaveTimer(this);

    auto *planPane = new QWidget(view);
    auto *planLayout = new QVBoxLayout(planPane);
    planLayout->setContentsMargins(0, 0, 0, 0);
    planLayout->addLayout(top);
    planLayout->addWidget(m_planEdit, 1);

    m_chat = new PlanChatPanel(view);
    auto *right = new QSplitter(Qt::Vertical, view);
    right->addWidget(planPane);
    right->addWidget(m_chat);
    right->setStretchFactor(0, 3);
    right->setStretchFactor(1, 2);

    auto *splitter = new QSplitter(view);
    splitter->addWidget(left);
    splitter->addWidget(right);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({300, 800});
    auto *layout = new QVBoxLayout(view);
    layout->addWidget(splitter);

    m_planRefresh = new QTimer(this);
    m_planRefresh->setSingleShot(true);
    m_planRefresh->setInterval(100);

    connect(m_planEdit, &QPlainTextEdit::textChanged, this, [this] {
        if (!m_loading && m_currentPlan && m_currentPlan != m_generatingPlan) {
            m_planDirty = true;
            m_planTimer->start();
        }
    });
    connect(m_planTimer, &QTimer::timeout, this, &MainWindow::savePlan);
    connect(m_planRefresh, &QTimer::timeout, this, [this] { showPlanText(m_pendingPlanText); });
    connect(m_stopButton, &QPushButton::clicked, this, &MainWindow::stopPlanGeneration);
    connect(m_planList, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) { showPlan(item); });
    connect(m_planList, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int column) {
        if (column == 0 && !m_planRequest) {
            m_planList->editItem(item, 0);
        }
    });
    connect(m_planList, &QTreeWidget::itemChanged, this, &MainWindow::onPlanRenamed);
    connect(m_newPlanButton, &QPushButton::clicked, this, &MainWindow::newPlan);
    connect(m_deletePlanButton, &QPushButton::clicked, this, &MainWindow::deletePlan);
    connect(m_createStructureButton, &QPushButton::clicked, this, &MainWindow::createStructureFromPlan);
    connect(m_chat, &PlanChatPanel::submitted, this, &MainWindow::sendChatMessage);
    connect(m_chat, &PlanChatPanel::replied, this, &MainWindow::onChatReply);
    connect(m_chat, &PlanChatPanel::busyChanged, this, [this](bool busy) { lockPlanEditing(busy); });
    connect(m_chat, &PlanChatPanel::clearRequested, this, &MainWindow::clearChat);
    return view;
}

void MainWindow::loadSynopsisAndPlan()
{
    QString synopsis;
    if (m_store) {
        attempt(tr("Could not load the synopsis."), [&] { synopsis = QString::fromStdString(m_store->synopsis()); });
    }
    m_synopsisTimer->stop();
    m_loading = true;
    m_synopsisEdit->setPlainText(synopsis);
    m_loading = false;
    m_synopsisDirty = false;

    m_synopsisEdit->setEnabled(m_store != nullptr);
    m_synopsisEdit->setPlaceholderText(m_store ? tr("Once upon a time...")
                                               : tr("Create or open a novel from the File menu first."));
    m_planStatus->clear();
    m_currentPlan.reset();
    std::optional<Id> first;
    if (m_store && !m_store->plans().empty()) {
        first = m_store->plans().front().id;
    }
    refreshPlanList(first);
    m_tabs->setTabVisible(m_tabs->indexOf(m_planView), m_store != nullptr);
    showPlanTemplate();
    populatePlanModelCombo();
    if (m_store) {
        refreshPlanModels();
    }
}

void MainWindow::showPlanTemplate()
{
    QString chosen;
    if (m_store) {
        attempt(tr("Could not read the plan template."),
                [&] { chosen = QString::fromStdString(m_store->planTemplate()); });
    }
    const PlanPrompt::Template &t = PlanPrompt::templateById(chosen); // unknown or empty: the default
    m_loading = true;
    m_planTemplateCombo->setCurrentIndex(m_planTemplateCombo->findData(t.id));
    m_planTemplateCombo->setEnabled(m_store != nullptr);
    m_loading = false;
    m_planTemplateHint->setText(t.description);
}

void MainWindow::onPlanTemplateChosen()
{
    const PlanPrompt::Template &t = PlanPrompt::templateById(m_planTemplateCombo->currentData().toString());
    m_planTemplateHint->setText(t.description);
    if (m_loading || !m_store) {
        return;
    }
    attempt(tr("Could not save the plan template."), [&] { m_store->setPlanTemplate(t.id.toStdString()); });
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
    if (!m_store || !m_currentPlan || !m_planDirty || m_currentPlan == m_generatingPlan) {
        return true; // a plan being generated is saved when generation ends
    }
    const Id plan = *m_currentPlan;
    if (!attempt(tr("Could not save the plan."),
                 [&] { m_store->setPlanText(plan, editorText(m_planEdit).toStdString()); })) {
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

void MainWindow::refreshPlanList(std::optional<Id> select)
{
    m_loading = true;
    m_planList->clear();
    QTreeWidgetItem *selected = nullptr;
    if (m_store) {
        for (const auto &plan : m_store->plans()) {
            auto *item = new QTreeWidgetItem(m_planList);
            item->setText(0, QString::fromStdString(plan.title));
            item->setText(1, plan.model.empty() ? tr("(by hand)") : QString::fromStdString(plan.model));
            item->setToolTip(1, item->text(1));
            item->setData(0, kIdRole, QVariant::fromValue<qulonglong>(plan.id));
            item->setFlags(item->flags() | Qt::ItemIsEditable);
            if (select && *select == plan.id) {
                selected = item;
            }
        }
    }
    m_planList->resizeColumnToContents(0);
    m_loading = false;

    m_currentPlan.reset(); // the previous plan was saved before the list changed
    if (selected) {
        m_planList->setCurrentItem(selected); // triggers showPlan
    } else {
        showPlan(nullptr);
    }
}

void MainWindow::showPlan(QTreeWidgetItem *item)
{
    if (m_loading) {
        return;
    }
    savePlan();
    m_planTimer->stop();

    QString text;
    std::optional<Id> plan = optionalItemId(item);
    if (plan && !attempt(tr("Could not load the plan."),
                         [&] { text = QString::fromStdString(m_store->planText(*plan)); })) {
        plan.reset();
    }
    m_loading = true;
    m_planEdit->setPlainText(text);
    m_loading = false;
    m_planDirty = false;
    m_currentPlan = plan;

    m_planEdit->setEnabled(plan.has_value());
    m_planEdit->setPlaceholderText(m_store ? tr("No plan selected. Generate one from the Synopsis tab, or use "
                                                "+ New Plan to write one by hand.")
                                           : QString());
    QString origin;
    if (plan) {
        const auto &plans = m_store->plans();
        const auto it = std::find_if(plans.begin(), plans.end(), [&](const auto &p) { return p.id == *plan; });
        if (it != plans.end()) {
            if (it->model.empty()) {
                origin = tr("Written by hand.");
            } else if (it->templateId.empty()) {
                origin = tr("Generated with %1.").arg(QString::fromStdString(it->model));
            } else {
                origin = tr("Generated with %1 using the \"%2\" template.")
                             .arg(QString::fromStdString(it->model),
                                  PlanPrompt::templateById(QString::fromStdString(it->templateId)).name);
            }
        }
    }
    m_planOrigin->setText(origin);
    m_deletePlanButton->setEnabled(plan.has_value() && !m_planRequest);
    m_createStructureButton->setEnabled(plan.has_value() && !m_planRequest);

    QList<PlanChat::Turn> history;
    if (plan) {
        attempt(tr("Could not load the plan's conversation."),
                [&] { history = PlanChat::parse(QString::fromStdString(m_store->planChat(*plan))); });
    }
    m_chat->setHistory(history);
    updateChatState();
}

QString MainWindow::nextPlanTitle() const
{
    QSet<QString> used;
    for (const auto &plan : m_store->plans()) {
        used.insert(QString::fromStdString(plan.title));
    }
    for (int n = static_cast<int>(m_store->plans().size()) + 1;; ++n) {
        const QString title = tr("Plan %1").arg(n);
        if (!used.contains(title)) {
            return title;
        }
    }
}

void MainWindow::newPlan()
{
    if (!m_store || m_planRequest || !savePlan()) {
        return;
    }
    Id id = 0;
    if (!attempt(tr("Could not add a plan."), [&] { id = m_store->addPlan(nextPlanTitle().toStdString(), ""); })) {
        return;
    }
    refreshPlanList(id);
    m_planEdit->setFocus();
}

void MainWindow::deletePlan()
{
    QTreeWidgetItem *item = m_planList->currentItem();
    if (!m_store || !item || m_planRequest) {
        return;
    }
    QMessageBox box(QMessageBox::Warning, tr("Delete Plan"),
                    tr("Delete the plan \"%1\"?\n\nThis cannot be undone.").arg(item->text(0)), QMessageBox::Cancel,
                    this);
    QPushButton *deleteButton = box.addButton(tr("Delete"), QMessageBox::DestructiveRole);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != deleteButton) {
        return;
    }
    const int index = m_planList->indexOfTopLevelItem(item);
    QTreeWidgetItem *neighbour = m_planList->topLevelItem(index > 0 ? index - 1 : index + 1);
    const std::optional<Id> select = optionalItemId(neighbour);
    const Id id = itemId(item);
    m_planTimer->stop();
    m_currentPlan.reset(); // drop unsaved edits of the plan being deleted
    attempt(tr("Could not delete the plan."), [&] { m_store->removePlan(id); });
    refreshPlanList(select);
}

void MainWindow::onPlanRenamed(QTreeWidgetItem *item, int column)
{
    if (m_loading || !m_store || column != 0) {
        return;
    }
    const Id id = itemId(item);
    const QString title = item->text(0).trimmed();
    QString stored;
    for (const auto &plan : m_store->plans()) {
        if (plan.id == id) {
            stored = QString::fromStdString(plan.title);
        }
    }
    if (title == stored) {
        return;
    }
    if (title.isEmpty() || !attempt(tr("Could not rename the plan."),
                                    [&] { m_store->renamePlan(id, title.toStdString()); })) {
        m_loading = true;
        item->setText(0, stored);
        m_loading = false;
    } else if (title != item->text(0)) {
        m_loading = true;
        item->setText(0, title);
        m_loading = false;
    }
}

void MainWindow::generatePlan()
{
    if (!m_store || m_planRequest || !saveSynopsis() || !savePlan()) {
        return;
    }
    const QString synopsis = editorText(m_synopsisEdit);
    if (synopsis.trimmed().isEmpty()) {
        QMessageBox::information(this, tr("Generate Plan"), tr("Write or paste a synopsis first."));
        return;
    }
    const AiSettings settings = AiSettings::load();
    const QUrl server = normalizeServerUrl(settings.serverUrl);
    const QString model = effectivePlanModel();
    if (model.isEmpty() || !server.isValid()) {
        QMessageBox box(QMessageBox::Information, tr("Generate Plan"),
                        tr("Choose a planner model (here or in Settings) and an LM Studio server in Settings first."),
                        QMessageBox::Cancel, this);
        QPushButton *open = box.addButton(tr("Open Settings..."), QMessageBox::AcceptRole);
        box.exec();
        if (box.clickedButton() == open) {
            openSettings();
        }
        return;
    }

    // Every generation makes a new plan that remembers the model and template; existing plans are never replaced.
    m_planModel = model;
    const QString templateId = PlanPrompt::templateById(m_planTemplateCombo->currentData().toString()).id;
    Id id = 0;
    if (!attempt(tr("Could not add a plan."), [&] {
            id = m_store->addPlan(nextPlanTitle().toStdString(), "", m_planModel.toStdString(),
                                  templateId.toStdString());
        })) {
        return;
    }
    m_planBeforeGeneration = m_currentPlan;
    m_generatingPlan = id;
    m_planRequest = new ChatCompletion(server, m_planModel, PlanPrompt::messages(synopsis, templateId),
                                       PlanPrompt::kTemperature, this);
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
    refreshPlanList(id);
    setPlanGenerating(true);
    m_planStatus->setText(
        tr("Generating with %1... (LM Studio may need a moment to load the model)").arg(m_planModel));
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
            m_planStatus->setText(tr("Writing with %1... %n word(s) so far", nullptr, int(countWords(text)))
                                      .arg(m_planModel));
        }
    }
}

void MainWindow::setPlanGenerating(bool generating)
{
    m_stopButton->setVisible(generating);
    lockPlanEditing(generating);
}

void MainWindow::lockPlanEditing(bool locked)
{
    // While a model is writing (a new plan, or an answer that may revise the plan), keep the plan on screen
    // and unchanged so the model's text can't clash with edits.
    m_planEdit->setReadOnly(locked);
    m_planList->setEnabled(!locked);
    m_newPlanButton->setEnabled(!locked);
    m_deletePlanButton->setEnabled(!locked && m_currentPlan.has_value());
    m_createStructureButton->setEnabled(!locked && m_currentPlan.has_value());
    m_generateButton->setEnabled(!locked && m_store != nullptr);
    updatePlannerStatus();
}

// Ends a generation: keeps the generated plan if it has text (saving it), otherwise removes it again and
// re-selects the plan shown before. The caller sets the status message.
void MainWindow::finishGeneratedPlan()
{
    if (m_planRequest) {
        m_planRequest->deleteLater();
        m_planRequest = nullptr;
    }
    m_planRefresh->stop();
    const std::optional<Id> plan = std::exchange(m_generatingPlan, std::nullopt);
    setPlanGenerating(false);
    if (!plan) {
        return;
    }
    if (m_pendingPlanText.trimmed().isEmpty()) {
        attempt(tr("Could not remove the empty plan."), [&] { m_store->removePlan(*plan); });
        refreshPlanList(m_planBeforeGeneration);
        return;
    }
    showPlanText(m_pendingPlanText);
    m_planDirty = true;
    savePlan();
    m_planEdit->moveCursor(QTextCursor::Start);
}

void MainWindow::stopPlanGeneration()
{
    if (!m_planRequest) {
        return;
    }
    m_planRequest->abort();
    const bool kept = !m_pendingPlanText.trimmed().isEmpty();
    finishGeneratedPlan(); // keeps the partial plan; the user may want to continue it by hand
    m_planStatus->setText(kept ? tr("Stopped. The plan is incomplete.") : tr("Stopped before anything was written."));
}

void MainWindow::onPlanFinished(const QString &plan, bool truncated)
{
    m_pendingPlanText = plan;
    finishGeneratedPlan();

    QString status = tr("Done. You can edit the plan here.");
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
    m_pendingPlanText.clear(); // discard any partial text: the generation went wrong
    finishGeneratedPlan();
    m_planStatus->setText(tr("Plan generation failed."));
    QMessageBox::warning(this, tr("Generate Plan"),
                         tr("Could not generate the plan with %1:\n\n%2").arg(m_planModel, error));
}

void MainWindow::openSettings()
{
    SettingsDialog dialog(this);
    dialog.exec();
    populatePlanModelCombo(); // the default model may have changed
    if (m_store) {
        refreshPlanModels(); // and so may the server
    }
}

void MainWindow::refreshPlanModels()
{
    const QUrl server = normalizeServerUrl(AiSettings::load().serverUrl);
    if (!server.isValid()) {
        m_modelsError = tr("the server address in Settings is not valid");
        updatePlannerStatus();
        return;
    }
    m_modelClient->fetchModels(server);
}

void MainWindow::populatePlanModelCombo()
{
    QString chosen;
    if (m_store) {
        attempt(tr("Could not read the plan model."),
                [&] { chosen = QString::fromStdString(m_store->planModelOverride()); });
    }
    const QString defaultModel = AiSettings::load().plannerModel;
    m_loading = true;
    ModelCombo::populate(m_planModelCombo, m_serverModels, chosen, m_modelsFetched);
    // First entry: no override, use the planner model from Settings.
    m_planModelCombo->insertItem(0,
                                 defaultModel.isEmpty() ? tr("Settings default (none chosen)")
                                                        : tr("Settings default (%1)").arg(defaultModel),
                                 QString());
    if (chosen.isEmpty()) {
        m_planModelCombo->setCurrentIndex(0);
    }
    m_planModelCombo->setEnabled(m_store != nullptr);
    m_refreshModelsButton->setEnabled(m_store != nullptr);
    m_loading = false;
    updatePlannerStatus();
}

void MainWindow::onPlanModelChosen()
{
    if (m_loading || !m_store) {
        return;
    }
    const QString model = ModelCombo::selectedId(m_planModelCombo);
    attempt(tr("Could not save the plan model."), [&] { m_store->setPlanModelOverride(model.toStdString()); });
    updatePlannerStatus();
}

QString MainWindow::effectivePlanModel() const
{
    const QString chosen = ModelCombo::selectedId(m_planModelCombo);
    return chosen.isEmpty() ? AiSettings::load().plannerModel : chosen;
}

void MainWindow::updatePlannerStatus()
{
    m_generateButton->setEnabled(m_store != nullptr && !m_planRequest);
    if (m_planRequest) {
        m_synopsisStatus->setText(tr("Generating the plan with %1...").arg(m_planModel));
        return;
    }
    QString status;
    const QString model = effectivePlanModel();
    if (model.isEmpty()) {
        status = tr("No planner model chosen yet: choose one here or in Edit > Settings.");
    } else if (ModelCombo::selectedId(m_planModelCombo).isEmpty()) {
        status = tr("Plans are generated with %1 (from Settings).").arg(model);
    } else {
        status = tr("Plans for this novel are generated with %1.").arg(model);
    }
    if (m_store && !m_modelsError.isEmpty()) {
        status += "\n" + tr("Could not list the server's models: %1").arg(m_modelsError);
    }
    m_synopsisStatus->setText(status);
    updateChatState();
}

bool MainWindow::manuscriptIsUntouched() const
{
    // Nothing, or just the "Act 1 / Chapter 1 / Scene 1" a new novel starts with, never written in.
    const auto &acts = m_store->acts();
    if (acts.empty()) {
        return true;
    }
    if (acts.size() != 1 || acts[0].chapters.size() != 1 || acts[0].chapters[0].scenes.size() != 1) {
        return false;
    }
    const auto &act = acts[0];
    const auto &chapter = act.chapters[0];
    const auto &scene = chapter.scenes[0];
    return act.title == tr("Act 1").toStdString() && chapter.title == tr("Chapter 1").toStdString() &&
           scene.title == tr("Scene 1").toStdString() && act.brief.empty() && chapter.brief.empty() &&
           scene.brief.empty() && wordsIn(scene.id) == 0;
}

void MainWindow::createStructureFromPlan()
{
    if (!m_store || !m_currentPlan || m_planRequest || m_chat->isBusy() || !savePlan() || !saveCurrentScene()) {
        return;
    }
    const QList<PlanStructure::Act> parsed = PlanStructure::parse(editorText(m_planEdit));
    const PlanStructure::Counts counts = PlanStructure::count(parsed);
    if (counts.acts == 0) {
        QMessageBox::information(
            this, tr("Create Structure"),
            tr("No acts, chapters or scenes were found in this plan. The structure is read from headings such as "
               "\"### Act 1: ...\" and \"#### Chapter 1: ...\" and scene lines such as \"- **Scene 1: Title**: "
               "...\"."));
        return;
    }
    const QString summary = tr("This plan has %1 act(s), %2 chapter(s) and %3 scene(s).")
                                .arg(counts.acts)
                                .arg(counts.chapters)
                                .arg(counts.scenes);

    bool replace = true;
    if (!manuscriptIsUntouched()) {
        int scenes = 0;
        for (const auto &act : m_store->acts()) {
            for (const auto &chapter : act.chapters) {
                scenes += static_cast<int>(chapter.scenes.size());
            }
        }
        const qsizetype words = wordsIn(0);
        QMessageBox box(QMessageBox::Question, tr("Create Structure"),
                        summary + "\n\n" +
                            tr("The manuscript already has %n scene(s)", nullptr, scenes) +
                            tr(" with %n word(s). Add the plan's structure after it, or replace the manuscript?",
                               nullptr, int(words)),
                        QMessageBox::Cancel, this);
        QPushButton *append = box.addButton(tr("Add After Existing"), QMessageBox::AcceptRole);
        QPushButton *replaceButton = box.addButton(tr("Replace Manuscript..."), QMessageBox::DestructiveRole);
        box.setDefaultButton(append);
        box.exec();
        if (box.clickedButton() == append) {
            replace = false;
        } else if (box.clickedButton() == replaceButton) {
            if (words > 0 &&
                QMessageBox::warning(this, tr("Replace Manuscript"),
                                     tr("Delete all %n scene(s) of the manuscript", nullptr, scenes) +
                                         tr(", with %n word(s)? This cannot be undone.", nullptr, int(words)),
                                     QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) {
                return;
            }
        } else {
            return;
        }
    }

    std::vector<NovelStore::NewAct> acts;
    for (const PlanStructure::Act &act : parsed) {
        NovelStore::NewAct newAct{act.title.toStdString(), act.brief.toStdString(), {}};
        for (const PlanStructure::Chapter &chapter : act.chapters) {
            NovelStore::NewChapter newChapter{chapter.title.toStdString(), chapter.brief.toStdString(), {}};
            for (const PlanStructure::Scene &scene : chapter.scenes) {
                newChapter.scenes.push_back({scene.title.toStdString(), scene.brief.toStdString()});
            }
            newAct.chapters.push_back(std::move(newChapter));
        }
        acts.push_back(std::move(newAct));
    }

    m_autosaveTimer->stop();
    m_currentScene.reset(); // saved above; the open scene may be replaced
    std::optional<Id> first;
    if (!attempt(tr("Could not create the structure."), [&] { first = m_store->importStructure(acts, replace); })) {
        rebuildTree(std::nullopt);
        return;
    }
    if (replace) {
        m_sceneWords.clear();
        m_collapsed.clear();
    }
    rebuildTree(first);
    m_tabs->setCurrentIndex(0); // Manuscript
    statusBar()->showMessage(tr("Created %1 act(s), %2 chapter(s) and %3 scene(s) from the plan.")
                                 .arg(counts.acts)
                                 .arg(counts.chapters)
                                 .arg(counts.scenes),
                             6000);
}

QString MainWindow::chatModel() const
{
    if (m_store && m_currentPlan) {
        for (const auto &plan : m_store->plans()) {
            if (plan.id == *m_currentPlan && !plan.model.empty()) {
                return QString::fromStdString(plan.model);
            }
        }
    }
    return effectivePlanModel(); // hand-written plans: this novel's planner model
}

void MainWindow::updateChatState()
{
    if (!m_chat || m_chat->isBusy()) {
        return;
    }
    const QString model = chatModel();
    bool generated = false;
    if (m_store && m_currentPlan) {
        for (const auto &plan : m_store->plans()) {
            generated = generated || (plan.id == *m_currentPlan && !plan.model.empty());
        }
    }
    if (!m_currentPlan) {
        m_chat->setHeader(tr("Chat"));
        m_chat->setAvailable(false, tr("Select a plan to talk about it."));
    } else if (m_planRequest) {
        m_chat->setHeader(tr("Chat"));
        m_chat->setAvailable(false, tr("Wait until the plan has been generated."));
    } else if (model.isEmpty()) {
        m_chat->setHeader(tr("Chat"));
        m_chat->setAvailable(false, tr("Choose a planner model on the Synopsis tab or in Settings first."));
    } else {
        m_chat->setHeader(generated ? tr("Chat with %1, which generated this plan.").arg(model)
                                    : tr("Chat with %1 (this novel's planner model).").arg(model));
        m_chat->setAvailable(true);
    }
}

void MainWindow::sendChatMessage(const QString &message)
{
    if (!m_store || !m_currentPlan || m_planRequest || !saveSynopsis() || !savePlan()) {
        return;
    }
    const QUrl server = normalizeServerUrl(AiSettings::load().serverUrl);
    const QString model = chatModel();
    if (!server.isValid() || model.isEmpty()) {
        m_chat->showNote(tr("Set a valid LM Studio server address in Settings first."));
        return;
    }
    QList<PlanChat::Turn> history;
    if (!attempt(tr("Could not load the plan's conversation."),
                 [&] { history = PlanChat::parse(QString::fromStdString(m_store->planChat(*m_currentPlan))); })) {
        return;
    }
    m_chatPlan = m_currentPlan;
    m_chat->start(server, model,
                  PlanChat::messages(editorText(m_synopsisEdit), editorText(m_planEdit), history, message), message);
}

void MainWindow::onChatReply(const QString &message, const QString &reply, bool truncated)
{
    const std::optional<Id> plan = std::exchange(m_chatPlan, std::nullopt);
    if (!m_store || !plan || plan != m_currentPlan) {
        return; // the novel changed meanwhile
    }
    const PlanChat::Reply parsed = PlanChat::parseReply(reply);
    if (parsed.message.isEmpty() && !parsed.plan) {
        m_chat->showNote(parsed.planCutOff ? tr("The answer was cut off before the revised plan was complete, so "
                                                "nothing was changed. Increase the model's context length in "
                                                "LM Studio and try again.")
                                           : tr("The model returned an empty answer."));
        return;
    }

    bool apply = parsed.plan.has_value();
    if (apply) {
        const QString reason = PlanChat::incompleteReason(editorText(m_planEdit), *parsed.plan);
        if (!reason.isEmpty()) {
            QMessageBox box(QMessageBox::Warning, tr("Revised Plan"),
                            tr("The revised plan from %1 looks incomplete: %2. Models sometimes return only the "
                               "changed part.\n\nReplace the plan with it anyway? (Ctrl+Z in the plan undoes it.)")
                                .arg(chatModel(), reason),
                            QMessageBox::NoButton, this);
            QPushButton *replace = box.addButton(tr("Replace Anyway"), QMessageBox::DestructiveRole);
            box.addButton(tr("Keep the Current Plan"), QMessageBox::RejectRole);
            box.exec();
            apply = box.clickedButton() == replace;
        }
    }
    if (apply) {
        // One undoable edit, so Ctrl+Z in the plan brings the previous version back.
        QTextCursor cursor(m_planEdit->document());
        cursor.beginEditBlock();
        cursor.select(QTextCursor::Document);
        cursor.insertText(*parsed.plan);
        cursor.endEditBlock();
        m_planEdit->moveCursor(QTextCursor::Start);
        savePlan();
    }

    QList<PlanChat::Turn> history;
    attempt(tr("Could not save the conversation."), [&] {
        history = PlanChat::parse(QString::fromStdString(m_store->planChat(*plan)));
        history.append({QStringLiteral("user"), message, false});
        QString content = parsed.message;
        if (parsed.plan && !apply) {
            content += (content.isEmpty() ? QString() : QStringLiteral("\n\n")) +
                       tr("(The revised plan looked incomplete and was not applied.)");
        }
        history.append({QStringLiteral("assistant"), content, apply});
        m_store->setPlanChat(*plan, PlanChat::serialize(history).toStdString());
    });
    m_chat->setHistory(history);
    updateChatState();
    if (parsed.planCutOff) {
        m_chat->showNote(tr("The revised plan was cut off (the model reached its length limit), so it was not "
                            "applied. Increase the model's context length in LM Studio and try again."));
    } else if (truncated) {
        m_chat->showNote(tr("The answer was cut off because the model reached its length limit."));
    }
}

void MainWindow::clearChat()
{
    if (!m_store || !m_currentPlan || m_chat->isBusy()) {
        return;
    }
    if (QMessageBox::question(this, tr("Clear Conversation"),
                              tr("Forget the conversation about this plan? The plan itself is kept.")) !=
        QMessageBox::Yes) {
        return;
    }
    const Id plan = *m_currentPlan;
    if (attempt(tr("Could not clear the conversation."), [&] { m_store->setPlanChat(plan, ""); })) {
        m_chat->setHistory({});
        updateChatState();
    }
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
