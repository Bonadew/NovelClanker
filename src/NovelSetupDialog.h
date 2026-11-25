#pragma once

#include <QDialog>
#include <QStringList>

#include <map>
#include <string>

class NovelStore;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QRadioButton;
class QTableWidget;

// The novel's book details for publishing: title, author, series, description, keywords and the other things
// Amazon KDP asks for when publishing an eBook. Stored in the novel (NovelStore::setDetails). Details can be filled
// in over time: saving is always possible, and the dialog lists what is missing or breaks KDP's rules.
class NovelSetupDialog : public QDialog
{
    Q_OBJECT

  public:
    // `defaultTitle` is suggested when the novel has no title yet (e.g. the novel's file name).
    NovelSetupDialog(NovelStore &store, const QString &defaultTitle, QWidget *parent = nullptr);

    // The values currently in the dialog, by detail name (see docs/storage-format.md).
    std::map<std::string, std::string> values() const;

    // What `values` lacks for publishing on KDP, or where it breaks KDP's rules; empty if nothing.
    static QStringList problems(const std::map<std::string, std::string> &values);

    void accept() override; // saves, then closes

  private:
    void load(const QString &defaultTitle);
    void updateChecks();
    void addContributor(const QString &role = {}, const QString &name = {});

    NovelStore &m_store;
    QLineEdit *m_title;
    QLineEdit *m_subtitle;
    QLineEdit *m_seriesName;
    QLineEdit *m_seriesNumber;
    QLineEdit *m_edition;
    QComboBox *m_language;
    QLineEdit *m_author;
    QTableWidget *m_contributors;
    QPlainTextEdit *m_description;
    QLabel *m_descriptionCount;
    QList<QLineEdit *> m_keywords;
    QList<QLineEdit *> m_categories;
    QLineEdit *m_publisher;
    QLineEdit *m_isbn;
    QRadioButton *m_rightsOwn;
    QRadioButton *m_rightsPublicDomain;
    QRadioButton *m_explicitNo;
    QRadioButton *m_explicitYes;
    QComboBox *m_readingAgeMin;
    QComboBox *m_readingAgeMax;
    QLabel *m_checks;
};
