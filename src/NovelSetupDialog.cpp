#include "NovelSetupDialog.h"

#include "NovelStore.h"

#include <QButtonGroup>
#include <QGridLayout>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <stdexcept>

namespace {

// Limits from Amazon KDP's eBook metadata guidelines.
constexpr int kTitleAndSubtitleLimit = 200; // characters, title and subtitle together, fewer than this
constexpr int kDescriptionLimit = 4000;     // characters
constexpr int kKeywordCount = 7;
constexpr int kCategoryCount = 3;

const QStringList kLanguages{"English", "German", "French", "Spanish", "Italian", "Portuguese", "Dutch", "Swedish",
                             "Danish", "Norwegian", "Finnish", "Icelandic", "Polish", "Czech", "Japanese", "Chinese",
                             "Hindi", "Arabic", "Hebrew", "Greek", "Turkish", "Ukrainian", "Russian", "Latin"};
const QStringList kContributorRoles{"Author",       "Editor",    "Foreword",   "Illustrator", "Introduction",
                                    "Narrator",     "Photographer", "Preface", "Translator",  "Contributions by"};

QString q(const std::string &s)
{
    return QString::fromStdString(s);
}

std::string s(const QString &q)
{
    return q.trimmed().toStdString();
}

QString value(const std::map<std::string, std::string> &values, const char *name)
{
    const auto it = values.find(name);
    return it == values.end() ? QString() : q(it->second).trimmed();
}

QLineEdit *lineEdit(QWidget *parent, const char *name, const QString &placeholder = {})
{
    auto *edit = new QLineEdit(parent);
    edit->setObjectName(QStringLiteral("detail_") + name);
    edit->setPlaceholderText(placeholder);
    return edit;
}

QString required(const QString &label)
{
    return label + QStringLiteral(" *");
}

} // namespace

NovelSetupDialog::NovelSetupDialog(NovelStore &store, const QString &defaultTitle, QWidget *parent)
    : QDialog(parent), m_store(store)
{
    setWindowTitle(tr("Novel Setup"));
    setObjectName("novelSetupDialog");

    auto *tabs = new QTabWidget(this);

    // --- Book ---------------------------------------------------------------------------------------------------
    auto *book = new QWidget(tabs);
    auto *bookForm = new QFormLayout(book);
    m_title = lineEdit(book, "title", tr("As on the cover"));
    m_subtitle = lineEdit(book, "subtitle", tr("Optional"));
    m_seriesName = lineEdit(book, "series_name", tr("Optional: the series' name only"));
    m_seriesNumber = lineEdit(book, "series_number", tr("e.g. 3"));
    m_edition = lineEdit(book, "edition", tr("Optional, e.g. 2"));
    m_language = new QComboBox(book);
    m_language->setObjectName("detail_language");
    m_language->setEditable(true);
    m_language->addItem(QString());
    m_language->addItems(kLanguages);
    bookForm->addRow(required(tr("Title")), m_title);
    bookForm->addRow(tr("Subtitle"), m_subtitle);
    auto *series = new QHBoxLayout;
    series->addWidget(m_seriesName, 3);
    series->addWidget(new QLabel(tr("Number"), book));
    series->addWidget(m_seriesNumber, 1);
    bookForm->addRow(tr("Series"), series);
    bookForm->addRow(tr("Edition"), m_edition);
    bookForm->addRow(required(tr("Language")), m_language);
    auto *locked = new QLabel(tr("On KDP, language, title, subtitle, edition and primary author can't be changed "
                                 "after publishing."),
                              book);
    locked->setWordWrap(true);
    bookForm->addRow(locked);
    tabs->addTab(book, tr("Book"));

    // --- People -------------------------------------------------------------------------------------------------
    auto *people = new QWidget(tabs);
    auto *peopleLayout = new QVBoxLayout(people);
    auto *authorForm = new QFormLayout;
    m_author = lineEdit(people, "author", tr("Your name or pen name"));
    authorForm->addRow(required(tr("Author")), m_author);
    peopleLayout->addLayout(authorForm);
    peopleLayout->addWidget(new QLabel(tr("Contributors (co-authors, editors, illustrators, translators, ...):"),
                                       people));
    m_contributors = new QTableWidget(0, 2, people);
    m_contributors->setObjectName("detail_contributors");
    m_contributors->setHorizontalHeaderLabels({tr("Role"), tr("Name")});
    m_contributors->horizontalHeader()->setStretchLastSection(true);
    m_contributors->verticalHeader()->hide();
    peopleLayout->addWidget(m_contributors, 1);
    auto *contributorButtons = new QHBoxLayout;
    auto *addContributorButton = new QPushButton(tr("Add"), people);
    addContributorButton->setObjectName("addContributorButton");
    auto *removeContributorButton = new QPushButton(tr("Remove"), people);
    contributorButtons->addWidget(addContributorButton);
    contributorButtons->addWidget(removeContributorButton);
    contributorButtons->addStretch();
    peopleLayout->addLayout(contributorButtons);
    connect(addContributorButton, &QPushButton::clicked, this, [this] { addContributor(); });
    connect(removeContributorButton, &QPushButton::clicked, this, [this] {
        if (m_contributors->currentRow() >= 0) {
            m_contributors->removeRow(m_contributors->currentRow());
            updateChecks();
        }
    });
    tabs->addTab(people, tr("People"));

    // --- Description and keywords -------------------------------------------------------------------------------
    auto *about = new QWidget(tabs);
    auto *aboutLayout = new QVBoxLayout(about);
    aboutLayout->addWidget(new QLabel(tr("Description (shown on the book's page; no contact details, prices, "
                                         "reviews or spoilers):"),
                                      about));
    m_description = new QPlainTextEdit(about);
    m_description->setObjectName("detail_description");
    aboutLayout->addWidget(m_description, 1);
    m_descriptionCount = new QLabel(about);
    m_descriptionCount->setObjectName("descriptionCount");
    m_descriptionCount->setAlignment(Qt::AlignRight);
    aboutLayout->addWidget(m_descriptionCount);
    auto *keywords = new QGroupBox(tr("Keywords (up to %1 words or short phrases readers might search for)")
                                       .arg(kKeywordCount),
                                   about);
    auto *keywordGrid = new QGridLayout(keywords);
    for (int i = 0; i < kKeywordCount; ++i) {
        auto *edit = new QLineEdit(keywords);
        edit->setObjectName(QStringLiteral("detail_keyword%1").arg(i + 1));
        keywordGrid->addWidget(edit, i / 2, i % 2);
        m_keywords << edit;
    }
    aboutLayout->addWidget(keywords);
    tabs->addTab(about, tr("Description"));

    // --- Publishing ---------------------------------------------------------------------------------------------
    auto *publishing = new QWidget(tabs);
    auto *publishingForm = new QFormLayout(publishing);
    m_rightsOwn = new QRadioButton(tr("I own the copyright and hold the publishing rights"), publishing);
    m_rightsOwn->setObjectName("detail_rights_own");
    m_rightsPublicDomain = new QRadioButton(tr("This is a public domain work"), publishing);
    auto *rights = new QVBoxLayout;
    rights->addWidget(m_rightsOwn);
    rights->addWidget(m_rightsPublicDomain);
    auto *rightsGroup = new QButtonGroup(this);
    rightsGroup->addButton(m_rightsOwn);
    rightsGroup->addButton(m_rightsPublicDomain);
    publishingForm->addRow(required(tr("Publishing rights")), rights);

    m_explicitNo = new QRadioButton(tr("No"), publishing);
    m_explicitNo->setObjectName("detail_explicit_no");
    m_explicitYes = new QRadioButton(tr("Yes"), publishing);
    auto *explicitRow = new QHBoxLayout;
    explicitRow->addWidget(m_explicitNo);
    explicitRow->addWidget(m_explicitYes);
    explicitRow->addStretch();
    auto *explicitGroup = new QButtonGroup(this);
    explicitGroup->addButton(m_explicitNo);
    explicitGroup->addButton(m_explicitYes);
    publishingForm->addRow(required(tr("Sexually explicit content")), explicitRow);

    m_readingAgeMin = new QComboBox(publishing);
    m_readingAgeMin->setObjectName("detail_reading_age_min");
    m_readingAgeMax = new QComboBox(publishing);
    m_readingAgeMax->setObjectName("detail_reading_age_max");
    m_readingAgeMin->addItem(tr("Not set"), QString());
    m_readingAgeMin->addItem(tr("Baby"), QStringLiteral("0"));
    m_readingAgeMax->addItem(tr("Not set"), QString());
    for (int age = 1; age <= 18; ++age) {
        m_readingAgeMin->addItem(QString::number(age), QString::number(age));
        m_readingAgeMax->addItem(age == 18 ? QStringLiteral("18+") : QString::number(age),
                                 age == 18 ? QStringLiteral("18+") : QString::number(age));
    }
    auto *readingAge = new QHBoxLayout;
    readingAge->addWidget(m_readingAgeMin);
    readingAge->addWidget(new QLabel(tr("to"), publishing));
    readingAge->addWidget(m_readingAgeMax);
    readingAge->addStretch();
    publishingForm->addRow(tr("Reading age"), readingAge);

    auto *categories = new QVBoxLayout;
    for (int i = 0; i < kCategoryCount; ++i) {
        auto *edit = new QLineEdit(publishing);
        edit->setObjectName(QStringLiteral("detail_category%1").arg(i + 1));
        edit->setPlaceholderText(i == 0 ? tr("e.g. Fiction > Literary") : QString());
        categories->addWidget(edit);
        m_categories << edit;
    }
    publishingForm->addRow(required(tr("Categories")), categories);
    m_publisher = lineEdit(publishing, "publisher", tr("Optional: your name or publishing company"));
    publishingForm->addRow(tr("Publisher"), m_publisher);
    m_isbn = lineEdit(publishing, "isbn", tr("Optional for eBooks; needed for print"));
    publishingForm->addRow(tr("ISBN"), m_isbn);
    tabs->addTab(publishing, tr("Publishing"));

    // --- Checks and buttons -------------------------------------------------------------------------------------
    m_checks = new QLabel(this);
    m_checks->setObjectName("setupChecks");
    m_checks->setWordWrap(true);
    m_checks->setTextFormat(Qt::PlainText);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &NovelSetupDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(tabs, 1);
    layout->addWidget(new QLabel(tr("* Required by Amazon KDP to publish an eBook."), this));
    layout->addWidget(m_checks);
    layout->addWidget(buttons);
    resize(640, 560);

    load(defaultTitle);
    for (QLineEdit *edit : findChildren<QLineEdit *>()) {
        connect(edit, &QLineEdit::textChanged, this, &NovelSetupDialog::updateChecks);
    }
    connect(m_description, &QPlainTextEdit::textChanged, this, &NovelSetupDialog::updateChecks);
    for (QRadioButton *radio : {m_rightsOwn, m_rightsPublicDomain, m_explicitNo, m_explicitYes}) {
        connect(radio, &QRadioButton::toggled, this, &NovelSetupDialog::updateChecks);
    }
    for (QComboBox *combo : {m_language, m_readingAgeMin, m_readingAgeMax}) {
        connect(combo, &QComboBox::currentTextChanged, this, &NovelSetupDialog::updateChecks);
    }
    updateChecks();
}

void NovelSetupDialog::addContributor(const QString &role, const QString &name)
{
    const int row = m_contributors->rowCount();
    m_contributors->insertRow(row);
    auto *roleCombo = new QComboBox(m_contributors);
    roleCombo->addItems(kContributorRoles);
    roleCombo->setCurrentText(role.isEmpty() ? kContributorRoles.first() : role);
    m_contributors->setCellWidget(row, 0, roleCombo);
    m_contributors->setItem(row, 1, new QTableWidgetItem(name));
    connect(roleCombo, &QComboBox::currentTextChanged, this, &NovelSetupDialog::updateChecks);
    connect(m_contributors, &QTableWidget::itemChanged, this, &NovelSetupDialog::updateChecks, Qt::UniqueConnection);
}

void NovelSetupDialog::load(const QString &defaultTitle)
{
    std::map<std::string, std::string> stored;
    try {
        stored = m_store.details();
    } catch (const std::exception &e) {
        QMessageBox::warning(this, windowTitle(), tr("Could not read the novel's details:\n\n%1").arg(e.what()));
    }
    const auto get = [&](const char *name) { return value(stored, name); };
    m_title->setText(get("title").isEmpty() ? defaultTitle : get("title"));
    m_subtitle->setText(get("subtitle"));
    m_seriesName->setText(get("series_name"));
    m_seriesNumber->setText(get("series_number"));
    m_edition->setText(get("edition"));
    m_language->setCurrentText(get("language"));
    m_author->setText(get("author"));
    for (const QString &line : get("contributors").split('\n', Qt::SkipEmptyParts)) {
        const qsizetype tab = line.indexOf('\t');
        addContributor(tab < 0 ? QString() : line.left(tab), tab < 0 ? line : line.mid(tab + 1));
    }
    m_description->setPlainText(get("description"));
    const QStringList keywords = get("keywords").split('\n', Qt::SkipEmptyParts);
    for (int i = 0; i < kKeywordCount && i < keywords.size(); ++i) {
        m_keywords[i]->setText(keywords[i]);
    }
    const QStringList categories = get("categories").split('\n', Qt::SkipEmptyParts);
    for (int i = 0; i < kCategoryCount && i < categories.size(); ++i) {
        m_categories[i]->setText(categories[i]);
    }
    m_publisher->setText(get("publisher"));
    m_isbn->setText(get("isbn"));
    m_rightsOwn->setChecked(get("rights") == "own");
    m_rightsPublicDomain->setChecked(get("rights") == "public_domain");
    m_explicitNo->setChecked(get("explicit") == "no");
    m_explicitYes->setChecked(get("explicit") == "yes");
    m_readingAgeMin->setCurrentIndex(std::max(0, m_readingAgeMin->findData(get("reading_age_min"))));
    m_readingAgeMax->setCurrentIndex(std::max(0, m_readingAgeMax->findData(get("reading_age_max"))));
}

std::map<std::string, std::string> NovelSetupDialog::values() const
{
    const auto lines = [](const QList<QLineEdit *> &edits) {
        QStringList out;
        for (const QLineEdit *edit : edits) {
            if (!edit->text().trimmed().isEmpty()) {
                out << edit->text().trimmed();
            }
        }
        return out.join('\n');
    };
    QStringList contributors;
    for (int row = 0; row < m_contributors->rowCount(); ++row) {
        const auto *role = qobject_cast<const QComboBox *>(m_contributors->cellWidget(row, 0));
        const QTableWidgetItem *name = m_contributors->item(row, 1);
        if (name && !name->text().trimmed().isEmpty()) {
            contributors << (role ? role->currentText() : QString()) + '\t' + name->text().trimmed();
        }
    }
    const QString rights = m_rightsOwn->isChecked() ? "own" : m_rightsPublicDomain->isChecked() ? "public_domain" : "";
    const QString explicitContent = m_explicitNo->isChecked() ? "no" : m_explicitYes->isChecked() ? "yes" : "";
    return {
        {"title", s(m_title->text())},
        {"subtitle", s(m_subtitle->text())},
        {"series_name", s(m_seriesName->text())},
        {"series_number", s(m_seriesNumber->text())},
        {"edition", s(m_edition->text())},
        {"language", s(m_language->currentText())},
        {"author", s(m_author->text())},
        {"contributors", s(contributors.join('\n'))},
        {"description", m_description->toPlainText().trimmed().toStdString()},
        {"keywords", s(lines(m_keywords))},
        {"categories", s(lines(m_categories))},
        {"publisher", s(m_publisher->text())},
        {"isbn", s(m_isbn->text())},
        {"rights", s(rights)},
        {"explicit", s(explicitContent)},
        {"reading_age_min", s(m_readingAgeMin->currentData().toString())},
        {"reading_age_max", s(m_readingAgeMax->currentData().toString())},
    };
}

QStringList NovelSetupDialog::problems(const std::map<std::string, std::string> &values)
{
    const auto get = [&](const char *name) { return value(values, name); };
    QStringList out;

    QStringList missing;
    if (get("title").isEmpty()) {
        missing << tr("title");
    }
    if (get("author").isEmpty()) {
        missing << tr("author");
    }
    if (get("language").isEmpty()) {
        missing << tr("language");
    }
    if (get("rights").isEmpty()) {
        missing << tr("publishing rights");
    }
    if (get("explicit").isEmpty()) {
        missing << tr("the answer about sexually explicit content");
    }
    if (get("categories").isEmpty()) {
        missing << tr("a category");
    }
    if (!missing.isEmpty()) {
        out << tr("Needed to publish on KDP: %1.").arg(missing.join(QStringLiteral(", ")));
    }

    const qsizetype titleLength = get("title").size() + get("subtitle").size();
    if (titleLength >= kTitleAndSubtitleLimit) {
        out << tr("Title and subtitle have %1 characters together; KDP allows fewer than %2.")
                   .arg(titleLength)
                   .arg(kTitleAndSubtitleLimit);
    }
    if (const qsizetype length = get("description").size(); length > kDescriptionLimit) {
        out << tr("The description has %1 characters; KDP allows up to %2.").arg(length).arg(kDescriptionLimit);
    }
    static const QRegularExpression digits(QStringLiteral("^[0-9]+$"));
    if (!get("series_number").isEmpty() && !digits.match(get("series_number")).hasMatch()) {
        out << tr("The series number must be digits only, e.g. 3 (not \"Book 3\").");
    }
    if (!get("series_number").isEmpty() && get("series_name").isEmpty()) {
        out << tr("A series number needs the series' name.");
    }
    if (!get("edition").isEmpty() && !digits.match(get("edition")).hasMatch()) {
        out << tr("The edition must be a number, e.g. 2.");
    }
    static const QRegularExpression html(QStringLiteral("<[a-zA-Z/!][^>]*>"));
    for (const char *name : {"title", "subtitle", "author", "contributors", "keywords"}) {
        if (html.match(get(name)).hasMatch()) {
            out << tr("KDP doesn't accept HTML in titles, names or keywords.");
            break;
        }
    }
    if (get("rights") == "public_domain" && get("contributors").isEmpty()) {
        out << tr("For a public domain work, KDP needs all of its contributors.");
    }
    const QString minAge = get("reading_age_min"), maxAge = get("reading_age_max");
    if (!minAge.isEmpty() && !maxAge.isEmpty() && minAge.toInt() > QString(maxAge).remove('+').toInt()) {
        out << tr("The youngest reading age is above the oldest.");
    }
    if (const QString isbn = QString(get("isbn")).remove('-').remove(' '); !isbn.isEmpty()) {
        static const QRegularExpression isbnPattern(QStringLiteral("^([0-9]{13}|[0-9]{9}[0-9Xx])$"));
        if (!isbnPattern.match(isbn).hasMatch()) {
            out << tr("An ISBN has 13 digits (or 10, the last of which may be X).");
        }
    }
    return out;
}

void NovelSetupDialog::updateChecks()
{
    const auto current = values();
    const qsizetype length = m_description->toPlainText().trimmed().size();
    m_descriptionCount->setText(tr("%1 / %2 characters").arg(length).arg(kDescriptionLimit));
    const QStringList found = problems(current);
    m_checks->setText(found.isEmpty() ? tr("Everything KDP asks for is filled in.") : found.join('\n'));
}

void NovelSetupDialog::accept()
{
    try {
        m_store.setDetails(values());
    } catch (const std::exception &e) {
        QMessageBox::critical(this, windowTitle(), tr("Could not save the novel's details:\n\n%1").arg(e.what()));
        return;
    }
    QDialog::accept();
}
