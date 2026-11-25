#include "SpellChecker.h"

#include <hunspell/hunspell.hxx>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>

namespace {

// Hunspell opens files with fopen(). On Windows it reads a UTF-8 path correctly only with the long-path prefix,
// so dictionaries in folders with non-ASCII names work too.
std::string hunspellPath(const QString &path)
{
    const QString absolute = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
#if defined(Q_OS_WIN)
    return (QStringLiteral("\\\\?\\") + absolute).toStdString();
#else
    return QFile::encodeName(absolute).toStdString();
#endif
}

} // namespace

SpellChecker::SpellChecker()
{
    setPersonalDictionaryPath(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                              QStringLiteral("/personal-dictionary.txt"));
}

SpellChecker::~SpellChecker() = default;

QStringList SpellChecker::dictionaryFolders()
{
    QStringList folders;
    if (const QString env = qEnvironmentVariable("NOVELCLANKER_DICTIONARIES"); !env.isEmpty()) {
        folders << env;
    }
    folders << QCoreApplication::applicationDirPath() + QStringLiteral("/dictionaries") << userDictionaryFolder();
    return folders;
}

QString SpellChecker::userDictionaryFolder()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/dictionaries");
}

QString SpellChecker::displayName(const QString &code)
{
    if (code == QLatin1String("en_US")) {
        return QStringLiteral("English (US)");
    }
    if (code == QLatin1String("en_GB")) {
        return QStringLiteral("English (UK)");
    }
    const QLocale locale(code);
    if (locale.language() == QLocale::C) {
        return code;
    }
    const QString language = QLocale::languageToString(locale.language());
    return code.contains('_') ? QStringLiteral("%1 (%2)").arg(language, QLocale::territoryToString(locale.territory()))
                              : language;
}

QList<SpellChecker::Dictionary> SpellChecker::available(const QStringList &folders)
{
    QList<Dictionary> found;
    QSet<QString> codes;
    for (const QString &folder : folders) {
        const QDir dir(folder);
        for (const QFileInfo &dic : dir.entryInfoList({QStringLiteral("*.dic")}, QDir::Files, QDir::Name)) {
            const QString code = dic.completeBaseName();
            const QString aff = dir.filePath(code + QStringLiteral(".aff"));
            if (codes.contains(code) || !QFileInfo::exists(aff)) {
                continue; // first folder wins; a .dic without its .aff is not a dictionary
            }
            codes.insert(code);
            found.append({code, displayName(code), dic.absoluteFilePath(), QFileInfo(aff).absoluteFilePath()});
        }
    }
    std::sort(found.begin(), found.end(), [](const Dictionary &a, const Dictionary &b) { return a.name < b.name; });
    return found;
}

bool SpellChecker::load(const Dictionary &dictionary)
{
    unload();
    if (!QFileInfo::exists(dictionary.affPath) || !QFileInfo::exists(dictionary.dicPath)) {
        return false;
    }
    auto hunspell = std::make_unique<Hunspell>(hunspellPath(dictionary.affPath).c_str(),
                                               hunspellPath(dictionary.dicPath).c_str());
    // A dictionary that failed to load knows no words at all.
    if (!hunspell->spell(std::string("the")) && !hunspell->spell(std::string("a"))) {
        return false;
    }
    m_hunspell = std::move(hunspell);
    m_code = dictionary.code;
    for (const QString &word : std::as_const(m_personal)) {
        m_hunspell->add(word.toStdString());
    }
    return true;
}

void SpellChecker::unload()
{
    m_hunspell.reset();
    m_code.clear();
    m_cache.clear();
}

bool SpellChecker::isLoaded() const
{
    return m_hunspell != nullptr;
}

QString SpellChecker::code() const
{
    return m_code;
}

QString SpellChecker::normalized(const QString &word)
{
    QString out = word;
    out.replace(QChar(0x2019), QLatin1Char('\''));
    return out;
}

bool SpellChecker::isCorrect(const QString &word) const
{
    if (!m_hunspell || word.isEmpty()) {
        return true;
    }
    const QString w = normalized(word);
    if (m_ignored.contains(w) || m_personal.contains(w)) {
        return true;
    }
    if (const auto it = m_cache.constFind(w); it != m_cache.constEnd()) {
        return *it;
    }
    bool correct = m_hunspell->spell(w.toStdString());
    // "Elin's": the possessive of a word the dictionary (or the user) knows is fine too.
    if (!correct && w.endsWith(QLatin1String("'s"), Qt::CaseInsensitive) && w.size() > 2) {
        correct = isCorrect(w.chopped(2));
    }
    m_cache.insert(w, correct);
    return correct;
}

QStringList SpellChecker::suggestions(const QString &word, int max) const
{
    QStringList out;
    if (!m_hunspell) {
        return out;
    }
    const bool curly = word.contains(QChar(0x2019));
    for (const std::string &s : m_hunspell->suggest(normalized(word).toStdString())) {
        QString suggestion = QString::fromStdString(s);
        if (curly) {
            suggestion.replace(QLatin1Char('\''), QChar(0x2019)); // keep the author's apostrophes
        }
        if (!out.contains(suggestion)) {
            out << suggestion;
        }
        if (out.size() >= max) {
            break;
        }
    }
    return out;
}

void SpellChecker::addToPersonalDictionary(const QString &word)
{
    const QString w = normalized(word).trimmed();
    if (w.isEmpty() || m_personal.contains(w)) {
        return;
    }
    m_personal.insert(w);
    if (m_hunspell) {
        m_hunspell->add(w.toStdString());
    }
    m_cache.clear(); // also forms of the word that were checked before ("Lindqvist's")

    QDir().mkpath(QFileInfo(m_personalPath).absolutePath());
    QStringList sorted(m_personal.begin(), m_personal.end());
    sorted.sort(Qt::CaseInsensitive);
    QSaveFile file(m_personalPath);
    if (file.open(QIODevice::WriteOnly)) { // binary: UTF-8 with LF line ends on every platform
        file.write((sorted.join('\n') + '\n').toUtf8());
        file.commit();
    }
}

void SpellChecker::ignore(const QString &word)
{
    const QString w = normalized(word);
    m_ignored.insert(w);
    m_cache.clear();
}

QString SpellChecker::personalDictionaryPath() const
{
    return m_personalPath;
}

void SpellChecker::setPersonalDictionaryPath(const QString &path)
{
    m_personalPath = path;
    loadPersonalDictionary();
}

void SpellChecker::loadPersonalDictionary()
{
    m_personal.clear();
    m_cache.clear();
    QFile file(m_personalPath);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        for (const QString &line : QString::fromUtf8(file.readAll()).split('\n')) {
            if (const QString word = normalized(line).trimmed(); !word.isEmpty()) {
                m_personal.insert(word);
                if (m_hunspell) {
                    m_hunspell->add(word.toStdString());
                }
            }
        }
    }
}

QList<SpellChecker::Word> SpellChecker::words(const QString &text)
{
    // Letters (with combining marks), joined by apostrophes inside a word.
    static const QRegularExpression word(QStringLiteral(R"([\p{L}\p{M}]+(?:['\x{2019}][\p{L}\p{M}]+)*)"),
                                         QRegularExpression::UseUnicodePropertiesOption);
    QList<Word> out;
    for (auto it = word.globalMatch(text); it.hasNext();) {
        const auto m = it.next();
        const qsizetype start = m.capturedStart(), end = m.capturedEnd();
        const auto joined = [&](qsizetype i) {
            return i >= 0 && i < text.size() && (text[i].isDigit() || text[i] == QLatin1Char('_'));
        };
        if (!joined(start - 1) && !joined(end)) {
            out.append({start, end - start});
        }
    }
    return out;
}
