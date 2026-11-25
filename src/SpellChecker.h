#pragma once

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#include <memory>

class Hunspell;

// Spell checking with Hunspell, one dictionary (language) at a time, plus the user's personal dictionary (words
// added with "Add to Dictionary", shared by all languages and novels) and words ignored until the program ends.
class SpellChecker
{
  public:
    struct Dictionary {
        QString code;    // "en_US"
        QString name;    // "English (US)"
        QString dicPath; // .../en_US.dic
        QString affPath; // .../en_US.aff
    };

    // A word in a text: position and length in UTF-16 code units.
    struct Word {
        qsizetype start = 0;
        qsizetype length = 0;
    };

    SpellChecker();
    ~SpellChecker();

    // Folders searched for <code>.dic / <code>.aff pairs, in order: NOVELCLANKER_DICTIONARIES (a folder, if set),
    // "dictionaries" next to the program, and the user's own dictionary folder (for more languages).
    static QStringList dictionaryFolders();
    static QString userDictionaryFolder();
    // The dictionaries found in `folders`, sorted by name; the first folder wins for a code found twice.
    static QList<Dictionary> available(const QStringList &folders = dictionaryFolders());
    static QString displayName(const QString &code); // "en_GB" -> "English (UK)"

    // Loads a dictionary; false (and none loaded) if it can't be read.
    bool load(const Dictionary &dictionary);
    void unload();
    bool isLoaded() const;
    QString code() const; // of the loaded dictionary; empty if none

    // True if `word` is spelled correctly (or no dictionary is loaded).
    bool isCorrect(const QString &word) const;
    QStringList suggestions(const QString &word, int max = 7) const;

    // Adds `word` to the personal dictionary (saved at once) / ignores it until the program ends.
    void addToPersonalDictionary(const QString &word);
    void ignore(const QString &word);
    QString personalDictionaryPath() const;
    void setPersonalDictionaryPath(const QString &path); // default: in the user's app data folder; reloads

    // The words of `text` that are checked: runs of letters, with apostrophes inside ("don't", "Elin's"). Parts
    // of words joined to digits or '_' ("x2", "file_name") are left out.
    static QList<Word> words(const QString &text);

  private:
    static QString normalized(const QString &word); // curly apostrophe -> straight one, as the dictionaries spell it
    void loadPersonalDictionary();

    std::unique_ptr<Hunspell> m_hunspell;
    QString m_code;
    QString m_personalPath;
    QSet<QString> m_personal;
    QSet<QString> m_ignored;
    mutable QHash<QString, bool> m_cache;
};
