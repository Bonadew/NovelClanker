#include "MainWindow.h"

#include "MarkdownHighlighter.h"

#include <QAction>
#include <QCloseEvent>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QListWidget>
#include <QPushButton>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimer>

#include <algorithm>

namespace {

constexpr int kAutosaveDelayMs = 1500;
constexpr auto kLastNovelKey = "lastNovel";

// A chapter's title is its first non-empty line, minus any Markdown heading markers.
QString chapterTitle(const QString &markdown)
{
    for (const QStringView line : QStringView(markdown).split(u'\n')) {
        QString title = line.trimmed().toString();
        title.remove(QRegularExpression(R"(^#+\s*)"));
        if (!title.isEmpty()) {
            return title.size() > 60 ? title.left(60) + QChar(0x2026) : title;
        }
    }
    return QObject::tr("Untitled chapter");
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

NovelStore::ChapterId itemChapterId(const QListWidgetItem *item)
{
    return item->data(Qt::UserRole).toULongLong();
}

} // namespace

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    m_chapterList = new QListWidget(this);
    m_chapterList->setDragDropMode(QAbstractItemView::InternalMove);
    m_chapterList->setDefaultDropAction(Qt::MoveAction);

    m_editor = new QPlainTextEdit(this);
    QFont font = m_editor->font();
    font.setPointSize(12);
    m_editor->setFont(font);
    m_editor->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_editor->document()->setDocumentMargin(24);
    m_highlighter = new MarkdownHighlighter(m_editor->document());

    auto *splitter = new QSplitter(this);
    splitter->addWidget(m_chapterList);
    splitter->addWidget(m_editor);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({220, 780});
    setCentralWidget(splitter);

    m_autosaveTimer = new QTimer(this);
    m_autosaveTimer->setSingleShot(true);
    m_autosaveTimer->setInterval(kAutosaveDelayMs);
    connect(m_autosaveTimer, &QTimer::timeout, this, &MainWindow::saveCurrentChapter);

    QMenu *fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(tr("&New Novel..."), this, &MainWindow::newNovel);
    fileMenu->addAction(tr("&Open Novel..."), QKeySequence::Open, this, &MainWindow::openNovelDialog);
    m_saveAction = fileMenu->addAction(tr("&Save"), QKeySequence::Save, this, &MainWindow::saveCurrentChapter);
    m_exportAction = fileMenu->addAction(tr("&Export as Markdown..."), this, &MainWindow::exportMarkdown);
    fileMenu->addSeparator();
    fileMenu->addAction(tr("E&xit"), QKeySequence::Quit, this, &QWidget::close);

    QMenu *editMenu = menuBar()->addMenu(tr("&Edit"));
    editMenu->addAction(tr("&Undo"), QKeySequence::Undo, m_editor, &QPlainTextEdit::undo);
    editMenu->addAction(tr("&Redo"), QKeySequence::Redo, m_editor, &QPlainTextEdit::redo);
    editMenu->addSeparator();
    editMenu->addAction(tr("Cu&t"), QKeySequence::Cut, m_editor, &QPlainTextEdit::cut);
    editMenu->addAction(tr("&Copy"), QKeySequence::Copy, m_editor, &QPlainTextEdit::copy);
    editMenu->addAction(tr("&Paste"), QKeySequence::Paste, m_editor, &QPlainTextEdit::paste);

    QMenu *chapterMenu = menuBar()->addMenu(tr("&Chapter"));
    m_addChapterAction = chapterMenu->addAction(tr("&Add Chapter"), QKeySequence(tr("Ctrl+Shift+N")), this,
                                                &MainWindow::addChapter);
    m_deleteChapterAction = chapterMenu->addAction(tr("&Delete Chapter..."), this, &MainWindow::deleteChapter);
    chapterMenu->addSeparator();
    m_moveUpAction =
        chapterMenu->addAction(tr("Move &Up"), QKeySequence(tr("Alt+Up")), this, [this] { moveChapter(-1); });
    m_moveDownAction =
        chapterMenu->addAction(tr("Move D&own"), QKeySequence(tr("Alt+Down")), this, [this] { moveChapter(1); });

    connect(m_chapterList, &QListWidget::currentRowChanged, this, &MainWindow::showChapter);
    connect(m_chapterList->model(), &QAbstractItemModel::rowsMoved, this,
            [this](const QModelIndex &, int start, int, const QModelIndex &, int row) {
                // `row` is the insertion point before the move; adjust when moving downwards.
                onRowsMoved(start, row > start ? row - 1 : row);
            });
    connect(m_editor, &QPlainTextEdit::textChanged, this, &MainWindow::onTextChanged);

    showChapter(-1);
    updateTitle();
}

MainWindow::~MainWindow() = default;

bool MainWindow::openNovel(const QString &dir, NovelStore::Mode mode)
{
    if (m_store && mode == NovelStore::Mode::Open &&
        QFileInfo(dir).canonicalFilePath() == QFileInfo(QString::fromStdU16String(m_store->path().u16string()))
                                                  .canonicalFilePath()) {
        return true; // already open (RocksDB would refuse a second lock anyway)
    }
    saveCurrentChapter();

    std::unique_ptr<NovelStore> store;
    try {
        store = std::make_unique<NovelStore>(toPath(dir), mode);
        if (mode == NovelStore::Mode::Create) {
            store->insertChapter(0, tr("# Chapter 1\n\n").toStdString());
        }
    } catch (const std::exception &e) {
        showError(mode == NovelStore::Mode::Create ? tr("Could not create the novel.") : tr("Could not open the novel."),
                  e);
        return false;
    }

    m_autosaveTimer->stop();
    m_currentChapter.reset();
    m_store = std::move(store);
    QSettings().setValue(kLastNovelKey, dir);
    reloadChapterList(0);
    updateTitle();
    return true;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (saveCurrentChapter() ||
        QMessageBox::question(this, tr("NovelWriter"), tr("The current chapter could not be saved. Quit anyway?")) ==
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
    const QString start = m_store ? QFileInfo(QString::fromStdU16String(m_store->path().u16string())).path()
                                  : QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Open Novel (select the .novel folder)"), start);
    if (!dir.isEmpty()) {
        openNovel(dir, NovelStore::Mode::Open);
    }
}

void MainWindow::exportMarkdown()
{
    if (!m_store || !saveCurrentChapter()) {
        return;
    }
    const QString base = QFileInfo(QString::fromStdU16String(m_store->path().u16string())).completeBaseName();
    const QString path =
        QFileDialog::getSaveFileName(this, tr("Export as Markdown"), base + ".md", tr("Markdown (*.md)"));
    if (path.isEmpty()) {
        return;
    }
    try {
        std::string out;
        for (NovelStore::ChapterId id : m_store->chapters()) {
            std::string text = m_store->chapterText(id);
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
                text.pop_back();
            }
            out += text;
            out += "\n\n";
        }
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(out.data(), static_cast<qint64>(out.size())) < 0 ||
            !file.commit()) {
            throw std::runtime_error(file.errorString().toStdString());
        }
        statusBar()->showMessage(tr("Exported to %1").arg(QDir::toNativeSeparators(path)), 4000);
    } catch (const std::exception &e) {
        showError(tr("Could not export the novel."), e);
    }
}

void MainWindow::addChapter()
{
    if (!m_store || !saveCurrentChapter()) {
        return;
    }
    const int row = m_chapterList->currentRow() + 1; // after the current chapter (or first if none)
    const QString heading = tr("# Chapter %1\n\n").arg(m_store->chapters().size() + 1);
    try {
        m_store->insertChapter(static_cast<std::size_t>(row), heading.toStdString());
    } catch (const std::exception &e) {
        showError(tr("Could not add a chapter."), e);
        return;
    }
    reloadChapterList(row);
    m_editor->moveCursor(QTextCursor::End);
    m_editor->setFocus();
}

void MainWindow::deleteChapter()
{
    QListWidgetItem *item = currentItem();
    if (!m_store || !item) {
        return;
    }
    QMessageBox box(QMessageBox::Warning, tr("Delete Chapter"),
                    tr("Permanently delete \"%1\"? This cannot be undone.").arg(item->text()),
                    QMessageBox::Cancel, this);
    QAbstractButton *deleteButton = box.addButton(tr("Delete"), QMessageBox::DestructiveRole);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != deleteButton) {
        return;
    }
    const int row = m_chapterList->currentRow();
    m_autosaveTimer->stop();
    m_currentChapter.reset(); // discard pending edits of the chapter being deleted
    try {
        m_store->removeChapter(itemChapterId(item));
    } catch (const std::exception &e) {
        showError(tr("Could not delete the chapter."), e);
    }
    reloadChapterList(row);
}

void MainWindow::moveChapter(int delta)
{
    const int from = m_chapterList->currentRow();
    const int to = from + delta;
    if (!m_store || from < 0 || to < 0 || to >= m_chapterList->count() || !saveCurrentChapter()) {
        return;
    }
    try {
        m_store->moveChapter(static_cast<std::size_t>(from), static_cast<std::size_t>(to));
    } catch (const std::exception &e) {
        showError(tr("Could not move the chapter."), e);
        return;
    }
    reloadChapterList(to);
}

void MainWindow::onRowsMoved(int from, int to)
{
    // The list has already been reordered by drag and drop; persist the new order.
    try {
        m_store->moveChapter(static_cast<std::size_t>(from), static_cast<std::size_t>(to));
    } catch (const std::exception &e) {
        showError(tr("Could not move the chapter."), e);
        reloadChapterList(from);
    }
}

void MainWindow::showChapter(int row)
{
    if (m_loading) {
        return;
    }
    saveCurrentChapter();
    m_autosaveTimer->stop();

    QListWidgetItem *item = row >= 0 ? m_chapterList->item(row) : nullptr;
    QString text;
    if (item) {
        try {
            text = QString::fromStdString(m_store->chapterText(itemChapterId(item)));
        } catch (const std::exception &e) {
            showError(tr("Could not load the chapter."), e);
            item = nullptr;
        }
    }

    m_loading = true;
    m_editor->setPlainText(text); // also resets undo history, which is per chapter
    m_loading = false;
    m_editor->document()->setModified(false);
    m_currentChapter = item ? std::optional(itemChapterId(item)) : std::nullopt;

    m_editor->setEnabled(item != nullptr);
    m_editor->setPlaceholderText(m_store ? tr("Add a chapter to start writing.")
                                         : tr("Create or open a novel from the File menu to start writing."));
    updateActions();
    updateStatus();
}

bool MainWindow::saveCurrentChapter()
{
    m_autosaveTimer->stop();
    if (!m_store || !m_currentChapter || !m_editor->document()->isModified()) {
        return true;
    }
    try {
        m_store->setChapterText(*m_currentChapter, m_editor->toPlainText().toStdString());
    } catch (const std::exception &e) {
        showError(tr("Could not save the chapter."), e);
        return false;
    }
    m_editor->document()->setModified(false);
    updateStatus();
    return true;
}

void MainWindow::onTextChanged()
{
    if (m_loading || !m_currentChapter) {
        return;
    }
    const QString text = m_editor->toPlainText();
    if (QListWidgetItem *item = currentItem()) {
        item->setText(chapterTitle(text));
    }
    m_wordCounts[*m_currentChapter] = countWords(text);
    m_autosaveTimer->start();
    updateStatus();
}

void MainWindow::reloadChapterList(int selectRow)
{
    m_loading = true;
    m_chapterList->clear();
    m_wordCounts.clear();
    if (m_store) {
        try {
            for (NovelStore::ChapterId id : m_store->chapters()) {
                const QString text = QString::fromStdString(m_store->chapterText(id));
                auto *item = new QListWidgetItem(chapterTitle(text), m_chapterList);
                item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(id));
                m_wordCounts[id] = countWords(text);
            }
        } catch (const std::exception &e) {
            showError(tr("Could not read the chapter list."), e);
        }
    }
    m_loading = false;

    // The current chapter was saved (or discarded) before reloading; don't save it again over new content.
    m_currentChapter.reset();
    const int row = std::min(selectRow, m_chapterList->count() - 1);
    if (row >= 0 && m_chapterList->currentRow() != row) {
        m_chapterList->setCurrentRow(row); // triggers showChapter
    } else {
        showChapter(row);
    }
}

void MainWindow::updateActions()
{
    const bool haveNovel = m_store != nullptr;
    const int row = m_chapterList->currentRow();
    m_saveAction->setEnabled(haveNovel);
    m_exportAction->setEnabled(haveNovel);
    m_addChapterAction->setEnabled(haveNovel);
    m_deleteChapterAction->setEnabled(row >= 0);
    m_moveUpAction->setEnabled(row > 0);
    m_moveDownAction->setEnabled(row >= 0 && row + 1 < m_chapterList->count());
}

void MainWindow::updateStatus()
{
    if (!m_store) {
        statusBar()->clearMessage();
        return;
    }
    qsizetype total = 0;
    for (const auto &[id, words] : m_wordCounts) {
        total += words;
    }
    QString message = tr("Novel: %1 words").arg(total);
    if (m_currentChapter) {
        message = tr("Chapter: %1 words  ·  %2  ·  %3")
                      .arg(m_wordCounts[*m_currentChapter])
                      .arg(message)
                      .arg(m_editor->document()->isModified() ? tr("Editing") : tr("Saved"));
    }
    statusBar()->showMessage(message);
}

void MainWindow::updateTitle()
{
    setWindowTitle(m_store ? QFileInfo(QString::fromStdU16String(m_store->path().u16string())).completeBaseName() +
                                 " - NovelWriter"
                           : QStringLiteral("NovelWriter"));
}

QListWidgetItem *MainWindow::currentItem() const
{
    return m_chapterList->currentItem();
}

void MainWindow::showError(const QString &what, const std::exception &e)
{
    QMessageBox::critical(this, tr("NovelWriter"), what + "\n\n" + QString::fromUtf8(e.what()));
}
