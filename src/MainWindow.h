#pragma once

#include <QMainWindow>

class QPlainTextEdit;
class MarkdownHighlighter;

class MainWindow : public QMainWindow
{
    Q_OBJECT

  public:
    explicit MainWindow(QWidget *parent = nullptr);

  protected:
    void closeEvent(QCloseEvent *event) override;

  private:
    void newFile();
    void openFile();
    bool saveFile();
    bool saveFileAs();
    bool writeTo(const QString &path);
    bool maybeSave();
    void setCurrentFile(const QString &path);
    void updateStatus();

    QPlainTextEdit *m_editor = nullptr;
    MarkdownHighlighter *m_highlighter = nullptr;
    QString m_currentFile;
};
