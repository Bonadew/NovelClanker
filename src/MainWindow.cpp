#include "MainWindow.h"

#include "MarkdownHighlighter.h"

#include <QAction>
#include <QCloseEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStatusBar>
#include <QTextStream>

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    m_editor = new QPlainTextEdit(this);
    QFont font = m_editor->font();
    font.setPointSize(12);
    m_editor->setFont(font);
    m_editor->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_editor->document()->setDocumentMargin(24);
    setCentralWidget(m_editor);

    m_highlighter = new MarkdownHighlighter(m_editor->document());

    QMenu *fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(tr("&New"), QKeySequence::New, this, &MainWindow::newFile);
    fileMenu->addAction(tr("&Open..."), QKeySequence::Open, this, &MainWindow::openFile);
    fileMenu->addAction(tr("&Save"), QKeySequence::Save, this, &MainWindow::saveFile);
    fileMenu->addAction(tr("Save &As..."), QKeySequence::SaveAs, this, &MainWindow::saveFileAs);
    fileMenu->addSeparator();
    fileMenu->addAction(tr("E&xit"), QKeySequence::Quit, this, &QWidget::close);

    QMenu *editMenu = menuBar()->addMenu(tr("&Edit"));
    editMenu->addAction(tr("&Undo"), QKeySequence::Undo, m_editor, &QPlainTextEdit::undo);
    editMenu->addAction(tr("&Redo"), QKeySequence::Redo, m_editor, &QPlainTextEdit::redo);
    editMenu->addSeparator();
    editMenu->addAction(tr("Cu&t"), QKeySequence::Cut, m_editor, &QPlainTextEdit::cut);
    editMenu->addAction(tr("&Copy"), QKeySequence::Copy, m_editor, &QPlainTextEdit::copy);
    editMenu->addAction(tr("&Paste"), QKeySequence::Paste, m_editor, &QPlainTextEdit::paste);

    connect(m_editor->document(), &QTextDocument::modificationChanged, this, &QWidget::setWindowModified);
    connect(m_editor, &QPlainTextEdit::textChanged, this, &MainWindow::updateStatus);

    setCurrentFile({});
    updateStatus();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (maybeSave()) {
        event->accept();
    } else {
        event->ignore();
    }
}

void MainWindow::newFile()
{
    if (maybeSave()) {
        m_editor->clear();
        setCurrentFile({});
    }
}

void MainWindow::openFile()
{
    if (!maybeSave()) {
        return;
    }
    const QString path = QFileDialog::getOpenFileName(this, tr("Open"), {},
                                                      tr("Markdown (*.md *.markdown *.txt);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Open"), tr("Cannot read %1:\n%2").arg(path, file.errorString()));
        return;
    }
    QTextStream in(&file);
    m_editor->setPlainText(in.readAll());
    setCurrentFile(path);
}

bool MainWindow::saveFile()
{
    return m_currentFile.isEmpty() ? saveFileAs() : writeTo(m_currentFile);
}

bool MainWindow::saveFileAs()
{
    const QString path =
        QFileDialog::getSaveFileName(this, tr("Save As"), m_currentFile, tr("Markdown (*.md);;All files (*)"));
    return !path.isEmpty() && writeTo(path);
}

bool MainWindow::writeTo(const QString &path)
{
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&file);
        out << m_editor->toPlainText();
        out.flush();
        if (file.commit()) {
            setCurrentFile(path);
            statusBar()->showMessage(tr("Saved"), 2000);
            return true;
        }
    }
    QMessageBox::warning(this, tr("Save"), tr("Cannot write %1:\n%2").arg(path, file.errorString()));
    return false;
}

bool MainWindow::maybeSave()
{
    if (!m_editor->document()->isModified()) {
        return true;
    }
    const auto choice =
        QMessageBox::warning(this, tr("NovelWriter"), tr("The document has unsaved changes. Save them?"),
                             QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (choice == QMessageBox::Save) {
        return saveFile();
    }
    return choice == QMessageBox::Discard;
}

void MainWindow::setCurrentFile(const QString &path)
{
    m_currentFile = path;
    m_editor->document()->setModified(false);
    setWindowModified(false);
    const QString shown = path.isEmpty() ? tr("Untitled") : QFileInfo(path).fileName();
    setWindowTitle(shown + "[*] - NovelWriter");
}

void MainWindow::updateStatus()
{
    const QString text = m_editor->toPlainText();
    const auto words = text.split(QRegularExpression(R"(\s+)"), Qt::SkipEmptyParts).size();
    statusBar()->showMessage(tr("%1 words").arg(words));
}
