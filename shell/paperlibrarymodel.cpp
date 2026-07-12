/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "paperlibrarymodel.h"
#include "telemetry.h"
#include "readingprogress.h"

#include <KConfigGroup>
#include <KSharedConfig>

#include <QAtomicInt>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>

#include <algorithm>
#include <utility>

static QString catalogPath(const QString &corpusDir)
{
    return corpusDir + QStringLiteral("/catalog.jsonl");
}

static QString paperLibraryConfigFilePath()
{
    const QString overridePath = qEnvironmentVariable("PAPERLIBRARY_CONFIG_PATH");
    const QString configDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    return !overridePath.isEmpty() ? overridePath : (configDir.isEmpty() ? QStringLiteral("paperlibraryrc") : configDir + QLatin1String("/paperlibraryrc"));
}

static KConfigGroup paperLibraryConfigGroupForPath(const QString &configFilePath, const QString &name)
{
    return KSharedConfig::openConfig(configFilePath, KConfig::SimpleConfig)->group(name);
}

static KConfigGroup paperLibraryConfigGroup(const QString &name)
{
    return paperLibraryConfigGroupForPath(paperLibraryConfigFilePath(), name);
}

static const char DOWNRANKED_SLUGS_KEY[] = "DownrankedSlugs";
static const char FINISHED_SLUGS_KEY[] = "FinishedSlugs";
static constexpr int InitialCorpusRows = 360;
static constexpr int CorpusFetchBatchRows = 240;

static QString curatedDisplayTitleFor(const QString &text)
{
    const QString lower = text.toCaseFolded();
    const QString simplified = lower.simplified();
    const bool bookish = lower.contains(QLatin1String("book:")) || lower.contains(QLatin1String("aa_book")) || lower.contains(QLatin1String("(book)"))
        || lower.contains(QLatin1String("anna")) || lower.contains(QLatin1String("imported-books")) || lower.contains(QLatin1String(".epub"))
        || lower.contains(QLatin1String(".azw3")) || lower.contains(QLatin1String(".mobi")) || lower.contains(QLatin1String("bantam books"))
        || lower.contains(QLatin1String("penguin")) || lower.contains(QLatin1String("melville")) || lower.contains(QLatin1String("farrar"))
        || lower.contains(QLatin1String("mit press")) || lower.contains(QLatin1String("pm press"));
    if (lower.contains(QLatin1String("1941")) && lower.contains(QLatin1String("america that went to war"))) {
        return QStringLiteral("1941: The America That Went to War");
    }
    if (lower.contains(QLatin1String("1941")) && lower.contains(QLatin1String("william m. christie"))) {
        return QStringLiteral("1941: The America That Went to War");
    }
    if (lower.contains(QLatin1String("aldo leopold")) && lower.contains(QLatin1String("sand county almanac"))) {
        return QStringLiteral("A Sand County Almanac & Other Writings on Ecology and Conservation");
    }
    if (lower.contains(QLatin1String("ref13 graeber 2011"))) {
        return QStringLiteral("Debt: The First 5,000 Years");
    }
    if (lower.contains(QLatin1String("parable of the sower")) || (lower.contains(QLatin1String("octavia butler")) && lower.contains(QLatin1String("parable")))) {
        return QStringLiteral("Parable of the Sower");
    }
    if (lower.contains(QLatin1String("red mars")) && (bookish || lower.contains(QLatin1String("kim stanley robinson")) || simplified.startsWith(QLatin1String("red mars")))) {
        return QStringLiteral("Red Mars");
    }
    if (lower.contains(QLatin1String("new york 2140")) && (bookish || lower.contains(QLatin1String("kim stanley robinson")) || simplified.startsWith(QLatin1String("new york 2140")))) {
        return QStringLiteral("New York 2140");
    }
    if (lower.contains(QLatin1String("years of rice and salt")) && (bookish || lower.contains(QLatin1String("kim stanley robinson")))) {
        return QStringLiteral("The Years of Rice and Salt");
    }
    if (lower.contains(QLatin1String("green mars")) && (bookish || lower.contains(QLatin1String("kim stanley robinson")) || simplified.startsWith(QLatin1String("green mars")))) {
        return QStringLiteral("Green Mars");
    }
    if (lower.contains(QLatin1String("blue mars")) && (bookish || lower.contains(QLatin1String("kim stanley robinson")) || simplified.startsWith(QLatin1String("blue mars")))) {
        return QStringLiteral("Blue Mars");
    }
    if (lower.contains(QLatin1String("antarctica")) && (lower.contains(QLatin1String("novel")) || lower.contains(QLatin1String("kim stanley robinson")))) {
        return QStringLiteral("Antarctica");
    }
    if (lower.contains(QLatin1String("best of kim stanley robinson"))) {
        return QStringLiteral("The Best of Kim Stanley Robinson");
    }
    if (lower.contains(QLatin1String("game of thrones")) && (bookish || simplified.startsWith(QLatin1String("a game of thrones")))) {
        return QStringLiteral("A Game of Thrones");
    }
    if (lower.contains(QLatin1String("one day in the life of ivan denisovich"))) {
        return QStringLiteral("One Day in the Life of Ivan Denisovich");
    }
    if (lower.contains(QLatin1String("ones who walk away from omelas"))) {
        return QStringLiteral("The Ones Who Walk Away from Omelas");
    }
    if (lower.contains(QLatin1String("dune, dune messiah")) || (lower.contains(QLatin1String("dune messiah")) && lower.contains(QLatin1String("children of dune")))) {
        return QStringLiteral("Dune / Dune Messiah / Children of Dune");
    }
    if (lower.contains(QLatin1String("water knife"))) {
        return QStringLiteral("The Water Knife");
    }
    if (lower.contains(QLatin1String("windup girl"))) {
        return QStringLiteral("The Windup Girl");
    }
    if (lower.contains(QLatin1String("carpentaria"))) {
        return QStringLiteral("Carpentaria");
    }
    if (lower.contains(QLatin1String("books of earthsea"))) {
        return QStringLiteral("The Books of Earthsea");
    }
    if ((lower.contains(QLatin1String("everything was forever until it was no more")) || lower.contains(QLatin1String("everything was forever, until it was no more")))
        && (bookish || simplified.startsWith(QLatin1String("everything was forever")))) {
        return QStringLiteral("Everything Was Forever Until It Was No More");
    }
    if (lower.contains(QLatin1String("dawn of everything")) && (bookish || simplified.startsWith(QLatin1String("the dawn of everything")))) {
        return QStringLiteral("The Dawn of Everything");
    }
    if (lower.contains(QLatin1String("bullshit jobs")) && (bookish || simplified.startsWith(QLatin1String("bullshit jobs")))) {
        return QStringLiteral("Bullshit Jobs");
    }
    if ((lower.contains(QLatin1String("debt the first 5000 years")) || lower.contains(QLatin1String("debt the first 5,000 years")) || simplified.startsWith(QLatin1String("debt graeber")))
        && bookish) {
        return QStringLiteral("Debt: The First 5,000 Years");
    }
    if (lower.contains(QLatin1String("cities made differently")) && (bookish || simplified.startsWith(QLatin1String("cities made differently")))) {
        return QStringLiteral("Cities Made Differently");
    }
    if (lower.contains(QLatin1String("utopia of rules")) && (bookish || simplified.startsWith(QLatin1String("the utopia of rules")))) {
        return QStringLiteral("The Utopia of Rules");
    }
    if (lower.contains(QLatin1String("pirate enlightenment")) && (bookish || simplified.startsWith(QLatin1String("pirate enlightenment")))) {
        return QStringLiteral("Pirate Enlightenment");
    }
    if (lower.contains(QLatin1String("path to power")) && (bookish || simplified.startsWith(QLatin1String("the path to power")))) {
        return QStringLiteral("The Path to Power");
    }
    if (lower.contains(QLatin1String("means of ascent")) && (bookish || simplified.startsWith(QLatin1String("means of ascent")) || simplified.startsWith(QLatin1String("the years of lyndon johnson means of ascent")))) {
        return QStringLiteral("Means of Ascent");
    }
    if (lower.contains(QLatin1String("master of the senate")) && (bookish || simplified.startsWith(QLatin1String("master of the senate")))) {
        return QStringLiteral("Master of the Senate");
    }
    if (lower.contains(QLatin1String("passage of power")) && (bookish || simplified.startsWith(QLatin1String("the passage of power")) || simplified.startsWith(QLatin1String("the years of lyndon johnson 04")))) {
        return QStringLiteral("The Passage of Power");
    }
    if (lower.contains(QLatin1String("power broker")) && (bookish || simplified.startsWith(QLatin1String("the power broker")))) {
        return QStringLiteral("The Power Broker");
    }
    if (lower.contains(QLatin1String("working researching interviewing writing")) && (bookish || simplified.startsWith(QLatin1String("working")))) {
        return QStringLiteral("Working: Researching, Interviewing, Writing");
    }
    if (lower.contains(QLatin1String("why work")) && lower.contains(QLatin1String("leisure society")) && (bookish || simplified.startsWith(QLatin1String("why work")))) {
        return QStringLiteral("Why Work?");
    }
    if (lower.contains(QLatin1String("on kings")) && lower.contains(QLatin1String("graeber")) && (bookish || simplified.startsWith(QLatin1String("on kings")))) {
        return QStringLiteral("On Kings");
    }
    return QString();
}

static QString cleanedDisplayTitle(QString title, const QString &path = QString(), const QString &context = QString())
{
    const QString pathBase = QFileInfo(path).completeBaseName();
    const QString curated = curatedDisplayTitleFor(QStringList({title, pathBase, context}).join(QLatin1Char(' ')));
    if (!curated.isEmpty()) {
        return curated;
    }

    if (title.trimmed().isEmpty()) {
        title = pathBase;
    }
    title.replace(QLatin1Char('_'), QLatin1Char(' '));
    title.replace(QRegularExpression(QStringLiteral("\\bAnna[’']s Archive\\b"), QRegularExpression::CaseInsensitiveOption), QString());
    title.remove(QRegularExpression(QStringLiteral("\\b[0-9a-fA-F]{32}\\b")));
    title.remove(QRegularExpression(QStringLiteral("\\b97[89][0-9]{10}\\b")));
    title.remove(QRegularExpression(QStringLiteral("\\s+[0-9a-fA-F]{8}$")));
    title.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    title = title.simplified();

    const QString curatedAfterCleanup = curatedDisplayTitleFor(title);
    return curatedAfterCleanup.isEmpty() ? title : curatedAfterCleanup;
}

static QString normalizedDuplicateWorkText(QString text)
{
    text = text.toCaseFolded();
    text.replace(QRegularExpression(QStringLiteral("[’‘`´]")), QStringLiteral("'"));
    text.replace(QLatin1Char('&'), QStringLiteral(" and "));
    text.replace(QRegularExpression(QStringLiteral("\\b5\\s*,?\\s*000\\b")), QStringLiteral("5000"));
    text.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral(" "));
    return text.simplified();
}

// An imported book's title is often a raw filename dump — a bare paren year, a download-site stamp
// ("( PDFDrive )"), a "final 2024"/"retail"/"_ocr" marker. When present, prefer the clean librarian
// title over the imported one. Curated feed titles (no such markers) are left untouched.
static bool titleHasImportCruft(const QString &title)
{
    static const QRegularExpression cruft(
        QStringLiteral("\\((?:19|20)\\d\\d\\)|\\bpdf ?drive\\b|\\bz-?lib(?:gen)?\\b|allitebooks|\\bfinal\\s+20\\d\\d\\b|\\bretail\\b|_ocr\\b"),
        QRegularExpression::CaseInsensitiveOption);
    return cruft.match(title).hasMatch();
}

static QString canonicalDuplicateWorkKey(const QString &title, const QString &authors = QString(), const QString &context = QString())
{
    const QString titleKey = normalizedDuplicateWorkText(title);
    const QString authorKey = normalizedDuplicateWorkText(authors);
    const QString contextKey = normalizedDuplicateWorkText(context);
    const QString haystack = QStringList({titleKey, authorKey, contextKey}).join(QLatin1Char(' '));

    if (titleKey.contains(QStringLiteral("dawn of everything"))
        && (titleKey.startsWith(QStringLiteral("the dawn of everything")) || haystack.contains(QStringLiteral("david graeber")) || haystack.contains(QStringLiteral("wengrow")))) {
        return QStringLiteral("work|dawn-of-everything|graeber-wengrow");
    }
    if (titleKey == QLatin1String("ref13 graeber 2011") || contextKey.contains(QStringLiteral("ref13 graeber 2011"))
        || (titleKey.contains(QStringLiteral("debt")) && haystack.contains(QStringLiteral("graeber"))
            && (titleKey.contains(QStringLiteral("5000")) || titleKey.contains(QStringLiteral("first"))))) {
        return QStringLiteral("work|debt-first-5000-years|graeber");
    }
    if (titleKey.contains(QStringLiteral("bullshit jobs")) && haystack.contains(QStringLiteral("graeber"))) {
        return QStringLiteral("work|bullshit-jobs|graeber");
    }
    if (titleKey.contains(QStringLiteral("pirate enlightenment")) && haystack.contains(QStringLiteral("graeber"))) {
        return QStringLiteral("work|pirate-enlightenment|graeber");
    }
    if (titleKey.contains(QStringLiteral("utopia of rules")) && haystack.contains(QStringLiteral("graeber"))) {
        return QStringLiteral("work|utopia-of-rules|graeber");
    }

    if (titleKey.isEmpty()) {
        return QString();
    }
    // Title alone must be distinctive — short titles collide across unrelated works, so
    // keep the conservative floor there. But a title PLUS a known author is strong enough
    // to treat as the same work even when the title is short ("Siddhartha" + "Hermann
    // Hesse"), which is what lets duplicate editions with one missing author collapse.
    if (authorKey.isEmpty()) {
        return titleKey.size() < 12 ? QString() : QStringLiteral("title|%1").arg(titleKey);
    }
    if (titleKey.size() < 4) {
        return QString();
    }
    return QStringLiteral("title|%1|author|%2").arg(titleKey, authorKey.left(60).trimmed());
}

struct ImportedBookMetadata {
    QString title;
    QString authors;
    QString year;
};

static bool isBookLikeMetadata(const QString &source, const QString &journal, const QString &title)
{
    const QString lower = QStringList({source, journal, title}).join(QLatin1Char(' ')).toCaseFolded();
    return source.startsWith(QLatin1String("book:")) || source == QLatin1String("aa_book") || journal == QLatin1String("(book)") || lower.contains(QLatin1String("anna"))
        || lower.contains(QLatin1String("isbn")) || lower.contains(QLatin1String("libgen")) || lower.contains(QLatin1String("pdfdrive"));
}

static QString cleanImportedBookSegment(QString text)
{
    text.replace(QChar(0x2028), QLatin1Char(' '));
    text.replace(QLatin1Char('_'), QLatin1Char(' '));
    text.replace(QRegularExpression(QStringLiteral("\\bAnna[’']s Archive\\b"), QRegularExpression::CaseInsensitiveOption), QString());
    text.replace(QRegularExpression(QStringLiteral("\\[\\s*TRUE\\s+PDF\\s*\\]"), QRegularExpression::CaseInsensitiveOption), QString());
    text.replace(QRegularExpression(QStringLiteral("\\(\\s*PDF\\s*Drive\\s*\\)"), QRegularExpression::CaseInsensitiveOption), QString());
    text.replace(QRegularExpression(QStringLiteral("\\blibgen\\.[a-z]+\\b"), QRegularExpression::CaseInsensitiveOption), QString());
    text.remove(QRegularExpression(QStringLiteral("\\b[0-9a-fA-F]{32}\\b")));
    text.remove(QRegularExpression(QStringLiteral("\\bisbn(?:13)?\\s*97[89][0-9]{10}\\b"), QRegularExpression::CaseInsensitiveOption));
    text.remove(QRegularExpression(QStringLiteral("\\b97[89][0-9]{10}\\b")));
    text.remove(QRegularExpression(QStringLiteral("^\\[[^\\]]+\\]\\s*")));
    text.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    return text.simplified();
}

static QString strippedPublisherTail(QString text)
{
    static const QRegularExpression yearParen(QStringLiteral("\\s*\\([^)]*(?:19|20)\\d{2}[^)]*\\)\\s*$"));
    text.remove(yearParen);
    text.remove(QRegularExpression(QStringLiteral("\\s*\\(Tier\\s+\\d+\\)\\s*$"), QRegularExpression::CaseInsensitiveOption));

    static const QRegularExpression publisherTail(
        QStringLiteral("\\s+(?:Oxford University Press|Princeton University Press|Cambridge University Press|Yale University Press|State University of New York Press|University of Nebraska Press|"
                       "MIT Press|The MIT Press|Basic Books|Penguin(?: Random House)?(?: LLC)?|Penguin Press|PublicAffairs|Belknap Press|Melville House|PM Press|"
                       "Farrar, Straus and Giroux|Knopf Doubleday Publishing Group|McGraw Hill|Elsevier(?:\\s+Health)?|Lippincott Williams & Wilkins|Garland Science|"
                       "Harper Perennial|Pantheon Books|Plume|Beresta Books|CSIRO Publishing|Sinauer Associates|The Mountaineers|National Outdoor Leadership School|"
                       "Springer(?: International Publishing)?|Pearson(?: Education(?:, Inc\\.)?)?|W\\. W\\. Norton(?: & Company)?|Wiley|Routledge(?:\\s+CRC)?|CRC Press|"
                       "Random House|St\\. Martin's Press|Stanford University Press|University of Chicago Press|World Scientific|BenBella Books, Inc\\.|City Lights Publishers|"
                       "Lightning Source Inc\\.)(?:,?\\s*[^()]{0,80})?$"),
        QRegularExpression::CaseInsensitiveOption);
    text.remove(publisherTail);
    text.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    return text.simplified();
}

static bool looksLikePersonList(const QString &segment)
{
    const QString text = segment.simplified();
    if (text.isEmpty() || text.size() > 120 || text.contains(QLatin1Char(':'))) {
        return false;
    }
    const QString lower = text.toCaseFolded();
    if (lower.startsWith(QLatin1Char('('))) {
        return false;
    }
    if (lower.startsWith(QLatin1String("the ")) || lower.startsWith(QLatin1String("a ")) || lower.startsWith(QLatin1String("an ")) || lower.startsWith(QLatin1String("first "))
        || lower.startsWith(QLatin1String("textbook ")) || lower.startsWith(QLatin1String("medicine ")) || lower.startsWith(QLatin1String("context "))
        || lower.startsWith(QLatin1String("pirate ")) || lower.startsWith(QLatin1String("battle ")) || lower.startsWith(QLatin1String("patterns "))
        || lower.startsWith(QLatin1String("color ")) || lower.startsWith(QLatin1String("consciousness ")) || lower.startsWith(QLatin1String("pocus "))
        || lower.startsWith(QLatin1String("usyd "))) {
        return false;
    }
    if (text.contains(QRegularExpression(QStringLiteral("\\d")))) {
        return false;
    }
    if (lower.contains(QRegularExpression(QStringLiteral("\\b(the|of|and|in|from|with|without|to|for|or)\\b")))) {
        return false;
    }
    const QStringList titleWords = {QStringLiteral("biomechanical"),
                                    QStringLiteral("blackshirts"),
                                    QStringLiteral("capital"),
                                    QStringLiteral("cooperate"),
                                    QStringLiteral("cybernetics"),
                                    QStringLiteral("debt"),
                                    QStringLiteral("democracy"),
                                    QStringLiteral("explaining"),
                                    QStringLiteral("falling"),
                                    QStringLiteral("fifth"),
                                    QStringLiteral("foundations"),
                                    QStringLiteral("genesis"),
                                    QStringLiteral("goodness"),
                                    QStringLiteral("i"),
                                    QStringLiteral("am"),
                                    QStringLiteral("jaws"),
                                    QStringLiteral("learning"),
                                    QStringLiteral("logic"),
                                    QStringLiteral("managing"),
                                    QStringLiteral("money"),
                                    QStringLiteral("moral"),
                                    QStringLiteral("origins"),
                                    QStringLiteral("point"),
                                    QStringLiteral("scientific"),
                                    QStringLiteral("skeletal"),
                                    QStringLiteral("social"),
                                    QStringLiteral("survival"),
                                    QStringLiteral("systems"),
                                    QStringLiteral("ultrasociety"),
                                    QStringLiteral("variation"),
                                    QStringLiteral("war"),
                                    QStringLiteral("what"),
                                    QStringLiteral("why")};
    for (const QString &word : titleWords) {
        if (lower.contains(QRegularExpression(QStringLiteral("\\b%1\\b").arg(QRegularExpression::escape(word))))) {
            return false;
        }
    }
    const QStringList titleNeedles = {QStringLiteral(" edition"),
                                      QStringLiteral(" press"),
                                      QStringLiteral(" university"),
                                      QStringLiteral(" science"),
                                      QStringLiteral(" medicine"),
                                      QStringLiteral(" biology"),
                                      QStringLiteral(" anthropology"),
                                      QStringLiteral(" physics"),
                                      QStringLiteral(" anatomy"),
                                      QStringLiteral(" physiology"),
                                      QStringLiteral(" pathology"),
                                      QStringLiteral(" history"),
                                      QStringLiteral(" culture"),
                                      QStringLiteral(" communication"),
                                      QStringLiteral(" fundamentals"),
                                      QStringLiteral(" principles"),
                                      QStringLiteral(" handbook"),
                                      QStringLiteral(" atlas"),
                                      QStringLiteral(" textbook"),
                                      QStringLiteral(" systems"),
                                      QStringLiteral(" strategy"),
                                      QStringLiteral(" options"),
                                      QStringLiteral(" guide"),
                                      QStringLiteral(" novel")};
    for (const QString &needle : titleNeedles) {
        if (lower.contains(needle)) {
            return false;
        }
    }
    const QStringList words = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (words.size() < 2 || words.size() > 18) {
        return false;
    }
    if (lower.endsWith(QLatin1String(" md")) && words.size() <= 8) {
        return true;
    }
    if (text.contains(QLatin1Char(';')) || text.contains(QLatin1Char(','))) {
        const QString lastCommaSegment = text.section(QLatin1Char(','), -1).simplified();
        if (lastCommaSegment.split(QLatin1Char(' '), Qt::SkipEmptyParts).size() < 2) {
            return false;
        }
        return true;
    }
    int capitalish = 0;
    for (const QString &word : words) {
        if (!word.isEmpty() && (word.at(0).isUpper() || word.at(0) == QLatin1Char('&'))) {
            ++capitalish;
        }
    }
    return capitalish >= words.size() - 1;
}

static QString firstYearIn(const QString &text)
{
    const QRegularExpressionMatch match = QRegularExpression(QStringLiteral("\\b(19|20)\\d{2}\\b")).match(text);
    return match.hasMatch() ? match.captured(0) : QString();
}

static bool isBookTitleStarter(const QString &word)
{
    QString normalized = word.toCaseFolded();
    normalized.remove(QRegularExpression(QStringLiteral("^[^a-z0-9]+|[^a-z0-9]+$")));
    static const QSet<QString> titleStarters = {QStringLiteral("a"),
                                                QStringLiteral("an"),
                                                QStringLiteral("the"),
                                                QStringLiteral("biological"),
                                                QStringLiteral("blackshirts"),
                                                QStringLiteral("capital"),
                                                QStringLiteral("competitive"),
                                                QStringLiteral("cybernetics"),
                                                QStringLiteral("debt"),
                                                QStringLiteral("democracy"),
                                                QStringLiteral("elements"),
                                                QStringLiteral("end"),
                                                QStringLiteral("everything"),
                                                QStringLiteral("falling"),
                                                QStringLiteral("free"),
                                                QStringLiteral("fundamentals"),
                                                QStringLiteral("handbook"),
                                                QStringLiteral("historical"),
                                                QStringLiteral("i"),
                                                QStringLiteral("introduction"),
                                                QStringLiteral("invention"),
                                                QStringLiteral("jaws"),
                                                QStringLiteral("lawlessness"),
                                                QStringLiteral("learning"),
                                                QStringLiteral("logic"),
                                                QStringLiteral("money"),
                                                QStringLiteral("moral"),
                                                QStringLiteral("neurocircuitry"),
                                                QStringLiteral("paleolithic"),
                                                QStringLiteral("pirate"),
                                                QStringLiteral("principles"),
                                                QStringLiteral("rediscovery"),
                                                QStringLiteral("scientific"),
                                                QStringLiteral("social"),
                                                QStringLiteral("systems"),
                                                QStringLiteral("theory"),
                                                QStringLiteral("ultrasociety"),
                                                QStringLiteral("war"),
                                                QStringLiteral("working"),
                                                QStringLiteral("you")};
    return titleStarters.contains(normalized);
}

static bool looksLikeBookTitleRemainder(const QString &segment)
{
    const QString text = segment.simplified();
    if (text.size() < 8) {
        return false;
    }
    const QString lower = text.toCaseFolded();
    const QString first = text.section(QLatin1Char(' '), 0, 0).toCaseFolded();
    if (isBookTitleStarter(first)) {
        return true;
    }
    if (looksLikePersonList(text.left(80))) {
        return false;
    }
    return lower.contains(QLatin1String(" of ")) || lower.contains(QLatin1String(" and ")) || lower.contains(QLatin1String(" in ")) || lower.contains(QLatin1String(" from "))
        || lower.contains(QLatin1String(" with ")) || lower.contains(QLatin1String(" without ")) || lower.contains(QLatin1String(" to "));
}

static bool splitLeadingAuthorTitle(const QString &text, QString *title, QString *authors)
{
    const QString cleaned = strippedPublisherTail(text);
    if (cleaned.startsWith(QLatin1Char('('))) {
        return false;
    }
    const QStringList words = cleaned.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (words.size() < 5 || words.size() > 34) {
        return false;
    }

    int bestCut = -1;
    int bestScore = -1;
    const int maxCut = std::min(12, static_cast<int>(words.size()) - 2);
    for (int cut = 2; cut <= maxCut; ++cut) {
        const QString leading = words.mid(0, cut).join(QLatin1Char(' '));
        const QString remainder = words.mid(cut).join(QLatin1Char(' '));
        if (!looksLikePersonList(leading) || !looksLikeBookTitleRemainder(remainder)) {
            continue;
        }

        int score = 0;
        const QString firstRemainderWord = remainder.section(QLatin1Char(' '), 0, 0).toCaseFolded();
        const bool firstIsTitleStarter = isBookTitleStarter(firstRemainderWord);
        if (!firstIsTitleStarter) {
            continue;
        }
        if (firstIsTitleStarter) {
            score += 3;
        }
        if (firstRemainderWord == QLatin1String("the") || firstRemainderWord == QLatin1String("a") || firstRemainderWord == QLatin1String("an")) {
            score += 4;
        }
        if (remainder.contains(QRegularExpression(QStringLiteral("\\b(of|and|in|from|with|without|to)\\b"), QRegularExpression::CaseInsensitiveOption))) {
            score += 2;
        }
        if (leading.contains(QLatin1Char(',')) || leading.contains(QLatin1Char(';'))) {
            score += 2;
        }
        score += leading.count(QRegularExpression(QStringLiteral("\\b[A-Z]\\.\\b")));
        if (cut >= 3 && words.value(cut - 2).endsWith(QLatin1Char('.'))) {
            score += 1;
        }
        bool shouldReplace = score > bestScore;
        if (!shouldReplace && score == bestScore && bestCut >= 0) {
            const QString bestFirstRemainderWord = words.mid(bestCut).join(QLatin1Char(' ')).section(QLatin1Char(' '), 0, 0).toCaseFolded();
            const bool bestFirstIsTitleStarter = isBookTitleStarter(bestFirstRemainderWord);
            if (firstIsTitleStarter != bestFirstIsTitleStarter) {
                shouldReplace = firstIsTitleStarter;
            } else if (firstIsTitleStarter) {
                shouldReplace = cut < bestCut;
            } else {
                shouldReplace = cut > bestCut && !remainder.section(QLatin1Char(' '), 0, 0).contains(QLatin1Char('.'));
            }
        }
        if (shouldReplace) {
            bestScore = score;
            bestCut = cut;
        }
    }

    if (bestCut < 0 || bestScore < 2) {
        return false;
    }
    *authors = words.mid(0, bestCut).join(QLatin1Char(' ')).simplified();
    *title = words.mid(bestCut).join(QLatin1Char(' ')).simplified();
    return !title->isEmpty() && !authors->isEmpty();
}

static ImportedBookMetadata importedBookMetadataFromTitle(const QString &rawTitle, const QString &authors, const QString &year, const QString &source, const QString &journal)
{
    ImportedBookMetadata metadata;
    if (!isBookLikeMetadata(source, journal, rawTitle)) {
        metadata.title = cleanedDisplayTitle(rawTitle, QString(), QStringList({authors, year, journal, source}).join(QLatin1Char(' ')));
        metadata.authors = authors;
        metadata.year = year;
        return metadata;
    }

    const QString sourceScrubbed = cleanImportedBookSegment(rawTitle);
    QStringList parts;
    for (const QString &part : rawTitle.split(QRegularExpression(QStringLiteral("\\s{3,}")), Qt::SkipEmptyParts)) {
        const QString cleaned = cleanImportedBookSegment(part);
        if (!cleaned.isEmpty()) {
            parts.append(cleaned);
        }
    }

    QString inferredTitle;
    QString inferredAuthors;
    if (parts.size() >= 2) {
        const QString first = parts.value(0);
        const QString second = parts.value(1);
        const QString third = parts.value(2);
        if (parts.size() >= 3 && !third.contains(QRegularExpression(QStringLiteral("\\d"))) && looksLikePersonList(third) && !looksLikePersonList(second)) {
            inferredTitle = QStringList({first, second}).join(QStringLiteral(": "));
            inferredAuthors = third;
        } else if (first.contains(QRegularExpression(QStringLiteral("^\\d+[a-z]?$"), QRegularExpression::CaseInsensitiveOption))) {
            inferredTitle = second;
        } else if (looksLikePersonList(second)) {
            inferredTitle = first;
            inferredAuthors = second;
        } else if (looksLikePersonList(first)) {
            inferredTitle = second;
            inferredAuthors = first;
        } else if (second.contains(QRegularExpression(QStringLiteral("\\b(19|20)\\d{2}\\b"))) && !looksLikePersonList(second)) {
            inferredTitle = first;
        } else {
            inferredTitle = first;
        }
    }

    if (inferredTitle.isEmpty()) {
        inferredTitle = sourceScrubbed;
    }

    const bool hasStructuredImportTail = !firstYearIn(rawTitle).isEmpty() || sourceScrubbed != strippedPublisherTail(sourceScrubbed)
        || rawTitle.contains(QRegularExpression(QStringLiteral("\\b(Anna[’']s Archive|isbn|libgen\\.|PDF\\s*Drive)\\b"), QRegularExpression::CaseInsensitiveOption));
    if (hasStructuredImportTail && inferredAuthors.isEmpty() && authors.isEmpty()) {
        QString leadingTitle;
        QString leadingAuthors;
        if (splitLeadingAuthorTitle(sourceScrubbed, &leadingTitle, &leadingAuthors)) {
            inferredTitle = leadingTitle;
            inferredAuthors = leadingAuthors;
        }
    }

    inferredTitle = strippedPublisherTail(inferredTitle);
    metadata.title = cleanedDisplayTitle(inferredTitle, QString(), QStringList({authors, year, journal, source}).join(QLatin1Char(' ')));
    metadata.authors = authors.isEmpty() ? cleanImportedBookSegment(inferredAuthors) : authors.simplified();
    metadata.year = year.isEmpty() ? firstYearIn(rawTitle) : year.simplified();
    return metadata;
}

static QString derivedPdfPath(const QString &corpusDir, const QString &slug)
{
    return corpusDir + QStringLiteral("/pdfs/") + slug + QStringLiteral(".pdf");
}

enum class ReadOnlyDatabaseMode {
    LiveWalAware,
    ImmutableSnapshot,
};

// catalog.db is a live backend database and commonly uses WAL. immutable=1 tells SQLite that the
// file will never change and therefore makes it ignore an uncheckpointed -wal file. Derived
// search.db/graph.db files are built under temporary names and atomically published, so immutable
// reads remain safe and avoid taking locks on those large static indexes.
static ReadOnlyDatabaseMode readOnlyDatabaseMode(const QString &dbPath)
{
    return QFileInfo(dbPath).fileName().compare(QLatin1String("catalog.db"), Qt::CaseInsensitive) == 0
        ? ReadOnlyDatabaseMode::LiveWalAware
        : ReadOnlyDatabaseMode::ImmutableSnapshot;
}

// Percent-encoding keeps paths containing URI-significant characters (spaces, %, ?, #) valid.
static QString readOnlyDatabaseUri(const QString &dbPath, ReadOnlyDatabaseMode mode)
{
    QString uri = QUrl::fromLocalFile(dbPath).toString(QUrl::FullyEncoded) + QStringLiteral("?mode=ro");
    if (mode == ReadOnlyDatabaseMode::ImmutableSnapshot) {
        uri += QStringLiteral("&immutable=1");
    }
    return uri;
}

static bool openReadOnlyDatabase(QSqlDatabase &db, const QString &dbPath)
{
    const ReadOnlyDatabaseMode mode = readOnlyDatabaseMode(dbPath);
    db.setDatabaseName(readOnlyDatabaseUri(dbPath, mode));
    db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_OPEN_URI"));
    if (!db.open()) {
        return false;
    }
    if (mode == ReadOnlyDatabaseMode::LiveWalAware) {
        QSqlQuery pragma(db);
        if (!pragma.exec(QStringLiteral("PRAGMA busy_timeout=5000")) || !pragma.exec(QStringLiteral("PRAGMA query_only=ON"))) {
            db.close();
            return false;
        }
    }
    return true;
}

static QString nextPaperLibraryDbConnectionName(const QString &prefix)
{
    static QAtomicInt connectionCounter;
    return QStringLiteral("%1_%2").arg(prefix).arg(connectionCounter.fetchAndAddRelaxed(1));
}

static bool catalogDbHasTable(const QString &dbPath, const QString &tableName)
{
    if (!QFileInfo::exists(dbPath) || !QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
        return false;
    }

    const QString connectionName = nextPaperLibraryDbConnectionName(QStringLiteral("paperlibrary_probe"));
    bool found = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        if (openReadOnlyDatabase(db, dbPath)) {
            QSqlQuery query(db);
            query.prepare(QStringLiteral("SELECT 1 FROM sqlite_master WHERE type='table' AND name=? LIMIT 1"));
            query.addBindValue(tableName);
            found = query.exec() && query.next();
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connectionName);
    return found;
}

// Resolve which database currently holds a derived-index table. The backend keeps
// derived indexes OUT of the durable catalog.db so its snapshot stays small: the FTS
// index lives in search.db and the semantic graph in graph/graph.db. Prefer that
// dedicated file; fall back to catalog.db for older corpora that still embed the
// index. Returns the DB path holding @p table, or an empty string if unavailable.
static QString resolveIndexDb(const QString &corpusDir, const QString &dedicatedRelative, const QString &table)
{
    const QString dedicated = corpusDir + QLatin1Char('/') + dedicatedRelative;
    if (catalogDbHasTable(dedicated, table)) {
        return dedicated;
    }
    const QString catalog = corpusDir + QStringLiteral("/catalog.db");
    if (catalogDbHasTable(catalog, table)) {
        return catalog;
    }
    return QString();
}

static QString ftsMatchQuery(const QString &raw)
{
    QStringList tokens;
    QRegularExpression tokenExpression(QStringLiteral("[A-Za-z0-9]+"));
    QRegularExpressionMatchIterator matches = tokenExpression.globalMatch(raw);
    while (matches.hasNext()) {
        QString token = matches.next().captured(0).toLower();
        if (token.size() > 1) {
            tokens.append(token + QLatin1Char('*'));
        }
    }
    return tokens.join(QStringLiteral(" AND "));
}

static QString normalizedFocusLookupKey(const QString &value)
{
    return value.trimmed().toCaseFolded();
}

static QString normalizedFocusPathLookupKey(const QString &path)
{
    const QString trimmed = path.trimmed();
    return trimmed.isEmpty() ? QString() : QDir::cleanPath(trimmed).toCaseFolded();
}

static QStringList jsonStringList(const QJsonValue &value)
{
    QStringList result;
    if (!value.isArray()) {
        return result;
    }
    for (const QJsonValue &item : value.toArray()) {
        const QString text = item.toString().trimmed();
        if (!text.isEmpty()) {
            result.append(text);
        }
    }
    return result;
}

static PaperLibraryModel::CorpusHealth readCorpusHealth(const QString &corpusDir)
{
    PaperLibraryModel::CorpusHealth health;
    QFile file(QDir(corpusDir).filePath(QStringLiteral("corpus_state.json")));
    if (!file.open(QIODevice::ReadOnly)) {
        health.issues.append(QStringLiteral("corpus_state.json is missing"));
        return health;
    }

    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        health.issues.append(QStringLiteral("corpus_state.json is invalid"));
        return health;
    }

    const QJsonObject object = document.object();
    if (object.value(QStringLiteral("schema_version")).toInt(-1) != 1) {
        health.issues.append(QStringLiteral("corpus_state.json uses an unsupported schema"));
        return health;
    }

    health.generatedAt = object.value(QStringLiteral("generated_at")).toString().trimmed();
    health.issues = jsonStringList(object.value(QStringLiteral("issues")));
    health.warnings = jsonStringList(object.value(QStringLiteral("warnings")));
    const QJsonObject catalog = object.value(QStringLiteral("catalog")).toObject();
    if (catalog.value(QStringLiteral("rows")).isDouble()) {
        health.catalogRows = catalog.value(QStringLiteral("rows")).toInt(-1);
    }
    health.catalogRevision = catalog.value(QStringLiteral("revision")).toString().trimmed();
    const QJsonObject artifacts = object.value(QStringLiteral("artifacts")).toObject();
    const QJsonObject manifest = artifacts.value(QStringLiteral("manifest")).toObject();
    const QJsonValue manifestFresh = manifest.value(QStringLiteral("fresh"));
    const QJsonValue searchFresh = artifacts.value(QStringLiteral("search")).toObject().value(QStringLiteral("fresh"));
    const QJsonValue graphFresh = artifacts.value(QStringLiteral("graph")).toObject().value(QStringLiteral("fresh"));
    health.manifestFresh = manifestFresh.toBool(false);
    health.manifestSha256 = manifest.value(QStringLiteral("sha256")).toString().trimmed();
    health.manifestSourceRevision = manifest.value(QStringLiteral("manifest_source_revision")).toString().trimmed();
    health.searchFresh = searchFresh.toBool(false);
    health.graphFresh = graphFresh.toBool(false);

    const bool complete = object.value(QStringLiteral("healthy")).isBool()
        && manifestFresh.isBool() && searchFresh.isBool() && graphFresh.isBool();
    if (!complete) {
        health.status = PaperLibraryModel::CorpusHealth::Degraded;
        health.searchFresh = false;
        health.graphFresh = false;
        health.issues.append(QStringLiteral("corpus health state is incomplete"));
    } else if (object.value(QStringLiteral("healthy")).toBool()) {
        if (health.manifestFresh && health.searchFresh && health.graphFresh) {
            health.status = PaperLibraryModel::CorpusHealth::Healthy;
        } else {
            health.status = PaperLibraryModel::CorpusHealth::Degraded;
            health.searchFresh = false;
            health.graphFresh = false;
            health.issues.append(QStringLiteral("corpus health state is inconsistent"));
        }
    } else {
        health.status = PaperLibraryModel::CorpusHealth::Degraded;
        if (health.issues.isEmpty()) {
            health.issues.append(QStringLiteral("one or more corpus artifacts are stale"));
        }
    }

    if (health.searchFresh) {
        const QString searchDb = resolveIndexDb(corpusDir, QStringLiteral("search.db"), QStringLiteral("paper_fts"));
        if (searchDb.isEmpty() || !catalogDbHasTable(searchDb, QStringLiteral("paper_search_rows"))) {
            health.status = PaperLibraryModel::CorpusHealth::Degraded;
            health.searchFresh = false;
            health.issues.append(QStringLiteral("full-text index is marked fresh but missing"));
        }
    }
    if (health.graphFresh
        && resolveIndexDb(corpusDir, QStringLiteral("graph/graph.db"), QStringLiteral("related_edges")).isEmpty()) {
        health.status = PaperLibraryModel::CorpusHealth::Degraded;
        health.graphFresh = false;
        health.issues.append(QStringLiteral("semantic graph is marked fresh but missing"));
    }
    health.issues.removeDuplicates();
    return health;
}

static void attestManifest(const QString &corpusDir,
                           const QByteArray &catalogBytes,
                           PaperLibraryModel::CorpusHealth &health)
{
    QFile file(QDir(corpusDir).filePath(QStringLiteral("catalog.meta.json")));
    QJsonParseError error;
    QJsonDocument document;
    if (file.open(QIODevice::ReadOnly)) {
        document = QJsonDocument::fromJson(file.readAll(), &error);
    }
    const QJsonObject metadata = document.isObject() ? document.object() : QJsonObject();
    const QString actualHash = QString::fromLatin1(
        QCryptographicHash::hash(catalogBytes, QCryptographicHash::Sha256).toHex());
    const QString sidecarHash = metadata.value(QStringLiteral("manifest_sha256")).toString().trimmed();
    const QString sidecarCatalogRevision = metadata.value(QStringLiteral("catalog_revision")).toString().trimmed();
    const QString sidecarSourceRevision = metadata.value(QStringLiteral("manifest_source_revision")).toString().trimmed();
    const int sidecarRows = metadata.value(QStringLiteral("rows")).toInt(-1);
    bool valid = error.error == QJsonParseError::NoError
        && metadata.value(QStringLiteral("schema_version")).toInt(-1) == 1
        && !sidecarHash.isEmpty() && sidecarHash == actualHash
        && sidecarRows >= 0;
    if (health.catalogRows >= 0) {
        valid = valid && sidecarRows == health.catalogRows;
    }
    if (!health.catalogRevision.isEmpty()) {
        valid = valid && sidecarCatalogRevision == health.catalogRevision;
    }
    if (!health.manifestSha256.isEmpty()) {
        valid = valid && health.manifestSha256 == sidecarHash;
    }
    if (!health.manifestSourceRevision.isEmpty()) {
        valid = valid && health.manifestSourceRevision == sidecarSourceRevision;
    }
    if (health.status == PaperLibraryModel::CorpusHealth::Healthy) {
        valid = valid && health.manifestFresh
            && !health.catalogRevision.isEmpty()
            && !health.manifestSha256.isEmpty()
            && !health.manifestSourceRevision.isEmpty();
    }
    health.manifestFresh = health.manifestFresh && valid;
    if (!valid) {
        health.status = PaperLibraryModel::CorpusHealth::Degraded;
        health.searchFresh = false;
        health.graphFresh = false;
        health.issues.prepend(QStringLiteral("catalog manifest attestation is missing or mismatched"));
        health.issues.removeDuplicates();
    }
}

PaperLibraryModel::PaperLibraryModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

PaperLibraryModel::~PaperLibraryModel()
{
    // A newer catalog generation may start from the previous generation's loaded() callback,
    // before that previous QThread's finished event is delivered. Reclaim every such tail worker,
    // not just m_worker (which always names the newest generation).
    const QList<QThread *> workers = m_workers.values();
    for (QThread *worker : workers) {
        worker->disconnect(this);
    }
    for (QThread *worker : workers) {
        worker->wait();
        delete worker;
    }
    m_workers.clear();
    m_worker = nullptr;
}

QString PaperLibraryModel::configuredCorpusDir()
{
    const KConfigGroup group = paperLibraryConfigGroup(QStringLiteral("General"));
    return group.readEntry("PaperLibraryPath", QString(QDir::homePath() + QStringLiteral("/Projects/PaperLibrary")));
}

bool PaperLibraryModel::corpusExists(const QString &corpusDir)
{
    return !corpusDir.isEmpty() && QFileInfo::exists(catalogPath(corpusDir));
}

QString PaperLibraryModel::corpusDir() const
{
    return m_corpusDir;
}

void PaperLibraryModel::load(const QString &corpusDir)
{
    const QDateTime catalogMtime = QFileInfo(catalogPath(corpusDir)).lastModified();
    const QDateTime manifestMtime = QFileInfo(QDir(corpusDir).filePath(QStringLiteral("catalog.meta.json"))).lastModified();
    const QDateTime healthMtime = QFileInfo(QDir(corpusDir).filePath(QStringLiteral("corpus_state.json"))).lastModified();
    // Repeated ensure-loaded calls for the same snapshot are no-ops. A different corpus or a
    // catalog whose mtime moved is a new generation and may supersede an in-flight parse.
    if (m_loading && corpusDir == m_corpusDir && catalogMtime == m_catalogMtime
        && manifestMtime == m_manifestMtime && healthMtime == m_healthMtime) {
        return;
    }
    m_loading = true;
    m_corpusDir = corpusDir;
    m_catalogMtime = catalogMtime;
    m_manifestMtime = manifestMtime;
    m_healthMtime = healthMtime;
    const quint64 generation = ++m_loadGeneration;

    // Parse and enrich on a worker thread — an ~18k-line catalog must never
    // stall the UI — then land the newest generation in one queued model reset.
    QThread *const worker = QThread::create([this, corpusDir, generation]() {
        QList<Record> records;
        QByteArray catalogBytes;
        QFile catalog(catalogPath(corpusDir));
        if (catalog.open(QIODevice::ReadOnly)) {
            catalogBytes = catalog.readAll();
            records = parseCatalog(catalogBytes);
        }
        sortRecords(records);
        enrichRecords(records, corpusDir);
        CorpusHealth health = readCorpusHealth(corpusDir);
        attestManifest(corpusDir, catalogBytes, health);
        QMetaObject::invokeMethod(this,
                                  [this, records = std::move(records), health = std::move(health), generation]() mutable {
                                      finishLoad(std::move(records), std::move(health), generation);
                                  },
                                  Qt::QueuedConnection);
    });
    worker->setParent(this);
    m_workers.insert(worker);
    m_worker = worker;
    connect(worker, &QThread::finished, this, [this, worker]() {
        m_workers.remove(worker);
        if (m_worker == worker) {
            m_worker = nullptr;
        }
        worker->deleteLater();
    });
    worker->start();
}

void PaperLibraryModel::reloadIfChanged()
{
    if (!m_loaded) {
        return;
    }
    if (QFileInfo(catalogPath(m_corpusDir)).lastModified() != m_catalogMtime
        || QFileInfo(QDir(m_corpusDir).filePath(QStringLiteral("catalog.meta.json"))).lastModified() != m_manifestMtime
        || QFileInfo(QDir(m_corpusDir).filePath(QStringLiteral("corpus_state.json"))).lastModified() != m_healthMtime) {
        load(m_corpusDir);
    }
}

bool PaperLibraryModel::isLoaded() const
{
    return m_loaded;
}

void PaperLibraryModel::finishLoad(QList<Record> records, CorpusHealth health, quint64 generation)
{
    if (generation != m_loadGeneration) {
        return; // superseded worker: never publish an older catalog snapshot
    }
    if (health.catalogRows >= 0 && health.catalogRows != records.count()) {
        health.status = CorpusHealth::Degraded;
        health.searchFresh = false;
        health.graphFresh = false;
        health.issues.prepend(QStringLiteral("corpus health covers %1 of %2 catalog rows").arg(health.catalogRows).arg(records.count()));
        health.issues.removeDuplicates();
    }
    TelemetryScope op(QStringLiteral("corpus_load")); // the reset + downstream shelf builds this triggers
    beginResetModel();
    m_catalogRecords = std::move(records);
    m_corpusHealth = std::move(health);
    rebuildRecords();
    endResetModel();
    m_loading = false;
    m_loaded = true;
    Q_EMIT loaded(m_records.count());
}

/** Case- and punctuation-insensitive title key, for matching an import to a catalog row. */
static QString titleKey(const QString &title)
{
    QString key;
    key.reserve(title.size());
    for (const QChar ch : title) {
        if (ch.isLetterOrNumber()) {
            key.append(ch.toCaseFolded());
        }
    }
    return key;
}

void PaperLibraryModel::rebuildRecords()
{
    m_records = m_catalogRecords;
    if (!m_localBooks.isEmpty()) {
        QSet<QString> known;
        known.reserve(m_catalogRecords.size());
        for (const Record &record : std::as_const(m_catalogRecords)) {
            known.insert(titleKey(record.title));
        }
        for (const Record &book : std::as_const(m_localBooks)) {
            // The catalog row is the richer one: it carries genre, topics and reading level.
            if (!known.contains(titleKey(book.title))) {
                m_records.append(book);
            }
        }
    }
    rebuildLookupRows();
}

void PaperLibraryModel::setLocalBooks(const QList<Record> &books)
{
    // LibraryView::refresh() runs on every library-tab show and calls this. A reset here forces
    // every sectioned proxy to rebuild over the whole corpus, so an unconditional reset made
    // switching to a library tab pay a 21k-row rebuild every time. Imports rarely change, so skip
    // the reset when the set is byte-for-byte the same. slug + title + path identify a local book.
    QByteArray signature;
    signature.reserve(books.size() * 64);
    for (const Record &book : books) {
        signature += book.slug.toUtf8();
        signature += '\x1f';
        signature += book.title.toUtf8();
        signature += '\x1f';
        signature += book.pdfPath.toUtf8();
        signature += '\x1e';
    }
    if (m_localBooksSet && signature == m_localBooksSignature) {
        return;
    }
    m_localBooksSet = true;
    m_localBooksSignature = signature;

    beginResetModel();
    m_localBooks = books;
    rebuildRecords();
    endResetModel();
}

int PaperLibraryModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_records.count();
}

static QString joinNonEmpty(const QStringList &parts)
{
    QStringList kept;
    for (const QString &part : parts) {
        if (!part.isEmpty()) {
            kept.append(part);
        }
    }
    return kept.join(QStringLiteral(" · "));
}

static bool hasWordBoundary(const QString &text, int start, int length)
{
    const int before = start - 1;
    const int after = start + length;
    const bool leftOk = before < 0 || !text.at(before).isLetterOrNumber();
    const bool rightOk = after >= text.size() || !text.at(after).isLetterOrNumber();
    return leftOk && rightOk;
}

static bool containsWholeWord(const QString &text, const QString &word)
{
    int pos = text.indexOf(word);
    while (pos >= 0) {
        if (hasWordBoundary(text, pos, word.size())) {
            return true;
        }
        pos = text.indexOf(word, pos + word.size());
    }
    return false;
}

static bool containsAnyWholeWord(const QString &text, const QStringList &words)
{
    return std::any_of(words.cbegin(), words.cend(), [&text](const QString &word) {
        return containsWholeWord(text, word);
    });
}

static bool containsAnyNeedle(const QString &text, const QStringList &needles)
{
    return std::any_of(needles.cbegin(), needles.cend(), [&text](const QString &needle) {
        return text.contains(needle);
    });
}

// Memoize a pure classifier keyed by its (content) inputs. The topic/shelf matchers below are the
// per-row regex sweep that dominated "open a new tab and click the subtabs" — each library tab
// builds its own models and re-classified the whole corpus. Because the result is a pure function of
// the input string, the first call computes and every later one (any sectioned model, any tab) is a
// hash lookup; keying by content means it never goes stale, so no invalidation is needed. The static
// lives per template instantiation, and each call site passes a uniquely-typed lambda, so every
// wrapped matcher gets its own cache.
template <typename Fn>
static bool memoMatch(const QString &key, Fn &&compute)
{
    static QHash<QString, bool> memo;
    const auto it = memo.constFind(key);
    if (it != memo.cend()) {
        return it.value();
    }
    return *memo.insert(key, compute());
}

// String-valued equivalent for the per-row derivations (publication kind, etc.) that internally run
// several of the matchers above; memoizing at this level caches a whole cluster of heuristics at once.
template <typename Fn>
static QString memoMatchStr(const QString &key, Fn &&compute)
{
    static QHash<QString, QString> memo;
    const auto it = memo.constFind(key);
    if (it != memo.cend()) {
        return it.value();
    }
    return *memo.insert(key, compute());
}

static bool recordMatchesMnd(const QString &text)
{
    return memoMatch(text, [&]() -> bool {
    return containsAnyWholeWord(text, {QStringLiteral("mnd"), QStringLiteral("als"), QStringLiteral("sod1"), QStringLiteral("c9orf72"), QStringLiteral("tdp43")})
        || containsAnyNeedle(text,
                             {QStringLiteral("motor neurone"),
                              QStringLiteral("motor neuron"),
                              QStringLiteral("amyotrophic lateral sclerosis"),
                              QStringLiteral("frontotemporal dementia"),
                              QStringLiteral("neurofilament"),
                              QStringLiteral("tdp-43"),
                              QStringLiteral("neurodegenerative"),
                              QStringLiteral("mnd-funnel"),
                              QStringLiteral("md-project-review-set")});
    });
}

static bool recordMatchesPsychiatry(const QString &text)
{
    return memoMatch(text, [&]() -> bool {
    if (containsAnyNeedle(text,
                          {QStringLiteral("great depression"),
                           QStringLiteral("world war"),
                           QStringLiteral("america that went to war"),
                           QStringLiteral("went to war"),
                           QStringLiteral("submarine warfare"),
                           QStringLiteral("military history"),
                           QStringLiteral("wwii"),
                           QStringLiteral("robert caro"),
                           QStringLiteral("robert a. caro"),
                           QStringLiteral("path to power"),
                           QStringLiteral("means of ascent"),
                           QStringLiteral("master of the senate"),
                           QStringLiteral("passage of power"),
                           QStringLiteral("years of lyndon johnson"),
                           QStringLiteral("lyndon b. johnson"),
                           QStringLiteral("presidential biography")})) {
        return false;
    }
    return containsAnyNeedle(text,
                             {QStringLiteral("psychiat"),
                              QStringLiteral("mental health"),
                              QStringLiteral("depression"),
                              QStringLiteral("anxiety"),
                              QStringLiteral("bipolar"),
                              QStringLiteral("schizophrenia"),
                              QStringLiteral("psychosis"),
                              QStringLiteral("suicide"),
                              QStringLiteral("substance use"),
                              QStringLiteral("addiction"),
                              QStringLiteral("adhd"),
                              QStringLiteral("autism"),
                              QStringLiteral("ptsd"),
                              QStringLiteral("personality disorder")});
    });
}

static bool recordMatchesPaediatrics(const QString &text)
{
    return memoMatch(text, [&]() -> bool {
    return containsAnyNeedle(text,
                             {QStringLiteral("paediatric"),
                              QStringLiteral("pediatric"),
                              QStringLiteral("neonat"),
                              QStringLiteral("infant"),
                              QStringLiteral("childhood"),
                              QStringLiteral("children"),
                              QStringLiteral("adolescent"),
                              QStringLiteral("developmental")});
    });
}

static bool recordMatchesObgyn(const QString &text)
{
    return memoMatch(text, [&]() -> bool {
    return containsAnyNeedle(text,
                             {QStringLiteral("obstetric"),
                              QStringLiteral("gynecology"),
                              QStringLiteral("gynaecology"),
                              QStringLiteral("pregnancy"),
                              QStringLiteral("maternal"),
                              QStringLiteral("fetal"),
                              QStringLiteral("foetal"),
                              QStringLiteral("antenatal"),
                              QStringLiteral("postpartum"),
                              QStringLiteral("reproductive")});
    });
}

static bool recordMatchesBeyondBayes(const QString &text, const QString &source, const QString &journal)
{
    Q_UNUSED(journal);
    return memoMatch(text + QLatin1Char('\x1f') + source, [&]() -> bool {
    return source.contains(QStringLiteral("highdimensional")) || source.contains(QStringLiteral("beyond-bayes")) || source.contains(QStringLiteral("beyond_bayes"))
        || containsAnyNeedle(text,
                             {QStringLiteral("beyond bayes"),
                              QStringLiteral("beyond bayesian"),
                              QStringLiteral("ian todd"),
                              QStringLiteral("dimensionality bound"),
                              QStringLiteral("sub-landauer"),
                              QStringLiteral("projection bound"),
                              QStringLiteral("timing inaccessibility"),
                              QStringLiteral("high-dimensional coherence"),
                              QStringLiteral("coherence time in biological oscillator")});
    });
}

static bool recordMatchesPeerReview(const QString &text, const QString &source)
{
    return memoMatch(text + QLatin1Char('\x1f') + source, [&]() -> bool {
    return source.contains(QStringLiteral("peer-review")) || source.contains(QStringLiteral("peerreview")) || source.contains(QStringLiteral("review-assignment"))
        || containsAnyNeedle(text,
                             {QStringLiteral("peer review"),
                              QStringLiteral("reviewer comments"),
                              QStringLiteral("major revisions"),
                              QStringLiteral("minor revisions"),
                              QStringLiteral("manuscript review"),
                              QStringLiteral("referee report")});
    });
}

static bool recordMatchesGameOfThrones(const QString &text)
{
    return memoMatch(text, [&]() -> bool {
    return containsAnyNeedle(text, {QStringLiteral("game of thrones"), QStringLiteral("song of ice and fire"), QStringLiteral("george r. r. martin"), QStringLiteral("george rr martin")});
    });
}

static bool recordLooksBookishFromText(const QString &text)
{
    return memoMatch(text, [&]() -> bool {
    return containsAnyNeedle(text,
                             {QStringLiteral("book:"),
                              QStringLiteral("aa_book"),
                              QStringLiteral("(book)"),
                              QStringLiteral(".epub"),
                              QStringLiteral(".azw3"),
                              QStringLiteral(".mobi"),
                              QStringLiteral("annas archive"),
                              QStringLiteral("anna's archive"),
                              QStringLiteral("isbn"),
                              QStringLiteral("bantam books"),
                              QStringLiteral("bantam spectra"),
                              QStringLiteral("penguin random house")});
    });
}

static bool recordMatchesFiction(const QString &text)
{
    return memoMatch(text, [&]() -> bool {
    if (containsAnyNeedle(text, {QStringLiteral("nonfiction"), QStringLiteral("non-fiction"), QStringLiteral("non fiction")})) {
        return false;
    }
    if (recordMatchesGameOfThrones(text)) {
        return true;
    }
    if (containsAnyNeedle(text,
                          {QStringLiteral("octavia butler"),
                           QStringLiteral("parable of the sower"),
                           QStringLiteral("frank herbert"),
                           QStringLiteral("dune messiah"),
                           QStringLiteral("children of dune"),
                           QStringLiteral("god emperor of dune"),
                           QStringLiteral("kim stanley robinson"),
                           QStringLiteral("years of rice and salt"),
                           QStringLiteral("green mars"),
                           QStringLiteral("blue mars"),
                           QStringLiteral("red mars"),
                           QStringLiteral("new york 2140"),
                           QStringLiteral("the best of kim stanley robinson"),
                           QStringLiteral("mars trilogy"),
                           QStringLiteral("ursula k. le guin"),
                           QStringLiteral("ursula k le guin"),
                           QStringLiteral("earthsea"),
                           QStringLiteral("the dispossessed"),
                           QStringLiteral("omelas"),
                           QStringLiteral("alexis wright"),
                           QStringLiteral("carpentaria"),
                           QStringLiteral("praiseworthy"),
                           QStringLiteral("the hobbit"),
                           QStringLiteral("lord of the rings"),
                           QStringLiteral("the silmarillion"),
                           QStringLiteral("unfinished tales"),
                           QStringLiteral("one day in the life of ivan denisovich"),
                           QStringLiteral("the water knife"),
                           QStringLiteral("the windup girl"),
                           QStringLiteral("song of ice and fire")})) {
        return true;
    }
    if (containsWholeWord(text, QStringLiteral("dune")) && recordLooksBookishFromText(text)) {
        return true;
    }
    if (!recordLooksBookishFromText(text)) {
        return false;
    }
    return containsAnyWholeWord(text, {QStringLiteral("fiction"), QStringLiteral("fantasy"), QStringLiteral("novel")})
        || text.contains(QStringLiteral("speculative fiction")) || text.contains(QStringLiteral("science fiction"));
    });
}

static bool recordMatchesCaroLbj(const QString &text)
{
    return memoMatch(text, [&]() -> bool {
    return containsAnyNeedle(text,
                             {QStringLiteral("robert caro"),
                              QStringLiteral("robert a. caro"),
                              QStringLiteral("lyndon johnson"),
                              QStringLiteral("lyndon b. johnson"),
                              QStringLiteral("lbj"),
                              QStringLiteral("years of lyndon johnson"),
                              QStringLiteral("path to power"),
                              QStringLiteral("means of ascent"),
                              QStringLiteral("master of the senate"),
                              QStringLiteral("passage of power")});
    });
}

static bool recordMatchesPolitics(const QString &text)
{
    return memoMatch(text, [&]() -> bool {
    return recordMatchesCaroLbj(text)
        || containsAnyNeedle(text,
                             {QStringLiteral("politics"),
                              QStringLiteral("political"),
                              QStringLiteral("congress"),
                              QStringLiteral("democracy"),
                              QStringLiteral("government"),
                              QStringLiteral("public policy"),
                              QStringLiteral("presidential biography")});
    });
}

static bool recordMatchesGraeber(const QString &text)
{
    return memoMatch(text, [&]() -> bool {
    return containsAnyNeedle(text, {QStringLiteral("david graeber"), QStringLiteral("graeber"), QStringLiteral("bullshit jobs"), QStringLiteral("debt:"), QStringLiteral("dawn of everything")});
    });
}

static bool recordMatchesAnthropology(const QString &text)
{
    return memoMatch(text, [&]() -> bool {
    return recordMatchesGraeber(text) || containsAnyNeedle(text, {QStringLiteral("anthropolog"), QStringLiteral("ethnograph"), QStringLiteral("archaeolog"), QStringLiteral("kinship"), QStringLiteral("debt and exchange")});
    });
}

static bool recordMatchesMedicine(const QString &text)
{
    return memoMatch(text, [&]() -> bool {
    return recordMatchesMnd(text) || recordMatchesPsychiatry(text) || recordMatchesPaediatrics(text) || recordMatchesObgyn(text)
        || containsAnyNeedle(text,
                             {QStringLiteral("medicine"),
                              QStringLiteral("medical"),
                              QStringLiteral("clinical"),
                              QStringLiteral("diagnos"),
                              QStringLiteral("treatment"),
                              QStringLiteral("therapy"),
                              QStringLiteral("patient"),
                              QStringLiteral("anatomy"),
                              QStringLiteral("physiology"),
                              QStringLiteral("pathology"),
                              QStringLiteral("pharmacology"),
                              QStringLiteral("surgery"),
                              QStringLiteral("emergency")});
    });
}

static bool recordMatchesClinicalEssentials(const QString &text)
{
    return containsAnyNeedle(text,
                             {QStringLiteral("clinical examination"),
                              QStringLiteral("clinical exam"),
                              QStringLiteral("talley"),
                              QStringLiteral("o'connor"),
                              QStringLiteral("oxford handbook of clinical medicine"),
                              QStringLiteral("clinical diagnosis"),
                              QStringLiteral("physical diagnosis"),
                              QStringLiteral("general practice"),
                              QStringLiteral("emergency medicine")});
}

static bool recordMatchesNeuroMedicine(const QString &text)
{
    return recordMatchesMnd(text)
        || containsAnyNeedle(text,
                             {QStringLiteral("neuroanatomy"),
                              QStringLiteral("neuroscience"),
                              QStringLiteral("neurology"),
                              QStringLiteral("brain"),
                              QStringLiteral("spinal cord"),
                              QStringLiteral("neuron"),
                              QStringLiteral("motor system")});
}

static bool recordMatchesMedicalCoreScience(const QString &text)
{
    return containsAnyNeedle(text,
                             {QStringLiteral("pathoma"),
                              QStringLiteral("pathology"),
                              QStringLiteral("pharmacology"),
                              QStringLiteral("physiology"),
                              QStringLiteral("basic and clinical pharmacology"),
                              QStringLiteral("costanzo")});
}

static bool recordMatchesMedicalAnatomy(const QString &text)
{
    return containsAnyNeedle(text,
                             {QStringLiteral("anatomy"),
                              QStringLiteral("netter"),
                              QStringLiteral("gray's anatomy"),
                              QStringLiteral("grays anatomy"),
                              QStringLiteral("snell")});
}

static bool recordMatchesPatientSafety(const QString &text)
{
    return containsAnyNeedle(text,
                             {QStringLiteral("patient safety"),
                              QStringLiteral("human error"),
                              QStringLiteral("quality improvement"),
                              QStringLiteral("healthcare systems"),
                              QStringLiteral("clinical governance")});
}

static bool recordMatchesNonfiction(const QString &text, const QString &source, const QString &journal)
{
    return memoMatch(text + QLatin1Char('\x1f') + source + QLatin1Char('\x1f') + journal, [&]() -> bool {
    if (recordMatchesFiction(text)) {
        return false;
    }
    const bool bookLike = source.startsWith(QLatin1String("book:")) || source == QLatin1String("aa_book") || journal == QLatin1String("(book)") || text.contains(QStringLiteral("annas archive"));
    return recordMatchesCaroLbj(text) || recordMatchesAnthropology(text) || recordMatchesPolitics(text) || (bookLike && containsAnyNeedle(text,
                                                                                                                   {QStringLiteral("nonfiction"),
                                                                                                                    QStringLiteral("biography"),
                                                                                                                    QStringLiteral("history"),
                                                                                                                    QStringLiteral("world war"),
                                                                                                                    QStringLiteral("went to war"),
                                                                                                                    QStringLiteral("america that went to war"),
                                                                                                                    QStringLiteral("submarine warfare"),
                                                                                                                    QStringLiteral("military history"),
                                                                                                                    QStringLiteral("conservation"),
                                                                                                                    QStringLiteral("ecology"),
                                                                                                                    QStringLiteral("environment"),
                                                                                                                    QStringLiteral("almanac"),
                                                                                                                    QStringLiteral("memoir"),
                                                                                                                    QStringLiteral("essay"),
                                                                                                                    QStringLiteral("politics"),
                                                                                                                    QStringLiteral("anthropology"),
                                                                                                                    QStringLiteral("science")}));
    });
}

static bool recordMatchesTextbook(const QString &text, const QString &source)
{
    return memoMatch(text + QLatin1Char('\x1f') + source, [&]() -> bool {
    const bool bookSource = source.startsWith(QLatin1String("book:")) || source == QLatin1String("aa_book");
    const bool textbookSignal = containsAnyNeedle(text,
                                                  {QStringLiteral("textbook"),
                                                   QStringLiteral("handbook"),
                                                   QStringLiteral("manual"),
                                                   QStringLiteral("lecture notes"),
                                                   QStringLiteral("course notes"),
                                                   QStringLiteral("fundamentals"),
                                                   QStringLiteral("essentials"),
                                                   QStringLiteral("principles of "),
                                                   QStringLiteral("introduction to "),
                                                   QStringLiteral("clinical examination"),
                                                   QStringLiteral("patient safety"),
                                                   QStringLiteral("anatomy"),
                                                   QStringLiteral("physiology"),
                                                   QStringLiteral("pathology"),
                                                   QStringLiteral("pharmacology"),
                                                   QStringLiteral("epidemiology"),
                                                   QStringLiteral("statistics"),
                                                   QStringLiteral("neuroscience"),
                                                   QStringLiteral("medicine"),
                                                   QStringLiteral("medical"),
                                                   QStringLiteral("medical students")});
    return (bookSource && textbookSignal) || text.contains(QStringLiteral("textbook of "));
    });
}

static bool recordMatchesBook(const QString &text, const QString &source, const QString &journal)
{
    return source.startsWith(QLatin1String("book:")) || source == QLatin1String("aa_book") || journal == QLatin1String("(book)") || text.contains(QStringLiteral("annas archive"));
}

static QString sourceRowDuplicateWorkKey(const PaperLibraryModel *source, int row)
{
    const QModelIndex index = source->index(row);
    const QString sourceName = index.data(PaperLibraryModel::SourceRole).toString().toCaseFolded();
    const QString journal = index.data(PaperLibraryModel::JournalRole).toString().toCaseFolded();
    const QString haystack = index.data(PaperLibraryModel::HaystackRole).toString();
    if (recordMatchesBook(haystack, sourceName, journal)) {
        return canonicalDuplicateWorkKey(index.data(Qt::DisplayRole).toString(), index.data(PaperLibraryModel::AuthorsRole).toString(), haystack);
    }

    const QString doi = index.data(PaperLibraryModel::DoiRole).toString().trimmed().toCaseFolded();
    return doi.isEmpty() ? QString() : QStringLiteral("doi|%1").arg(doi);
}

static QString publicationKindFor(const QString &text, const QString &source, const QString &journal)
{
    return memoMatchStr(text + QLatin1Char('\x1f') + source + QLatin1Char('\x1f') + journal, [&]() -> QString {
    if (recordMatchesTextbook(text, source)) {
        return QStringLiteral("Textbooks");
    }
    if (recordMatchesBook(text, source, journal)) {
        return QStringLiteral("Books");
    }
    if (source.startsWith(QLatin1String("guideline:")) || source.startsWith(QLatin1String("localevidence:")) || source == QLatin1String("gov-report")) {
        return QStringLiteral("Guidelines & Evidence");
    }
    if (recordMatchesPeerReview(text, source)) {
        return QStringLiteral("Peer Reviews");
    }
    if (journal.contains(QStringLiteral("cochrane database of systematic reviews")) || containsAnyNeedle(text, {QStringLiteral("systematic review"), QStringLiteral("meta-analysis"), QStringLiteral("scoping review"), QStringLiteral("review and meta")})) {
        return QStringLiteral("Reviews");
    }
    if (containsAnyNeedle(text, {QStringLiteral("randomized"), QStringLiteral("randomised"), QStringLiteral("clinical trial"), QStringLiteral("cohort study"), QStringLiteral("case-control"), QStringLiteral("cross-sectional")})) {
        return QStringLiteral("Studies");
    }
    return QStringLiteral("Papers");
    });
}

static QString sourceBucketFor(const QString &source)
{
    if (source == QLatin1String("md-project-review-set") || source.contains(QStringLiteral("mnd"))) {
        return QStringLiteral("MD / MND Project");
    }
    if (source.startsWith(QLatin1String("book:")) || source == QLatin1String("aa_book")) {
        return QStringLiteral("Books");
    }
    if (source.startsWith(QLatin1String("guideline:")) || source.startsWith(QLatin1String("localevidence:")) || source == QLatin1String("gov-report")) {
        return QStringLiteral("Guidelines & Evidence");
    }
    if (source == QLatin1String("unpaywall") || source == QLatin1String("europepmc")) {
        return QStringLiteral("Open Access");
    }
    if (source == QLatin1String("scihub") || source == QLatin1String("libgen") || source == QLatin1String("aa_fast_download")) {
        return QStringLiteral("Acquired PDFs");
    }
    if (source.startsWith(QLatin1String("harvest:"))) {
        return QStringLiteral("Harvests");
    }
    if (source.startsWith(QLatin1String("rescued-"))) {
        return QStringLiteral("Recovered");
    }
    if (source.isEmpty()) {
        return QStringLiteral("Unknown Source");
    }
    return source;
}

static QString projectBucketFor(const QString &text, const QString &source, const QString &journal)
{
    if (recordMatchesMnd(text) || source == QLatin1String("md-project-review-set")) {
        return QStringLiteral("MND / ALS");
    }
    if (recordMatchesPsychiatry(text)) {
        return QStringLiteral("Psychiatry");
    }
    if (recordMatchesPaediatrics(text)) {
        return QStringLiteral("Paeds Rotation");
    }
    if (recordMatchesObgyn(text)) {
        return QStringLiteral("OBGYN Rotation");
    }
    if (recordMatchesBeyondBayes(text, source, journal)) {
        return QStringLiteral("Beyond Bayes / Highdimensional");
    }
    if (recordMatchesPeerReview(text, source)) {
        return QStringLiteral("Peer Reviews");
    }
    if (source.startsWith(QLatin1String("localevidence:"))) {
        return QStringLiteral("LocalEvidence");
    }
    if (source.startsWith(QLatin1String("guideline:")) || journal.contains(QStringLiteral("clinical practice guidelines"))) {
        return QStringLiteral("Clinical Guidelines");
    }
    if (journal == QLatin1String("biosystems") || source.contains(QStringLiteral("biosystems")) || containsAnyNeedle(text, {QStringLiteral("bioelectric"), QStringLiteral("morphogenesis"), QStringLiteral("anticipatory systems")})) {
        return QStringLiteral("BioSystems / Highdimensional");
    }
    if (containsAnyNeedle(text, {QStringLiteral("statistics"), QStringLiteral("epidemiology"), QStringLiteral("prediction model"), QStringLiteral("bayesian"), QStringLiteral("monte carlo"), QStringLiteral("information geometry")})) {
        return QStringLiteral("Methods & Statistics");
    }
    if (recordMatchesAnthropology(text)) {
        return QStringLiteral("Anthropology");
    }
    if (recordMatchesPolitics(text)) {
        return QStringLiteral("Politics");
    }
    if (recordMatchesBook(text, source, journal)) {
        return QStringLiteral("Books");
    }
    return QStringLiteral("General Research");
}

static bool recordMatchesPapersNovelty(const QString &text, const QString &source, const QString &journal)
{
    if (recordMatchesMnd(text) || recordMatchesBeyondBayes(text, source, journal) || recordMatchesPeerReview(text, source) || recordMatchesBook(text, source, journal)) {
        return false;
    }
    return projectBucketFor(text, source, journal) == QLatin1String("BioSystems / Highdimensional")
        || containsAnyNeedle(text,
                             {QStringLiteral("bioelectric"),
                              QStringLiteral("anticipatory"),
                              QStringLiteral("morphogenesis"),
                              QStringLiteral("active inference"),
                              QStringLiteral("free energy principle"),
                              QStringLiteral("complex systems"),
                              QStringLiteral("systems biology"),
                              QStringLiteral("causal model"),
                              QStringLiteral("mechanistic model"),
                              QStringLiteral("neuroimmune"),
                              QStringLiteral("microbiome"),
                              QStringLiteral("computational boundary"),
                              QStringLiteral("information theory"),
                              QStringLiteral("network neuroscience")});
}

static QString papersReadNextSectionFor(const QModelIndex &index, const QString &text, const QString &source, const QString &journal)
{
    if (index.data(PaperLibraryModel::PinnedRole).toBool()) {
        return QStringLiteral("Pinned");
    }
    if (index.data(PaperLibraryModel::AccessCountRole).toInt() > 0) {
        return QStringLiteral("Continue Reading");
    }
    if (recordMatchesBeyondBayes(text, source, journal) || recordMatchesPeerReview(text, source)) {
        return QStringLiteral("Active Work");
    }
    if (source == QLatin1String("md-project-review-set") || recordMatchesMnd(text)) {
        return QStringLiteral("MND Project");
    }
    if (recordMatchesPaediatrics(text) || recordMatchesObgyn(text) || recordMatchesPsychiatry(text)) {
        return QStringLiteral("Clinical Rotations");
    }
    if (projectBucketFor(text, source, journal) == QLatin1String("Methods & Statistics")) {
        return QStringLiteral("Methods & Statistics");
    }
    if (recordMatchesPapersNovelty(text, source, journal)) {
        return QStringLiteral("Novelty / Adjacent Ideas");
    }
    const QString kind = publicationKindFor(text, source, journal);
    if (kind == QLatin1String("Guidelines & Evidence") || kind == QLatin1String("Reviews")) {
        return QStringLiteral("Reviews & Guidelines");
    }
    if (index.data(PaperLibraryModel::CitedByCountRole).toInt() >= 100) {
        return QStringLiteral("Highly Cited");
    }
    if (index.data(PaperLibraryModel::AddedRole).toString() >= QLatin1String("2026-06")) {
        return QStringLiteral("Recent Additions");
    }
    return QStringLiteral("Other Papers");
}

static int sectionRank(const QString &section)
{
    const QStringList order = {
        QStringLiteral("Pinned"),
        QStringLiteral("Continue Reading"),
        QStringLiteral("Active Work"),
        QStringLiteral("Clinical Essentials"),
        QStringLiteral("MD Project Review Set"),
        QStringLiteral("Core MND / ALS"),
        QStringLiteral("Diagnosis & Criteria"),
        QStringLiteral("Biomarkers & Neurofilament"),
        QStringLiteral("Neurophysiology / Hyperexcitability"),
        QStringLiteral("Trials & Treatment"),
        QStringLiteral("Mechanisms & Metabolism"),
        QStringLiteral("Cognitive / FTD"),
        QStringLiteral("Imaging & Networks"),
        QStringLiteral("Epidemiology / Natural History"),
        QStringLiteral("Care & Respiratory"),
        QStringLiteral("MND Project"),
        QStringLiteral("MND / ALS"),
        QStringLiteral("Neuro / MND"),
        QStringLiteral("Clinical Rotations"),
        QStringLiteral("Paeds Rotation"),
        QStringLiteral("OBGYN Rotation"),
        QStringLiteral("Psychiatry"),
        QStringLiteral("Child & Adolescent Psychiatry"),
        QStringLiteral("Mood, Anxiety & Trauma"),
        QStringLiteral("Psychosis & Bipolar"),
        QStringLiteral("Substance Use"),
        QStringLiteral("Beyond Bayes / Highdimensional"),
        QStringLiteral("Peer Reviews"),
        QStringLiteral("Fiction"),
        QStringLiteral("Politics"),
        QStringLiteral("Anthropology"),
        QStringLiteral("Non-fiction"),
        QStringLiteral("Biography & History"),
        QStringLiteral("Clinical Guidelines"),
        QStringLiteral("Reviews & Guidelines"),
        QStringLiteral("Reviews & Evidence Synthesis"),
        QStringLiteral("Pathology / Pharmacology / Physiology"),
        QStringLiteral("Methods & Statistics"),
        QStringLiteral("Novelty / Adjacent Ideas"),
        QStringLiteral("Anatomy"),
        QStringLiteral("Neuroscience"),
        QStringLiteral("Medicine & Clinical"),
        QStringLiteral("Medicine"),
        QStringLiteral("Patient Safety & Systems"),
        QStringLiteral("Systems & Theory"),
        QStringLiteral("Highly Cited"),
        QStringLiteral("Recent Additions"),
        QStringLiteral("Other Papers"),
        QStringLiteral("Less Relevant"),
        QStringLiteral("Textbooks"),
        QStringLiteral("Books"),
        QStringLiteral("Neuroscience & Mind"),
        QStringLiteral("Social Theory"),
        QStringLiteral("Other Textbooks"),
        QStringLiteral("Other Medical Textbooks"),
        QStringLiteral("Other Books"),
        QStringLiteral("Guidelines & Evidence"),
        QStringLiteral("Peer Reviews"),
        QStringLiteral("Reviews"),
        QStringLiteral("Studies"),
        QStringLiteral("Papers"),
        QStringLiteral("LocalEvidence"),
        QStringLiteral("BioSystems / Highdimensional"),
        QStringLiteral("General Research"),
        QStringLiteral("Open Access"),
        QStringLiteral("Acquired PDFs"),
        QStringLiteral("Harvests"),
        QStringLiteral("Recovered"),
        QStringLiteral("Unknown Source"),
    };
    const int index = order.indexOf(section);
    return index >= 0 ? index : 1000;
}

static QString readNextSectionFor(const QModelIndex &index, const QString &text, const QString &source, const QString &journal)
{
    if (index.data(PaperLibraryModel::PinnedRole).toBool()) {
        return QStringLiteral("Pinned");
    }
    if (index.data(PaperLibraryModel::AccessCountRole).toInt() > 0) {
        return QStringLiteral("Continue Reading");
    }
    if (source == QLatin1String("md-project-review-set")) {
        return QStringLiteral("MD Project Review Set");
    }
    if (recordMatchesMnd(text)) {
        return QStringLiteral("MND Project");
    }
    if (recordMatchesPsychiatry(text)) {
        return QStringLiteral("Psychiatry");
    }
    if (recordMatchesPaediatrics(text)) {
        return QStringLiteral("Paeds Rotation");
    }
    if (recordMatchesObgyn(text)) {
        return QStringLiteral("OBGYN Rotation");
    }
    if (recordMatchesBeyondBayes(text, source, journal)) {
        return QStringLiteral("Beyond Bayes / Highdimensional");
    }
    if (recordMatchesPeerReview(text, source)) {
        return QStringLiteral("Peer Reviews");
    }
    if (recordMatchesFiction(text)) {
        return QStringLiteral("Fiction");
    }
    if (recordMatchesPolitics(text)) {
        return QStringLiteral("Politics");
    }
    if (recordMatchesAnthropology(text)) {
        return QStringLiteral("Anthropology");
    }
    const QString kind = publicationKindFor(text, source, journal);
    if (kind == QLatin1String("Guidelines & Evidence") || kind == QLatin1String("Reviews")) {
        return QStringLiteral("Reviews & Guidelines");
    }
    if (projectBucketFor(text, source, journal) == QLatin1String("Methods & Statistics")) {
        return QStringLiteral("Methods & Statistics");
    }
    if (index.data(PaperLibraryModel::CitedByCountRole).toInt() >= 100) {
        return QStringLiteral("Highly Cited");
    }
    if (index.data(PaperLibraryModel::AddedRole).toString() >= QLatin1String("2026-06")) {
        return QStringLiteral("Recent Additions");
    }
    return QStringLiteral("Other Papers");
}

static QString medicineReadNextSectionFor(const QModelIndex &index, const QString &text)
{
    if (index.data(PaperLibraryModel::PinnedRole).toBool()) {
        return QStringLiteral("Pinned");
    }
    if (index.data(PaperLibraryModel::AccessCountRole).toInt() > 0) {
        return QStringLiteral("Continue Reading");
    }
    if (recordMatchesClinicalEssentials(text)) {
        return QStringLiteral("Clinical Essentials");
    }
    if (recordMatchesNeuroMedicine(text)) {
        return QStringLiteral("Neuro / MND");
    }
    if (recordMatchesPaediatrics(text)) {
        return QStringLiteral("Paeds Rotation");
    }
    if (recordMatchesObgyn(text)) {
        return QStringLiteral("OBGYN Rotation");
    }
    if (recordMatchesPsychiatry(text)) {
        return QStringLiteral("Psychiatry");
    }
    if (recordMatchesMedicalCoreScience(text)) {
        return QStringLiteral("Pathology / Pharmacology / Physiology");
    }
    if (recordMatchesMedicalAnatomy(text)) {
        return QStringLiteral("Anatomy");
    }
    if (recordMatchesPatientSafety(text)) {
        return QStringLiteral("Patient Safety & Systems");
    }
    return QStringLiteral("Other Medical Textbooks");
}

QVariant PaperLibraryModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_records.count()) {
        return QVariant();
    }
    const Record &record = m_records.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
        return record.title.isEmpty() ? record.slug : record.title;
    case Qt::ToolTipRole:
        return joinNonEmpty({record.doi, record.slug, record.source});
    case DetailRole:
        return joinNonEmpty({record.authors, record.year, record.journal});
    case SlugRole:
        return record.slug;
    case DoiRole:
        return record.doi;
    case SourceRole:
        return record.source;
    case AuthorsRole:
        return record.authors;
    case YearRole:
        return record.year;
    case JournalRole:
        return record.journal;
    case AddedRole:
        return record.addedTs;
    case LastAccessedRole:
        return record.lastAccessed;
    case AccessCountRole:
        return record.accessCount;
    case PinnedRole:
        return record.pinned;
    case CitedByCountRole:
        return record.citedByCount;
    case RelatedCountRole:
        return record.relatedCount;
    case GenreRole:
        return record.genre;
    case RecordKindRole:
        return record.recordKind;
    case DescriptionRole:
        return record.description;
    case TopicsRole:
        return record.topics;
    case ReadingLevelRole:
        return record.readingLevel;
    case SubgenreRole:
        return record.subgenre;
    case HaystackRole:
        return record.haystack;
    case MissingRole:
        return record.availability == Missing;
    case ResolvedPathRole:
        return record.pdfPath;
    }
    return QVariant();
}

QString PaperLibraryModel::resolvePdfPath(int row) const
{
    if (row < 0 || row >= m_records.count()) {
        return QString();
    }
    const Record &record = m_records.at(row);
    // Re-checked against the disk at activation time: the corpus may have
    // restored or evicted the file since the catalog was loaded
    if (!record.pdfPath.isEmpty() && QFileInfo::exists(record.pdfPath)) {
        return record.pdfPath;
    }
    const QString derived = derivedPdfPath(m_corpusDir, record.slug);
    return QFileInfo::exists(derived) ? derived : QString();
}

int PaperLibraryModel::rowForLookupSlug(const QString &slug) const
{
    const QString key = normalizedFocusLookupKey(slug);
    return key.isEmpty() ? -1 : m_rowsByLookupSlug.value(key, -1);
}

int PaperLibraryModel::rowForLookupDoi(const QString &doi) const
{
    const QString key = normalizedFocusLookupKey(doi);
    return key.isEmpty() ? -1 : m_rowsByLookupDoi.value(key, -1);
}

int PaperLibraryModel::rowForLookupPath(const QString &path) const
{
    const QString key = normalizedFocusPathLookupKey(path);
    return key.isEmpty() ? -1 : m_rowsByLookupPath.value(key, -1);
}

// A title reduced to lowercase letters and digits, so "The Power Broker" and "the power broker!"
// map to one key. Used only for the local-shelf -> corpus blurb cross-reference (best-effort).
static QString normalizedTitleLookupKey(const QString &title)
{
    QString key;
    key.reserve(title.size());
    for (const QChar ch : title) {
        if (ch.isLetterOrNumber()) {
            key.append(ch.toCaseFolded());
        }
    }
    return key;
}

int PaperLibraryModel::rowForLookupTitle(const QString &title) const
{
    const QString key = normalizedTitleLookupKey(title);
    return key.size() >= 6 ? m_rowsByLookupTitle.value(key, -1) : -1;
}

int PaperLibraryModel::rowForAnyTitle(const QString &title) const
{
    const QString key = normalizedTitleLookupKey(title);
    return key.size() >= 6 ? m_rowsByAnyTitle.value(key, -1) : -1;
}

PaperLibraryModel::CorpusHealth PaperLibraryModel::corpusHealth() const
{
    return m_corpusHealth;
}

bool PaperLibraryModel::hasFullTextSearchIndex() const
{
    // FTS index lives in search.db (backend build_search_index.py); older corpora embed it in catalog.db.
    const QString dbPath = resolveIndexDb(m_corpusDir, QStringLiteral("search.db"), QStringLiteral("paper_fts"));
    return !dbPath.isEmpty() && catalogDbHasTable(dbPath, QStringLiteral("paper_search_rows"));
}

bool PaperLibraryModel::hasSemanticGraph() const
{
    // Semantic graph lives in graph/graph.db (backend build_semantic_graph.py); older corpora embed it in catalog.db.
    return !resolveIndexDb(m_corpusDir, QStringLiteral("graph/graph.db"), QStringLiteral("related_edges")).isEmpty();
}

bool PaperLibraryModel::hasFreshFullTextSearchIndex() const
{
    return m_corpusHealth.searchFresh && hasFullTextSearchIndex();
}

bool PaperLibraryModel::hasFreshSemanticGraph() const
{
    return m_corpusHealth.graphFresh && hasSemanticGraph();
}

QList<int> PaperLibraryModel::fullTextSearchRows(const QString &queryText, int limit) const
{
    QList<int> rows;
    const QString match = ftsMatchQuery(queryText);
    const QString dbPath = resolveIndexDb(m_corpusDir, QStringLiteral("search.db"), QStringLiteral("paper_fts"));
    if (match.isEmpty() || limit <= 0 || dbPath.isEmpty()) {
        return rows;
    }

    const QString connectionName = nextPaperLibraryDbConnectionName(QStringLiteral("paperlibrary_fts"));
    QSet<int> seenRows;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        if (openReadOnlyDatabase(db, dbPath)) {
            QSqlQuery query(db);
            query.prepare(QStringLiteral(
                "SELECT sr.slug, bm25(paper_fts, 8.0, 5.0, 1.5, 3.0, 9.0, 1.0, 1.0) AS rank "
                "FROM paper_fts "
                "JOIN paper_search_rows sr ON sr.rowid = paper_fts.rowid "
                "WHERE paper_fts MATCH ? "
                "ORDER BY rank "
                "LIMIT ?"));
            query.addBindValue(match);
            query.addBindValue(qMax(1, limit * 2));
            if (query.exec()) {
                while (query.next() && rows.size() < limit) {
                    const int row = rowForLookupSlug(query.value(0).toString());
                    if (row < 0 || seenRows.contains(row)) {
                        continue;
                    }
                    seenRows.insert(row);
                    rows.append(row);
                }
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connectionName);
    return rows;
}

QList<int> PaperLibraryModel::relatedRowsForSlug(const QString &slug, int limit) const
{
    QList<int> rows;
    const QString normalizedSlug = slug.trimmed();
    const QString dbPath = resolveIndexDb(m_corpusDir, QStringLiteral("graph/graph.db"), QStringLiteral("related_edges"));
    if (normalizedSlug.isEmpty() || limit <= 0 || dbPath.isEmpty()) {
        return rows;
    }

    const QString connectionName = nextPaperLibraryDbConnectionName(QStringLiteral("paperlibrary_graph"));
    QSet<int> seenRows;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        if (openReadOnlyDatabase(db, dbPath)) {
            QSqlQuery query(db);
            query.prepare(QStringLiteral(
                "SELECT target_slug "
                "FROM related_edges "
                "WHERE kind='semantic' AND source_slug=? "
                "ORDER BY rank "
                "LIMIT ?"));
            query.addBindValue(normalizedSlug);
            query.addBindValue(qMax(1, limit * 2));
            if (query.exec()) {
                while (query.next() && rows.size() < limit) {
                    const int row = rowForLookupSlug(query.value(0).toString());
                    if (row < 0 || seenRows.contains(row)) {
                        continue;
                    }
                    seenRows.insert(row);
                    rows.append(row);
                }
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connectionName);
    return rows;
}

void PaperLibraryModel::rebuildLookupRows()
{
    m_rowsByLookupSlug.clear();
    m_rowsByLookupDoi.clear();
    m_rowsByLookupPath.clear();
    m_rowsByLookupTitle.clear();
    m_rowsByAnyTitle.clear();

    for (int row = 0; row < m_records.count(); ++row) {
        const Record &record = m_records.at(row);
        const QString titleKey = normalizedTitleLookupKey(record.title);
        // Only index rows that actually carry a librarian description, and keep the first such row
        // for a title -- a title-only match is fuzzy, so bias it toward a row with something to show.
        if (titleKey.size() >= 6 && !record.description.trimmed().isEmpty()
            && !m_rowsByLookupTitle.contains(titleKey)) {
            m_rowsByLookupTitle.insert(titleKey, row);
        }
        // A blurb-agnostic title index: any corpus row, first wins. Used to find the corpus twin of a
        // file-backed feed book when it is marked finished.
        if (titleKey.size() >= 6 && !m_rowsByAnyTitle.contains(titleKey)) {
            m_rowsByAnyTitle.insert(titleKey, row);
        }
        const QString slugKey = normalizedFocusLookupKey(record.slug);
        if (!slugKey.isEmpty() && !m_rowsByLookupSlug.contains(slugKey)) {
            m_rowsByLookupSlug.insert(slugKey, row);
        }
        const QString doiKey = normalizedFocusLookupKey(record.doi);
        if (!doiKey.isEmpty() && !m_rowsByLookupDoi.contains(doiKey)) {
            m_rowsByLookupDoi.insert(doiKey, row);
        }
        const QString pathKey = normalizedFocusPathLookupKey(record.pdfPath);
        if (!pathKey.isEmpty() && !m_rowsByLookupPath.contains(pathKey)) {
            m_rowsByLookupPath.insert(pathKey, row);
        }
        const QString derivedPathKey = normalizedFocusPathLookupKey(derivedPdfPath(m_corpusDir, record.slug));
        if (!derivedPathKey.isEmpty() && !m_rowsByLookupPath.contains(derivedPathKey)) {
            m_rowsByLookupPath.insert(derivedPathKey, row);
        }
    }
}

// A catalog title is "identifier-shaped" when it carries no real words — a bare
// PMID ("30850440"), a mangled DOI ("10-3109-21678421-2014-960176-052"), or an
// empty string. Such rows show a clean librarian norm_title instead when one exists.
static bool titleLooksLikeIdentifier(const QString &title)
{
    int letters = 0;
    for (const QChar ch : title) {
        if (ch.isLetter() && ++letters >= 3) {
            return false;
        }
    }
    return true;
}

QList<PaperLibraryModel::Record> PaperLibraryModel::parseCatalog(const QByteArray &jsonl)
{
    QList<Record> records;
    qsizetype lineStart = 0;
    while (lineStart < jsonl.size()) {
        qsizetype lineEnd = jsonl.indexOf('\n', lineStart);
        if (lineEnd < 0) {
            lineEnd = jsonl.size();
        }
        const QByteArray line = jsonl.mid(lineStart, lineEnd - lineStart).trimmed();
        lineStart = lineEnd + 1;
        if (line.isEmpty()) {
            continue;
        }
        const QJsonDocument document = QJsonDocument::fromJson(line);
        if (!document.isObject()) {
            continue; // a malformed line never takes the catalog down
        }
        const QJsonObject object = document.object();

        Record record;
        record.slug = object.value(QLatin1String("slug")).toString();
        record.doi = object.value(QLatin1String("doi")).toString();
        record.pmid = object.value(QLatin1String("pmid")).toString();
        record.citeKey = object.value(QLatin1String("cite_key")).toString();
        const QString rawTitle = object.value(QLatin1String("title")).toString();
        record.authors = object.value(QLatin1String("authors")).toString();
        record.year = object.value(QLatin1String("year")).toString();
        record.journal = object.value(QLatin1String("journal")).toString();
        record.source = object.value(QLatin1String("source")).toString();
        record.genre = object.value(QLatin1String("genre")).toString().trimmed();
        record.recordKind = object.value(QLatin1String("record_kind")).toString().trimmed();
        record.description = object.value(QLatin1String("description")).toString().trimmed();
        record.topics = jsonStringList(object.value(QLatin1String("topics")));
        if (record.topics.isEmpty()) {
            const QString singleTopic = object.value(QLatin1String("topics")).toString().trimmed();
            if (!singleTopic.isEmpty()) {
                record.topics.append(singleTopic);
            }
        }
        record.readingLevel = object.value(QLatin1String("reading_level")).toString().trimmed();
        record.subgenre = object.value(QLatin1String("subgenre")).toString().trimmed();
        // Librarian-cleaned strings; authoritative fallbacks when the raw catalog values
        // are empty or (for titles) a bare identifier. Only confident records are exported.
        const QString normTitle = object.value(QLatin1String("norm_title")).toString().trimmed();
        const QString normAuthors = object.value(QLatin1String("norm_authors")).toString().trimmed();
        const ImportedBookMetadata cleanedMetadata = importedBookMetadataFromTitle(rawTitle, record.authors, record.year, record.source, record.journal);
        // Book titles in the raw catalog are libgen dumps — author + publisher + year + filename
        // ("Robert Shea, Robert Anton Wilson  The illuminatus!", "amari2016", "USYD Textbook ...
        // final 2024"). The librarian norm_title is the clean book title and, on audit, is better
        // in every differing case and never empty/worse — so for BOOK records prefer it outright.
        // Papers keep the conservative rule (norm_title only when the raw title is a bare
        // identifier), since crossref/pubmed paper titles are already clean. The raw title stays in
        // the search haystack below, so a user who remembers the messy string can still find the row.
        const bool isBookRecord = record.recordKind.compare(QLatin1String("book"), Qt::CaseInsensitive) == 0;
        record.title = (!normTitle.isEmpty() && (isBookRecord || titleLooksLikeIdentifier(cleanedMetadata.title)))
            ? normTitle
            : cleanedMetadata.title;
        record.authors = !cleanedMetadata.authors.isEmpty() ? cleanedMetadata.authors : normAuthors;
        record.year = cleanedMetadata.year;
        record.addedTs = object.value(QLatin1String("added_ts")).toString();
        record.bytes = static_cast<qint64>(object.value(QLatin1String("bytes")).toDouble());

        // Precomputed so the filter does one contains() per row; the query
        // side case-folds the same way. The raw title stays searchable (a user who
        // remembers a PMID/DOI can still find the row) even when we display norm_title.
        record.haystack = QStringList({record.title,
                                      rawTitle,
                                      record.authors,
                                      record.journal,
                                      record.year,
                                      record.citeKey,
                                      record.doi,
                                      record.slug,
                                      record.source,
                                      record.genre,
                                      record.recordKind,
                                      record.description,
                                      record.topics.join(QLatin1Char(' ')),
                                      record.readingLevel,
                                      record.subgenre})
                             .join(QLatin1Char('\n'))
                             .toCaseFolded();
        records.append(record);
    }
    return records;
}

void PaperLibraryModel::sortRecords(QList<Record> &records)
{
    // Useful-first for a corpus this size: what was added most recently on
    // top (added_ts is ISO-8601 in a fixed offset — string order is time
    // order), slug as a deterministic tiebreak
    std::stable_sort(records.begin(), records.end(), [](const Record &a, const Record &b) {
        if (a.addedTs != b.addedTs) {
            return a.addedTs > b.addedTs;
        }
        return a.slug < b.slug;
    });
}

struct CatalogDbRow {
    QString pdfPath;
    QString lastAccessed;
    int accessCount = 0;
    bool pinned = false;
    int citedByCount = -1;
    bool evicted = false;
};

/**
 * The pdf_path/pdf_evicted columns of catalog.db, keyed by slug. The connection
 * is strictly read-only but WAL-aware so committed harvester changes that have
 * not yet been checkpointed remain visible to the app.
 */
static QHash<QString, CatalogDbRow> readCatalogDb(const QString &dbPath, bool *ok)
{
    *ok = false;
    QHash<QString, CatalogDbRow> rows;
    if (!QFileInfo::exists(dbPath) || !QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
        return rows;
    }

    const QString connectionName = nextPaperLibraryDbConnectionName(QStringLiteral("paperlibrary"));
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        if (openReadOnlyDatabase(db, dbPath)) {
            QSqlQuery query(db);
            if (query.exec(QStringLiteral("SELECT slug, pdf_path, pdf_evicted, last_accessed, access_count, pinned, cited_by_count FROM papers"))) {
                *ok = true;
                while (query.next()) {
                    CatalogDbRow row;
                    row.pdfPath = query.value(1).toString();
                    row.evicted = query.value(2).toInt() != 0;
                    row.lastAccessed = query.value(3).toString();
                    row.accessCount = query.value(4).toInt();
                    row.pinned = query.value(5).toInt() != 0;
                    row.citedByCount = query.value(6).isNull() ? -1 : query.value(6).toInt();
                    rows.insert(query.value(0).toString(), row);
                }
            }
            db.close();
        }
    } // db and query must be gone before removeDatabase()
    QSqlDatabase::removeDatabase(connectionName);
    return rows;
}

static QHash<QString, int> readRelatedCountsFromCatalogDb(const QString &dbPath, bool *ok)
{
    *ok = false;
    QHash<QString, int> counts;
    if (!QFileInfo::exists(dbPath) || !QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
        return counts;
    }

    const QString connectionName = nextPaperLibraryDbConnectionName(QStringLiteral("paperlibrary_graph_counts"));
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        if (openReadOnlyDatabase(db, dbPath)) {
            QSqlQuery query(db);
            if (query.exec(QStringLiteral("SELECT source_slug, COUNT(*) FROM related_edges WHERE kind='semantic' GROUP BY source_slug"))) {
                *ok = true;
                while (query.next()) {
                    counts.insert(query.value(0).toString(), query.value(1).toInt());
                }
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connectionName);
    return counts;
}

void PaperLibraryModel::enrichRecords(QList<Record> &records, const QString &corpusDir)
{
    const QString dbPath = corpusDir + QStringLiteral("/catalog.db");
    bool dbOk = false;
    const QHash<QString, CatalogDbRow> dbRows = readCatalogDb(dbPath, &dbOk);
    bool graphOk = false;
    // related_edges lives in graph/graph.db now (falls back to catalog.db for older corpora).
    const QString graphDbPath = resolveIndexDb(corpusDir, QStringLiteral("graph/graph.db"), QStringLiteral("related_edges"));
    const QHash<QString, int> relatedCounts = readRelatedCountsFromCatalogDb(graphDbPath, &graphOk);

    for (Record &record : records) {
        if (graphOk) {
            record.relatedCount = relatedCounts.value(record.slug, 0);
        }
        if (dbOk) {
            const CatalogDbRow dbRow = dbRows.value(record.slug);
            record.lastAccessed = dbRow.lastAccessed;
            record.accessCount = dbRow.accessCount;
            record.pinned = dbRow.pinned;
            record.citedByCount = dbRow.citedByCount;
            if (dbRow.evicted) {
                // The corpus evicted this PDF; the viewer never re-fetches
                record.availability = Missing;
                continue;
            }
            if (!dbRow.pdfPath.isEmpty() && QFileInfo::exists(dbRow.pdfPath)) {
                record.pdfPath = dbRow.pdfPath;
                record.availability = Available;
                continue;
            }
        }
        const QString derived = derivedPdfPath(corpusDir, record.slug);
        if (QFileInfo::exists(derived)) {
            record.pdfPath = derived;
            record.availability = Available;
        } else {
            // With a database that answered, "nothing resolves" is a fact;
            // without one it is only ignorance — don't grey what may exist
            record.availability = dbOk ? Missing : Unknown;
        }
    }
}

void PaperLibraryFilterModel::setQuery(const QString &query)
{
    const QString folded = query.trimmed().toCaseFolded();
    if (m_query == folded) {
        return;
    }
    m_query = folded;
    invalidateRowsFilter();
}

void PaperLibraryFilterModel::setSmartFilter(SmartFilter filter)
{
    if (m_smartFilter == filter) {
        return;
    }
    m_smartFilter = filter;
    invalidateRowsFilter();
}

bool PaperLibraryFilterModel::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    const QModelIndex index = sourceModel()->index(sourceRow, 0, sourceParent);
    const QString haystack = index.data(PaperLibraryModel::HaystackRole).toString();
    const QString source = index.data(PaperLibraryModel::SourceRole).toString().toCaseFolded();
    const QString smartText = haystack + QLatin1Char('\n') + source;

    switch (m_smartFilter) {
    case Textbooks:
        if (!recordMatchesTextbook(smartText, source)) {
            return false;
        }
        break;
    case Mnd:
        if (!recordMatchesMnd(smartText)) {
            return false;
        }
        break;
    case All:
        break;
    }
    // The haystack is stored case-folded, so this stays a plain (fast)
    // case-sensitive scan across all ~18k rows per keystroke
    return m_query.isEmpty() || haystack.contains(m_query);
}

static QString mndTopicSectionFor(const QString &text)
{
    if (containsAnyNeedle(text,
                          {QStringLiteral("biomarker"),
                           QStringLiteral("neurofilament"),
                           QStringLiteral(" nfl "),
                           QStringLiteral("nf-l"),
                           QStringLiteral("light chain"),
                           QStringLiteral("csf"),
                           QStringLiteral("serum"),
                           QStringLiteral("plasma"),
                           QStringLiteral("chitinase"),
                           QStringLiteral("chi3l1")})) {
        return QStringLiteral("Biomarkers & Neurofilament");
    }
    if (containsAnyNeedle(text,
                          {QStringLiteral("criteria"),
                           QStringLiteral("awaji"),
                           QStringLiteral("el escorial"),
                           QStringLiteral("gold coast"),
                           QStringLiteral("mimic")})) {
        return QStringLiteral("Diagnosis & Criteria");
    }
    if (containsAnyNeedle(text,
                          {QStringLiteral("cortical hyperexcitability"),
                           QStringLiteral("hyperexcitability"),
                           QStringLiteral("threshold tracking"),
                           QStringLiteral("electrodiagnos"),
                           QStringLiteral("electromyography"),
                           QStringLiteral(" emg "),
                           QStringLiteral("nerve conduction"),
                           QStringLiteral("split-hand"),
                           QStringLiteral("transcranial magnetic stimulation"),
                           QStringLiteral(" tms "),
                           QStringLiteral("motor cortex"),
                           QStringLiteral("beta-band"),
                           QStringLiteral("intermuscular"),
                           QStringLiteral("excitability")})) {
        return QStringLiteral("Neurophysiology / Hyperexcitability");
    }
    if (containsAnyNeedle(text, {QStringLiteral("diagnos")})) {
        return QStringLiteral("Diagnosis & Criteria");
    }
    if (containsAnyNeedle(text,
                          {QStringLiteral("treatment"),
                           QStringLiteral("therapy"),
                           QStringLiteral("trial"),
                           QStringLiteral("riluzole"),
                           QStringLiteral("edaravone"),
                           QStringLiteral("tofersen"),
                           QStringLiteral("antisense"),
                           QStringLiteral("ceftriaxone")})) {
        return QStringLiteral("Trials & Treatment");
    }
    if (containsAnyNeedle(text,
                          {QStringLiteral("metabolic"),
                           QStringLiteral("bioenergetic"),
                           QStringLiteral("mitochond"),
                           QStringLiteral("energy metabolism"),
                           QStringLiteral("sirt3"),
                           QStringLiteral("excitotoxic"),
                           QStringLiteral("c9orf72"),
                           QStringLiteral("sod1"),
                           QStringLiteral("tdp-43"),
                           QStringLiteral("genetic"),
                           QStringLiteral("mutation"),
                           QStringLiteral("pathology"),
                           QStringLiteral("mechanism")})) {
        return QStringLiteral("Mechanisms & Metabolism");
    }
    if (containsAnyNeedle(text, {QStringLiteral("cognitive"), QStringLiteral("frontotemporal"), QStringLiteral(" ftd "), QStringLiteral("executive"), QStringLiteral("behaviour"), QStringLiteral("behavior")})) {
        return QStringLiteral("Cognitive / FTD");
    }
    if (containsAnyNeedle(text, {QStringLiteral("imaging"), QStringLiteral("network"), QStringLiteral("structural brain"), QStringLiteral("diffusion"), QStringLiteral(" mri "), QStringLiteral("connectivity")})) {
        return QStringLiteral("Imaging & Networks");
    }
    if (containsAnyNeedle(text, {QStringLiteral("epidemiology"), QStringLiteral("incidence"), QStringLiteral("prevalence"), QStringLiteral("risk factor"), QStringLiteral("natural history"), QStringLiteral("preclinical"), QStringLiteral("survival"), QStringLiteral("prognosis"), QStringLiteral("phenotype"), QStringLiteral("cohort")})) {
        return QStringLiteral("Epidemiology / Natural History");
    }
    if (containsAnyNeedle(text, {QStringLiteral("care"), QStringLiteral("management"), QStringLiteral("respiratory"), QStringLiteral("feeding"), QStringLiteral("nutrition"), QStringLiteral("end-of-life")})) {
        return QStringLiteral("Care & Respiratory");
    }
    return QStringLiteral("Core MND / ALS");
}

static QString textbookTopicSectionFor(const QString &text)
{
    if (containsAnyNeedle(text, {QStringLiteral("anatomy"), QStringLiteral("physiology"), QStringLiteral("pathology"), QStringLiteral("pharmacology"), QStringLiteral("medicine"), QStringLiteral("clinical")})) {
        return QStringLiteral("Medicine & Clinical");
    }
    if (containsAnyNeedle(text, {QStringLiteral("neuroscience"), QStringLiteral("neurology"), QStringLiteral("neural"), QStringLiteral("brain"), QStringLiteral("cortex")})) {
        return QStringLiteral("Neuroscience");
    }
    if (containsAnyNeedle(text, {QStringLiteral("statistics"), QStringLiteral("epidemiology"), QStringLiteral("monte carlo"), QStringLiteral("bayesian"), QStringLiteral("information theory"), QStringLiteral("information geometry")})) {
        return QStringLiteral("Methods & Statistics");
    }
    if (containsAnyNeedle(text, {QStringLiteral("systems"), QStringLiteral("dynamics"), QStringLiteral("control"), QStringLiteral("complexity"), QStringLiteral("synergetics")})) {
        return QStringLiteral("Systems & Theory");
    }
    return QStringLiteral("Other Textbooks");
}

static QString bookTopicSectionFor(const QString &text)
{
    if (recordMatchesTextbook(text, QStringLiteral("book:pdf"))) {
        return QStringLiteral("Textbooks");
    }
    if (containsAnyNeedle(text, {QStringLiteral("medicine"), QStringLiteral("clinical"), QStringLiteral("anatomy"), QStringLiteral("physiology"), QStringLiteral("pathology")})) {
        return QStringLiteral("Medicine");
    }
    if (containsAnyNeedle(text, {QStringLiteral("neuroscience"), QStringLiteral("brain"), QStringLiteral("cortex"), QStringLiteral("mind")})) {
        return QStringLiteral("Neuroscience & Mind");
    }
    if (containsAnyNeedle(text, {QStringLiteral("systems"), QStringLiteral("dynamics"), QStringLiteral("information"), QStringLiteral("control"), QStringLiteral("anticipatory")})) {
        return QStringLiteral("Systems & Theory");
    }
    if (containsAnyNeedle(text, {QStringLiteral("graeber"), QStringLiteral("anthropolog"), QStringLiteral("politics"), QStringLiteral("state"), QStringLiteral("society")})) {
        return QStringLiteral("Social Theory");
    }
    return QStringLiteral("Other Books");
}

static QString psychiatryTopicSectionFor(const QString &text)
{
    if (containsAnyNeedle(text, {QStringLiteral("child"), QStringLiteral("adolescent"), QStringLiteral("developmental"), QStringLiteral("adhd"), QStringLiteral("autism")})) {
        return QStringLiteral("Child & Adolescent Psychiatry");
    }
    if (containsAnyNeedle(text, {QStringLiteral("depression"), QStringLiteral("anxiety"), QStringLiteral("trauma"), QStringLiteral("ptsd"), QStringLiteral("suicide")})) {
        return QStringLiteral("Mood, Anxiety & Trauma");
    }
    if (containsAnyNeedle(text, {QStringLiteral("psychosis"), QStringLiteral("schizophrenia"), QStringLiteral("bipolar"), QStringLiteral("mania")})) {
        return QStringLiteral("Psychosis & Bipolar");
    }
    if (containsAnyNeedle(text, {QStringLiteral("substance use"), QStringLiteral("addiction"), QStringLiteral("alcohol"), QStringLiteral("opioid"), QStringLiteral("stimulant")})) {
        return QStringLiteral("Substance Use");
    }
    return QStringLiteral("Psychiatry");
}

static QString topicBucketFor(const QString &text, const QString &source, const QString &journal)
{
    if (recordMatchesMnd(text)) {
        return QStringLiteral("MND / ALS");
    }
    if (recordMatchesPsychiatry(text)) {
        return psychiatryTopicSectionFor(text);
    }
    if (recordMatchesFiction(text)) {
        return QStringLiteral("Fiction");
    }
    if (recordMatchesPaediatrics(text)) {
        return QStringLiteral("Paediatrics");
    }
    if (recordMatchesObgyn(text)) {
        return QStringLiteral("Obstetrics & Gynaecology");
    }
    if (recordMatchesBeyondBayes(text, source, journal)) {
        return QStringLiteral("Beyond Bayes / Highdimensional");
    }
    if (recordMatchesPeerReview(text, source)) {
        return QStringLiteral("Peer Reviews");
    }
    if (recordMatchesPolitics(text)) {
        return QStringLiteral("Politics");
    }
    if (recordMatchesAnthropology(text)) {
        return QStringLiteral("Anthropology");
    }
    if (containsAnyNeedle(text, {QStringLiteral("clinical practice guidelines"), QStringLiteral("guideline:"), QStringLiteral("recommendation statement")})) {
        return QStringLiteral("Clinical Guidelines");
    }
    if (journal.contains(QStringLiteral("cochrane")) || containsAnyNeedle(text, {QStringLiteral("systematic review"), QStringLiteral("meta-analysis"), QStringLiteral("scoping review")})) {
        return QStringLiteral("Reviews & Evidence Synthesis");
    }
    if (containsAnyNeedle(text, {QStringLiteral("statistics"), QStringLiteral("epidemiology"), QStringLiteral("prediction model"), QStringLiteral("bayesian"), QStringLiteral("monte carlo"), QStringLiteral("information geometry")})) {
        return QStringLiteral("Methods & Statistics");
    }
    if (containsAnyNeedle(text, {QStringLiteral("neuroscience"), QStringLiteral("neurology"), QStringLiteral("brain"), QStringLiteral("cortex"), QStringLiteral("neuron")})) {
        return QStringLiteral("Neuroscience");
    }
    if (containsAnyNeedle(text, {QStringLiteral("systems"), QStringLiteral("dynamics"), QStringLiteral("control"), QStringLiteral("complexity"), QStringLiteral("bioelectric"), QStringLiteral("anticipatory")})) {
        return QStringLiteral("Systems & Theory");
    }
    if (containsAnyNeedle(text, {QStringLiteral("pathology"), QStringLiteral("physiology"), QStringLiteral("pharmacology"), QStringLiteral("anatomy"), QStringLiteral("medicine"), QStringLiteral("clinical")})) {
        return QStringLiteral("Medicine & Clinical");
    }
    if (source.startsWith(QLatin1String("book:")) || source == QLatin1String("aa_book")) {
        return bookTopicSectionFor(text);
    }
    return QStringLiteral("General Research");
}

static QString focusBucketFor(const QModelIndex &index, const QString &text, const QString &source, const QString &journal)
{
    if (index.data(PaperLibraryModel::PinnedRole).toBool()) {
        return QStringLiteral("Pinned");
    }
    if (index.data(PaperLibraryModel::AccessCountRole).toInt() > 0) {
        return QStringLiteral("Continue Reading");
    }
    if (source == QLatin1String("md-project-review-set") || recordMatchesMnd(text)) {
        const QString mndTopic = mndTopicSectionFor(text);
        return mndTopic == QLatin1String("Core MND / ALS") ? QStringLiteral("MND Project") : mndTopic;
    }
    if (recordMatchesPsychiatry(text)) {
        return QStringLiteral("Psychiatry");
    }
    if (recordMatchesPaediatrics(text)) {
        return QStringLiteral("Paeds Rotation");
    }
    if (recordMatchesObgyn(text)) {
        return QStringLiteral("OBGYN Rotation");
    }
    if (recordMatchesBeyondBayes(text, source, journal)) {
        return QStringLiteral("Beyond Bayes");
    }
    if (recordMatchesPeerReview(text, source)) {
        return QStringLiteral("Peer Review");
    }
    if (recordMatchesFiction(text)) {
        return QStringLiteral("Fiction");
    }
    if (recordMatchesPolitics(text)) {
        return QStringLiteral("Politics");
    }
    if (recordMatchesAnthropology(text)) {
        return QStringLiteral("Anthropology");
    }
    if (recordMatchesNonfiction(text, source, journal)) {
        return QStringLiteral("Non-fiction");
    }
    const QString project = projectBucketFor(text, source, journal);
    return project == QLatin1String("General Research") ? topicBucketFor(text, source, journal) : project;
}

static QString thumbnailSeedFor(const QModelIndex &index, const QString &text, const QString &source, const QString &journal)
{
    const QString focus = focusBucketFor(index, text, source, journal);
    if (!focus.isEmpty() && focus != QLatin1String("General Research")) {
        return focus;
    }
    return topicBucketFor(text, source, journal);
}

static QString relatedQueryFor(const QString &text, const QString &source, const QString &journal)
{
    const QString candidates[] = {
        QStringLiteral("amyotrophic lateral sclerosis"),
        QStringLiteral("motor neuron"),
        QStringLiteral("motor neurone"),
        QStringLiteral("neurofilament"),
        QStringLiteral("cortical hyperexcitability"),
        QStringLiteral("threshold tracking"),
        QStringLiteral("transcranial magnetic stimulation"),
        QStringLiteral("electrodiagnosis"),
        QStringLiteral("c9orf72"),
        QStringLiteral("sod1"),
        QStringLiteral("tdp-43"),
        QStringLiteral("riluzole"),
        QStringLiteral("tofersen"),
        QStringLiteral("frontotemporal dementia"),
        QStringLiteral("natural history"),
        QStringLiteral("bioenergetic"),
        QStringLiteral("systematic review"),
        QStringLiteral("clinical guideline"),
        QStringLiteral("statistics"),
        QStringLiteral("epidemiology"),
        QStringLiteral("prediction model"),
        QStringLiteral("bayesian"),
        QStringLiteral("information geometry"),
        QStringLiteral("neuroscience"),
        QStringLiteral("bioelectric"),
        QStringLiteral("anticipatory systems"),
        QStringLiteral("psychiatry"),
        QStringLiteral("mental health"),
        QStringLiteral("depression"),
        QStringLiteral("psychosis"),
        QStringLiteral("child adolescent psychiatry"),
        QStringLiteral("paediatrics"),
        QStringLiteral("pediatrics"),
        QStringLiteral("obstetrics"),
        QStringLiteral("gynaecology"),
        QStringLiteral("gynecology"),
        QStringLiteral("beyond bayes"),
        QStringLiteral("peer review"),
        QStringLiteral("fiction"),
        QStringLiteral("politics"),
        QStringLiteral("anthropology"),
        QStringLiteral("pathology"),
        QStringLiteral("physiology"),
    };
    for (const QString &candidate : candidates) {
        if (text.contains(candidate)) {
            return candidate;
        }
    }
    const QString bucket = topicBucketFor(text, source, journal);
    if (bucket != QLatin1String("General Research")) {
        return bucket;
    }
    return journal.isEmpty() ? sourceBucketFor(source) : journal;
}

static QString corpusMetadataHint(const QModelIndex &index)
{
    const QString year = index.data(PaperLibraryModel::YearRole).toString();
    QString journal = index.data(PaperLibraryModel::JournalRole).toString();
    if (journal == QLatin1String("(book)")) {
        journal.clear();
    }
    return joinNonEmpty({year == QLatin1String("None") ? QString() : year, journal});
}

static bool isBookPresentationShelf(PaperLibrarySectionedModel::SmartFilter filter)
{
    return filter == PaperLibrarySectionedModel::Books || filter == PaperLibrarySectionedModel::Fiction || filter == PaperLibrarySectionedModel::Nonfiction;
}

static QStringList splitCorpusCreators(QString creators)
{
    creators = creators.trimmed();
    creators.replace(QRegularExpression(QStringLiteral("\\s+&\\s+")), QStringLiteral("; "));
    creators.replace(QRegularExpression(QStringLiteral("\\s+and\\s+"), QRegularExpression::CaseInsensitiveOption), QStringLiteral("; "));
    QStringList parts = creators.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    if (parts.size() == 1 && creators.count(QLatin1Char(',')) >= 2) {
        parts = creators.split(QLatin1Char(','), Qt::SkipEmptyParts);
    }
    for (QString &part : parts) {
        part = part.simplified();
    }
    parts.removeAll(QString());
    return parts;
}

static QString corpusCreatorTagForBookShelf(const QModelIndex &index)
{
    const QStringList creators = splitCorpusCreators(index.data(PaperLibraryModel::AuthorsRole).toString());
    if (creators.isEmpty()) {
        return QString();
    }
    if (creators.size() == 1) {
        return creators.constFirst();
    }
    if (creators.size() == 2) {
        return creators.at(0) + QStringLiteral(" & ") + creators.at(1);
    }
    return creators.constFirst() + QStringLiteral(" et al.");
}

static QString bookShelfSubjectTagFor(PaperLibrarySectionedModel::SmartFilter filter, const QString &text, const QString &source, const QString &journal)
{
    if (recordMatchesGameOfThrones(text)) {
        return QStringLiteral("Fantasy");
    }
    if (filter == PaperLibrarySectionedModel::Fiction || recordMatchesFiction(text)) {
        if (containsAnyNeedle(text, {QStringLiteral("kim stanley robinson"), QStringLiteral("octavia butler"), QStringLiteral("red mars"), QStringLiteral("new york 2140")})) {
            return QStringLiteral("Science fiction");
        }
        return QStringLiteral("Fiction");
    }
    if (recordMatchesCaroLbj(text)) {
        return QStringLiteral("Political biography");
    }
    if (recordMatchesAnthropology(text)) {
        return QStringLiteral("Anthropology / social theory");
    }
    if (containsAnyNeedle(text, {QStringLiteral("world war"), QStringLiteral("went to war"), QStringLiteral("submarine warfare"), QStringLiteral("total war"), QStringLiteral("military history")})) {
        return QStringLiteral("Military history");
    }
    if (containsAnyNeedle(text, {QStringLiteral("sand county"), QStringLiteral("aldo leopold"), QStringLiteral("ecology"), QStringLiteral("conservation")})) {
        return QStringLiteral("Ecology / conservation");
    }
    if (containsAnyNeedle(text, {QStringLiteral("mountaineering"), QStringLiteral("wilderness medicine"), QStringLiteral("remote travel")})) {
        return QStringLiteral("Wilderness medicine");
    }
    if (containsAnyNeedle(text, {QStringLiteral("galen"), QStringLiteral("roman empire")})) {
        return QStringLiteral("History of medicine");
    }
    if (recordMatchesPolitics(text)) {
        return QStringLiteral("Politics and history");
    }
    if (recordMatchesMedicine(text)) {
        return QStringLiteral("Medicine");
    }
    const QString topic = bookTopicSectionFor(text);
    if (topic != QLatin1String("Other Books") && topic != QLatin1String("Textbooks")) {
        return topic;
    }
    const QString bucket = topicBucketFor(text, source, journal);
    if (bucket != QLatin1String("General Research") && bucket != QLatin1String("MND / ALS")) {
        return bucket;
    }
    return filter == PaperLibrarySectionedModel::Nonfiction ? QStringLiteral("Non-fiction") : QStringLiteral("Book");
}

static QStringList bookShelfTopicTagsFor(PaperLibrarySectionedModel::SmartFilter filter, const QModelIndex &index, const QString &text, const QString &source, const QString &journal)
{
    QStringList tags;
    const QString creator = corpusCreatorTagForBookShelf(index);
    if (!creator.isEmpty()) {
        tags.append(creator);
    }
    const QString subject = bookShelfSubjectTagFor(filter, text, source, journal);
    if (!subject.isEmpty() && !tags.contains(subject)) {
        tags.append(subject);
    }
    const QString year = index.data(PaperLibraryModel::YearRole).toString().trimmed();
    if (tags.size() < 2 && !year.isEmpty() && year != QLatin1String("None") && !tags.contains(year)) {
        tags.append(year);
    }
    return tags;
}

static QString corpusTileTooltip(const QString &title, const QStringList &lines)
{
    QStringList kept;
    const QString cleanTitle = title.trimmed();
    if (!cleanTitle.isEmpty()) {
        kept.append(cleanTitle);
    }
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty() && trimmed != cleanTitle && !kept.contains(trimmed)) {
            kept.append(trimmed);
        }
    }
    return kept.join(QLatin1Char('\n'));
}

static QString corpusPriorityHintFor(const QModelIndex &index, const QString &text, const QString &source, const QString &journal)
{
    if (index.data(PaperLibraryModel::PinnedRole).toBool()) {
        return QStringLiteral("Pinned");
    }
    if (index.data(PaperLibraryModel::AccessCountRole).toInt() > 0) {
        return QStringLiteral("Continue reading");
    }
    if (source == QLatin1String("md-project-review-set")) {
        return QStringLiteral("MD project review set");
    }
    if (recordMatchesBeyondBayes(text, source, journal)) {
        return QStringLiteral("Beyond Bayes revision");
    }
    if (recordMatchesPeerReview(text, source)) {
        return QStringLiteral("Peer review queue");
    }
    const int citedBy = index.data(PaperLibraryModel::CitedByCountRole).toInt();
    if (citedBy >= 100) {
        return QStringLiteral("Cited by %1").arg(citedBy);
    }
    if (index.data(PaperLibraryModel::MissingRole).toBool()) {
        return QStringLiteral("PDF not local");
    }
    if (index.data(PaperLibraryModel::AddedRole).toString() >= QLatin1String("2026-06")) {
        return QStringLiteral("Recently added");
    }
    return corpusMetadataHint(index);
}

static QString corpusShelfIntentFor(PaperLibrarySectionedModel::SmartFilter filter, const QModelIndex &index, const QString &text, const QString &source, const QString &journal)
{
    const QString focus = focusBucketFor(index, text, source, journal);
    const QString topic = topicBucketFor(text, source, journal);
    const QString priority = corpusPriorityHintFor(index, text, source, journal);
    if (priority == QLatin1String("Pinned") || priority == QLatin1String("Continue reading")) {
        return priority;
    }

    switch (filter) {
    case PaperLibrarySectionedModel::Papers:
        {
            const QString section = papersReadNextSectionFor(index, text, source, journal);
            if (section == QLatin1String("Active Work")) {
                return QStringLiteral("Active work reading");
            }
            if (section == QLatin1String("MND Project")) {
                return QStringLiteral("MD project adjacent paper");
            }
            if (section == QLatin1String("Clinical Rotations")) {
                return QStringLiteral("Rotation-relevant paper");
            }
            if (section == QLatin1String("Methods & Statistics")) {
                return QStringLiteral("Methods / stats paper");
            }
            if (section == QLatin1String("Novelty / Adjacent Ideas")) {
                return QStringLiteral("Novel adjacent idea");
            }
            if (section == QLatin1String("Reviews & Guidelines")) {
                return QStringLiteral("Review / guideline paper");
            }
            if (!focus.isEmpty() && focus != QLatin1String("General Research")) {
                return focus + QStringLiteral(" paper");
            }
            if (topic != QLatin1String("General Research")) {
                return topic + QStringLiteral(" paper");
            }
        }
        return QStringLiteral("Reading candidate");
    case PaperLibrarySectionedModel::Books:
        if (recordMatchesFiction(text)) {
            return QStringLiteral("Long-form fiction");
        }
        if (recordMatchesCaroLbj(text)) {
            return QStringLiteral("Political biography");
        }
        if (recordMatchesAnthropology(text)) {
            return QStringLiteral("Anthropology / social theory");
        }
        if (recordMatchesPolitics(text)) {
            return QStringLiteral("Politics and history");
        }
        return QStringLiteral("Long-form reading");
    case PaperLibrarySectionedModel::Textbooks:
        return topic == QLatin1String("General Research") ? QStringLiteral("Reference textbook") : topic + QStringLiteral(" reference");
    case PaperLibrarySectionedModel::Medicine:
        if (recordMatchesClinicalEssentials(text)) {
            return QStringLiteral("Clinical rotation reference");
        }
        if (recordMatchesNeuroMedicine(text)) {
            return QStringLiteral("Neuro / MND reference");
        }
        if (recordMatchesPaediatrics(text)) {
            return QStringLiteral("Paeds rotation textbook");
        }
        if (recordMatchesObgyn(text)) {
            return QStringLiteral("OBGYN rotation textbook");
        }
        if (recordMatchesPsychiatry(text)) {
            return QStringLiteral("Psychiatry training");
        }
        if (recordMatchesMedicalCoreScience(text)) {
            return QStringLiteral("Core medical science");
        }
        if (recordMatchesMedicalAnatomy(text)) {
            return QStringLiteral("Anatomy reference");
        }
        if (recordMatchesPatientSafety(text)) {
            return QStringLiteral("Systems / patient safety");
        }
        return QStringLiteral("Medical textbook");
    case PaperLibrarySectionedModel::Psychiatry:
        return QStringLiteral("Psychiatry training");
    case PaperLibrarySectionedModel::Mnd:
        if (source == QLatin1String("md-project-review-set")) {
            return QStringLiteral("MD project core paper");
        }
        {
            const QString mndTopic = mndTopicSectionFor(text);
            if (mndTopic == QLatin1String("Diagnosis & Criteria")) {
                return QStringLiteral("Diagnosis / criteria paper");
            }
            if (mndTopic == QLatin1String("Biomarkers & Neurofilament")) {
                return QStringLiteral("Biomarker candidate");
            }
            if (mndTopic == QLatin1String("Neurophysiology / Hyperexcitability")) {
                return QStringLiteral("Electrophysiology / excitability paper");
            }
            if (mndTopic == QLatin1String("Trials & Treatment")) {
                return QStringLiteral("Treatment / trial paper");
            }
            if (mndTopic == QLatin1String("Mechanisms & Metabolism")) {
                return QStringLiteral("Mechanism paper");
            }
            if (mndTopic == QLatin1String("Cognitive / FTD")) {
                return QStringLiteral("ALS-FTD paper");
            }
            if (mndTopic == QLatin1String("Imaging & Networks")) {
                return QStringLiteral("Imaging / network paper");
            }
            if (mndTopic == QLatin1String("Epidemiology / Natural History")) {
                return QStringLiteral("Natural history paper");
            }
            if (mndTopic == QLatin1String("Care & Respiratory")) {
                return QStringLiteral("Care pathway paper");
            }
            return topic == QLatin1String("MND / ALS") ? QStringLiteral("MND project paper") : topic;
        }
    case PaperLibrarySectionedModel::Work:
        if (recordMatchesBeyondBayes(text, source, journal)) {
            return QStringLiteral("Beyond Bayes work");
        }
        if (recordMatchesPeerReview(text, source)) {
            return QStringLiteral("Peer review work");
        }
        return QStringLiteral("Active work item");
    case PaperLibrarySectionedModel::Anthropology:
        return QStringLiteral("Anthropology / social theory");
    case PaperLibrarySectionedModel::Politics:
        return QStringLiteral("Politics and history");
    case PaperLibrarySectionedModel::Fiction:
        return recordMatchesGameOfThrones(text) ? QStringLiteral("A Song of Ice and Fire") : QStringLiteral("Fiction queue");
    case PaperLibrarySectionedModel::Nonfiction:
        if (recordMatchesCaroLbj(text)) {
            return QStringLiteral("Political biography");
        }
        if (recordMatchesAnthropology(text)) {
            return QStringLiteral("Anthropology / social theory");
        }
        if (recordMatchesPolitics(text)) {
            return QStringLiteral("Politics and history");
        }
        return QStringLiteral("Non-fiction reading");
    case PaperLibrarySectionedModel::Finished:
        return QStringLiteral("Finished reading");
    }
    return QStringLiteral("Reading candidate");
}

static QString corpusRelationHintFor(PaperLibrarySectionedModel::SmartFilter filter, const QModelIndex &index, const QString &text, const QString &source, const QString &journal)
{
    if (index.data(PaperLibraryModel::MissingRole).toBool()) {
        return QStringLiteral("Needs local PDF");
    }
    if (filter == PaperLibrarySectionedModel::Papers) {
        if (index.data(PaperLibraryModel::PinnedRole).toBool()) {
            return QStringLiteral("Manually promoted");
        }
        if (index.data(PaperLibraryModel::AccessCountRole).toInt() > 0) {
            return QStringLiteral("Opened before; keep warm");
        }
        const QString section = papersReadNextSectionFor(index, text, source, journal);
        if (section == QLatin1String("Active Work")) {
            return recordMatchesBeyondBayes(text, source, journal) ? QStringLiteral("Connected to Beyond Bayes / revisions") : QStringLiteral("Connected to current review work");
        }
        if (section == QLatin1String("MND Project")) {
            const QString mndTopic = mndTopicSectionFor(text);
            return mndTopic == QLatin1String("Core MND / ALS") ? QStringLiteral("Connected to the MD project") : QStringLiteral("MND angle: %1").arg(mndTopic);
        }
        if (section == QLatin1String("Clinical Rotations")) {
            if (recordMatchesPaediatrics(text)) {
                return QStringLiteral("Useful for paeds context");
            }
            if (recordMatchesObgyn(text)) {
                return QStringLiteral("Useful for OBGYN context");
            }
            return QStringLiteral("Useful for psychiatry context");
        }
        if (section == QLatin1String("Methods & Statistics")) {
            return QStringLiteral("Methodological leverage");
        }
        if (section == QLatin1String("Novelty / Adjacent Ideas")) {
            return QStringLiteral("Explore adjacent ideas");
        }
        if (section == QLatin1String("Reviews & Guidelines")) {
            return QStringLiteral("Good map of a topic");
        }
    }
    if (filter == PaperLibrarySectionedModel::Work) {
        if (recordMatchesBeyondBayes(text, source, journal)) {
            return QStringLiteral("Linked to Beyond Bayes");
        }
        if (recordMatchesPeerReview(text, source)) {
            return QStringLiteral("Linked to peer-review stack");
        }
    }
    if (filter == PaperLibrarySectionedModel::Medicine) {
        if (recordMatchesClinicalEssentials(text)) {
            return QStringLiteral("For clinical placement");
        }
        if (recordMatchesNeuroMedicine(text)) {
            return QStringLiteral("Bridge: medicine + neuro");
        }
        if (recordMatchesPsychiatry(text)) {
            return QStringLiteral("Bridge: medicine + psychiatry");
        }
        if (recordMatchesPaediatrics(text)) {
            return QStringLiteral("Rotation: paeds");
        }
        if (recordMatchesObgyn(text)) {
            return QStringLiteral("Rotation: OBGYN");
        }
        if (recordMatchesMedicalCoreScience(text)) {
            return QStringLiteral("Foundation: path/pharm/phys");
        }
        if (recordMatchesPatientSafety(text)) {
            return QStringLiteral("Lower priority for rotations");
        }
    }
    if (filter == PaperLibrarySectionedModel::Mnd && source == QLatin1String("md-project-review-set")) {
        return QStringLiteral("Linked to MD project review set");
    }
    if (filter == PaperLibrarySectionedModel::Mnd) {
        const QString mndTopic = mndTopicSectionFor(text);
        if (mndTopic == QLatin1String("Diagnosis & Criteria")) {
            return QStringLiteral("Use for diagnostic framing");
        }
        if (mndTopic == QLatin1String("Biomarkers & Neurofilament")) {
            return QStringLiteral("Use for biomarker evidence");
        }
        if (mndTopic == QLatin1String("Neurophysiology / Hyperexcitability")) {
            return QStringLiteral("Use for electrophysiology evidence");
        }
        if (mndTopic == QLatin1String("Trials & Treatment")) {
            return QStringLiteral("Use for therapy context");
        }
        if (mndTopic == QLatin1String("Mechanisms & Metabolism")) {
            return QStringLiteral("Use for pathogenesis model");
        }
        if (mndTopic == QLatin1String("Cognitive / FTD")) {
            return QStringLiteral("Use for cognitive overlap");
        }
        if (mndTopic == QLatin1String("Imaging & Networks")) {
            return QStringLiteral("Use for network evidence");
        }
        if (mndTopic == QLatin1String("Epidemiology / Natural History")) {
            return QStringLiteral("Use for cohort framing");
        }
        if (mndTopic == QLatin1String("Care & Respiratory")) {
            return QStringLiteral("Use for clinical pathway");
        }
    }
    if (filter == PaperLibrarySectionedModel::Fiction && recordMatchesGameOfThrones(text)) {
        return QStringLiteral("Continue the series");
    }
    if (isBookPresentationShelf(filter)) {
        return bookShelfSubjectTagFor(filter, text, source, journal);
    }
    const QString related = relatedQueryFor(text, source, journal);
    if (!related.isEmpty() && related != QLatin1String("General Research")) {
        return QStringLiteral("Related: %1").arg(related);
    }
    const QString metadata = corpusMetadataHint(index);
    return metadata.isEmpty() ? QStringLiteral("Find adjacent items") : metadata;
}

static int sourceRowShelfPriorityScore(const PaperLibraryModel *source, int row, PaperLibrarySectionedModel::SmartFilter filter)
{
    const QModelIndex index = source->index(row);
    const QString haystack = index.data(PaperLibraryModel::HaystackRole).toString();
    const QString sourceName = index.data(PaperLibraryModel::SourceRole).toString().toCaseFolded();
    const QString journal = index.data(PaperLibraryModel::JournalRole).toString().toCaseFolded();
    const QString text = haystack + QLatin1Char('\n') + sourceName;

    int score = 1000;
    if (index.data(PaperLibraryModel::PinnedRole).toBool()) {
        score -= 700;
    }
    if (index.data(PaperLibraryModel::AccessCountRole).toInt() > 0) {
        score -= 600;
    }
    if (!index.data(PaperLibraryModel::MissingRole).toBool()) {
        score -= 60;
    } else {
        score += 180;
    }

    switch (filter) {
    case PaperLibrarySectionedModel::Papers:
        if (recordMatchesBeyondBayes(text, sourceName, journal) || recordMatchesPeerReview(text, sourceName)) {
            score -= 380;
        }
        if (sourceName == QLatin1String("md-project-review-set") || recordMatchesMnd(text)) {
            score -= 360;
        }
        if (recordMatchesPsychiatry(text) || recordMatchesPaediatrics(text) || recordMatchesObgyn(text)) {
            score -= 290;
        }
        if (topicBucketFor(text, sourceName, journal) == QLatin1String("Methods & Statistics")) {
            score -= 250;
        }
        if (recordMatchesPapersNovelty(text, sourceName, journal)) {
            score -= 215;
        }
        {
            const QString kind = publicationKindFor(text, sourceName, journal);
            if (kind == QLatin1String("Guidelines & Evidence") || kind == QLatin1String("Reviews")) {
                score -= 140;
            }
        }
        break;
    case PaperLibrarySectionedModel::Books:
    case PaperLibrarySectionedModel::Nonfiction:
        if (recordMatchesCaroLbj(text)) {
            score -= 280;
        }
        if (recordMatchesAnthropology(text) || recordMatchesPolitics(text)) {
            score -= 220;
        }
        break;
    case PaperLibrarySectionedModel::Textbooks:
        if (recordMatchesMedicine(text) || topicBucketFor(text, sourceName, journal) == QLatin1String("Methods & Statistics")) {
            score -= 220;
        }
        break;
    case PaperLibrarySectionedModel::Medicine:
        if (recordMatchesClinicalEssentials(text)) {
            score -= 330;
        }
        if (recordMatchesNeuroMedicine(text)) {
            score -= 315;
        }
        if (recordMatchesPaediatrics(text) || recordMatchesObgyn(text)) {
            score -= 300;
        }
        if (recordMatchesPsychiatry(text)) {
            score -= 280;
        }
        if (recordMatchesMedicalCoreScience(text)) {
            score -= 240;
        }
        if (recordMatchesMedicalAnatomy(text)) {
            score -= 210;
        }
        if (recordMatchesPatientSafety(text)) {
            score -= 70;
        }
        break;
    case PaperLibrarySectionedModel::Mnd:
        if (sourceName == QLatin1String("md-project-review-set")) {
            score -= 440;
        }
        {
            const QString mndTopic = mndTopicSectionFor(text);
            if (mndTopic == QLatin1String("Biomarkers & Neurofilament") || mndTopic == QLatin1String("Diagnosis & Criteria")
                || mndTopic == QLatin1String("Neurophysiology / Hyperexcitability")) {
                score -= 300;
            } else if (mndTopic == QLatin1String("Trials & Treatment")) {
                score -= 260;
            } else if (mndTopic == QLatin1String("Mechanisms & Metabolism") || mndTopic == QLatin1String("Epidemiology / Natural History")) {
                score -= 210;
            } else if (mndTopic == QLatin1String("Cognitive / FTD") || mndTopic == QLatin1String("Imaging & Networks")) {
                score -= 180;
            } else if (mndTopic == QLatin1String("Core MND / ALS")) {
                score -= 150;
            }
        }
        break;
    case PaperLibrarySectionedModel::Work:
        if (recordMatchesBeyondBayes(text, sourceName, journal)) {
            score -= 300;
        }
        if (recordMatchesPeerReview(text, sourceName)) {
            score -= 260;
        }
        break;
    case PaperLibrarySectionedModel::Fiction:
        if (recordMatchesGameOfThrones(text)) {
            score -= 260;
        }
        break;
    case PaperLibrarySectionedModel::Psychiatry:
        score -= 220;
        break;
    case PaperLibrarySectionedModel::Anthropology:
    case PaperLibrarySectionedModel::Politics:
        score -= 180;
        break;
    case PaperLibrarySectionedModel::Finished:
        break;
    }

    const int citedBy = index.data(PaperLibraryModel::CitedByCountRole).toInt();
    if (citedBy > 0) {
        score -= qMin(citedBy, 250) / 5;
    }
    const QString added = index.data(PaperLibraryModel::AddedRole).toString();
    if (added >= QLatin1String("2026-06")) {
        score -= 80;
    } else if (added >= QLatin1String("2026")) {
        score -= 30;
    }
    return score;
}

struct FocusManifestEntry {
    QString id;
    QString title;
    QString path;
    QString kind;
    QString authors;
    QString year;
    QString journal;
    QString source;
    QString doi;
    QString reason;
    QString shelf;
    QString section;
    QString sectionLabel;
    QString focusLink;
    QString thumbnailPath;
    QString thumbnailSource;
    int order = -1;
    double score = 0.0;
};

struct FocusManifest {
    bool found = false;
    QList<FocusManifestEntry> entries;
};

static QString focusManifestShelfName(PaperLibrarySectionedModel::SmartFilter filter)
{
    switch (filter) {
    case PaperLibrarySectionedModel::Books:
    case PaperLibrarySectionedModel::Fiction:
    case PaperLibrarySectionedModel::Nonfiction:
        return QStringLiteral("Reading");
    case PaperLibrarySectionedModel::Work:
        return QStringLiteral("Work");
    case PaperLibrarySectionedModel::Mnd:
        return QStringLiteral("MND");
    case PaperLibrarySectionedModel::Medicine:
        return QStringLiteral("Medicine");
    default:
        return QString();
    }
}

static QString focusLookupKey(const QString &value)
{
    return normalizedFocusLookupKey(value);
}

static QString focusPathLookupKey(const QString &path)
{
    return normalizedFocusPathLookupKey(path);
}

static QString resolveFocusManifestPath(const QString &corpusDir, const QString &path)
{
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) {
        return QString();
    }
    if (QFileInfo(trimmed).isAbsolute()) {
        return QDir::cleanPath(trimmed);
    }
    return QDir::cleanPath(QDir(corpusDir).absoluteFilePath(trimmed));
}

static bool allDigits(const QString &text)
{
    return !text.isEmpty()
        && std::all_of(text.cbegin(), text.cend(), [](const QChar &character) {
               return character.isDigit();
           });
}

static QString titleCasedFocusWord(QString word)
{
    const QString lower = word.toCaseFolded();
    if (lower == QLatin1String("mnd") || lower == QLatin1String("als") || lower == QLatin1String("md") || lower == QLatin1String("fep")
        || lower == QLatin1String("obgyn")) {
        return lower.toUpper();
    }
    if (lower == QLatin1String("and") || lower == QLatin1String("or") || lower == QLatin1String("of") || lower == QLatin1String("for")
        || lower == QLatin1String("to") || lower == QLatin1String("in")) {
        return lower;
    }
    if (!word.isEmpty()) {
        word = lower;
        word[0] = word.at(0).toUpper();
    }
    return word;
}

static QString focusSectionLabel(QString section)
{
    section = section.trimmed();
    const int dash = section.indexOf(QLatin1Char('-'));
    if (dash > 0 && allDigits(section.left(dash))) {
        section = section.mid(dash + 1);
    }
    section.replace(QLatin1Char('-'), QLatin1Char(' '));
    section.replace(QLatin1Char('_'), QLatin1Char(' '));

    QStringList words = section.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (QString &word : words) {
        word = titleCasedFocusWord(word);
    }
    return words.join(QLatin1Char(' '));
}

static QString focusFallbackTitle(const QString &path, const QString &id)
{
    const QString base = QFileInfo(path).completeBaseName().trimmed();
    return cleanedDisplayTitle(base.isEmpty() ? id : base, path);
}

static QString focusDetail(const QString &authors, const QString &year, const QString &journal)
{
    return joinNonEmpty({authors, year, journal});
}

static QString focusThumbnailFileName(QString id)
{
    id = id.trimmed();
    id.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")), QStringLiteral("_"));
    id.replace(QRegularExpression(QStringLiteral("_+")), QStringLiteral("_"));
    id = id.trimmed();
    while (id.startsWith(QLatin1Char('_'))) {
        id.remove(0, 1);
    }
    while (id.endsWith(QLatin1Char('_'))) {
        id.chop(1);
    }
    return id;
}

static QString resolveFocusThumbnailPath(const QString &corpusDir, const QString &shelfName, const QString &rawPath)
{
    const QString trimmed = rawPath.trimmed();
    if (trimmed.isEmpty()) {
        return QString();
    }

    const QFileInfo directInfo(trimmed);
    if (directInfo.isAbsolute()) {
        return directInfo.exists() ? directInfo.absoluteFilePath() : QString();
    }

    const QStringList bases = {
        corpusDir + QStringLiteral("/focus/") + shelfName,
        corpusDir,
    };
    for (const QString &base : bases) {
        const QFileInfo candidate(QDir(base).filePath(trimmed));
        if (candidate.exists() && candidate.isFile()) {
            return candidate.absoluteFilePath();
        }
    }
    return QString();
}

static QString objectThumbnailPath(const QJsonObject &object)
{
    const QStringList keys = {
        QStringLiteral("thumbnail_path"),
        QStringLiteral("thumbnail"),
        QStringLiteral("image_path"),
        QStringLiteral("image"),
    };
    for (const QString &key : keys) {
        const QJsonValue value = object.value(key);
        if (value.isString()) {
            const QString path = value.toString().trimmed();
            if (!path.isEmpty()) {
                return path;
            }
        } else if (value.isObject()) {
            const QString path = value.toObject().value(QStringLiteral("path")).toString().trimmed();
            if (!path.isEmpty()) {
                return path;
            }
        }
    }
    return QString();
}

static QString objectThumbnailSource(const QJsonObject &object)
{
    const QString direct = object.value(QStringLiteral("thumbnail_source")).toString().trimmed();
    if (!direct.isEmpty()) {
        return direct;
    }

    const QStringList keys = {
        QStringLiteral("thumbnail"),
        QStringLiteral("image"),
    };
    for (const QString &key : keys) {
        const QJsonValue value = object.value(key);
        if (!value.isObject()) {
            continue;
        }
        const QString source = value.toObject().value(QStringLiteral("source")).toString().trimmed();
        if (!source.isEmpty()) {
            return source;
        }
    }
    return QString();
}

static QString inferredFocusThumbnailPath(const QString &corpusDir, const QString &shelfName, const QString &id)
{
    const QString fileName = focusThumbnailFileName(id);
    if (fileName.isEmpty()) {
        return QString();
    }
    const QStringList extensions = {
        QStringLiteral(".png"),
        QStringLiteral(".jpg"),
        QStringLiteral(".jpeg"),
        QStringLiteral(".webp"),
    };
    const QDir thumbnailDir(corpusDir + QStringLiteral("/focus/") + shelfName + QStringLiteral("/thumbnails"));
    for (const QString &extension : extensions) {
        const QFileInfo candidate(thumbnailDir.filePath(fileName + extension));
        if (candidate.exists() && candidate.isFile()) {
            return candidate.absoluteFilePath();
        }
    }
    return QString();
}

// AI-generated cover art (from the backend cover pipeline) for a book that has no real cover.
// Lives at <corpus>/covers-ai/<slug>.png. Preferred over a PDF render or the typographic card, but
// never over a genuine extracted cover.
static QString aiCoverPathFor(const QString &corpusDir, const QString &slug)
{
    if (corpusDir.isEmpty() || slug.isEmpty()) {
        return QString();
    }
    const QString path = corpusDir + QStringLiteral("/covers-ai/") + slug + QStringLiteral(".png");
    return QFileInfo::exists(path) ? path : QString();
}

static QString inferredCorpusThumbnailPath(const QString &corpusDir, const QString &id)
{
    const QString fileName = focusThumbnailFileName(id);
    if (fileName.isEmpty() || corpusDir.isEmpty()) {
        return QString();
    }
    const QStringList extensions = {
        QStringLiteral(".png"),
        QStringLiteral(".jpg"),
        QStringLiteral(".jpeg"),
        QStringLiteral(".webp"),
    };
    const QStringList directories = {
        corpusDir + QStringLiteral("/thumbnails"),
        corpusDir + QStringLiteral("/assets/thumbnails"),
    };
    for (const QString &directory : directories) {
        const QDir thumbnailDir(directory);
        for (const QString &extension : extensions) {
            const QFileInfo candidate(thumbnailDir.filePath(fileName + extension));
            if (candidate.exists() && candidate.isFile()) {
                return candidate.absoluteFilePath();
            }
        }
    }
    return QString();
}

static QString focusManifestThumbnailPath(const QString &corpusDir, const QString &shelfName, const QJsonObject &object, const QString &id)
{
    const QString explicitPath = resolveFocusThumbnailPath(corpusDir, shelfName, objectThumbnailPath(object));
    if (!explicitPath.isEmpty()) {
        return explicitPath;
    }
    return inferredFocusThumbnailPath(corpusDir, shelfName, id);
}

static QString focusReasonPrimary(const QString &reason)
{
    const QStringList parts = reason.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    return parts.isEmpty() ? reason.trimmed() : parts.constFirst().trimmed();
}

static QString focusReasonSecondary(const QString &reason)
{
    QStringList parts = reason.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    if (parts.size() <= 1) {
        return QString();
    }
    parts.removeFirst();
    for (QString &part : parts) {
        part = part.trimmed();
    }
    return parts.join(QStringLiteral(" · "));
}

static bool focusManifestEntryMatchesFilter(PaperLibrarySectionedModel::SmartFilter filter, const FocusManifestEntry &entry)
{
    const QString kind = entry.kind.toCaseFolded();
    const QString path = entry.path.toCaseFolded();
    const QString source = entry.source.toCaseFolded();
    const QString text = QStringList({entry.title, entry.reason, entry.section, entry.source, entry.kind}).join(QLatin1Char('\n')).toCaseFolded();

    switch (filter) {
    case PaperLibrarySectionedModel::Books:
        return kind == QLatin1String("epub") || path.endsWith(QLatin1String(".epub")) || source.contains(QLatin1String("book:epub"));
    case PaperLibrarySectionedModel::Fiction:
        if (containsAnyNeedle(text, {QStringLiteral("nonfiction"), QStringLiteral("non-fiction"), QStringLiteral("non fiction")})) {
            return false;
        }
        return containsAnyNeedle(text, {QStringLiteral("fiction-current"), QStringLiteral("current fiction"), QStringLiteral("game of thrones"), QStringLiteral("song of ice and fire")});
    case PaperLibrarySectionedModel::Nonfiction:
        return containsAnyNeedle(text,
                                 {QStringLiteral("nonfiction"),
                                  QStringLiteral("non-fiction"),
                                  QStringLiteral("non fiction"),
                                  QStringLiteral("anthropology"),
                                  QStringLiteral("graeber"),
                                  QStringLiteral("caro"),
                                  QStringLiteral("lbj"),
                                  QStringLiteral("politics"),
                                  QStringLiteral("history")});
    default:
        return true;
    }
}

static bool focusManifestUsesSectionOrder(PaperLibrarySectionedModel::SmartFilter filter)
{
    return filter == PaperLibrarySectionedModel::Mnd || filter == PaperLibrarySectionedModel::Work || filter == PaperLibrarySectionedModel::Medicine;
}

static FocusManifest loadFocusManifest(PaperLibrarySectionedModel::SmartFilter filter, const QString &corpusDir)
{
    FocusManifest manifest;
    const QString shelfName = focusManifestShelfName(filter);
    if (shelfName.isEmpty() || corpusDir.isEmpty()) {
        return manifest;
    }

    QFile file(corpusDir + QStringLiteral("/focus/") + shelfName + QStringLiteral("/manifest.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        return manifest;
    }
    manifest.found = true;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isArray()) {
        return manifest;
    }

    const QJsonArray array = document.array();
    manifest.entries.reserve(array.size());
    int order = 0;
    for (const QJsonValue &value : array) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject object = value.toObject();
        FocusManifestEntry entry;
        entry.id = object.value(QLatin1String("id")).toString().trimmed();
        entry.title = object.value(QLatin1String("title")).toString().trimmed();
        entry.path = resolveFocusManifestPath(corpusDir, object.value(QLatin1String("path")).toString());
        entry.kind = object.value(QLatin1String("kind")).toString().trimmed();
        entry.authors = object.value(QLatin1String("authors")).toString().trimmed();
        entry.year = object.value(QLatin1String("year")).toString().trimmed();
        entry.journal = object.value(QLatin1String("journal")).toString().trimmed();
        entry.source = object.value(QLatin1String("source")).toString().trimmed();
        entry.doi = object.value(QLatin1String("doi")).toString().trimmed();
        entry.reason = object.value(QLatin1String("reason")).toString().trimmed();
        entry.shelf = object.value(QLatin1String("shelf")).toString().trimmed();
        entry.section = object.value(QLatin1String("section")).toString().trimmed();
        entry.sectionLabel = focusSectionLabel(entry.section);
        entry.focusLink = object.value(QLatin1String("focus_link")).toString().trimmed();
        entry.thumbnailPath = focusManifestThumbnailPath(corpusDir, shelfName, object, entry.id);
        entry.thumbnailSource = objectThumbnailSource(object);
        entry.score = object.value(QLatin1String("score")).toDouble();
        entry.order = order++;
        if (entry.title.isEmpty()) {
            entry.title = focusFallbackTitle(entry.path, entry.id);
        } else {
            entry.title = cleanedDisplayTitle(entry.title, entry.path);
        }
        if (entry.sectionLabel.isEmpty()) {
            entry.sectionLabel = shelfName;
        }
        if (!focusManifestEntryMatchesFilter(filter, entry)) {
            continue;
        }
        if (!entry.title.isEmpty() || !entry.path.isEmpty() || !entry.id.isEmpty()) {
            manifest.entries.append(entry);
        }
    }
    if (focusManifestUsesSectionOrder(filter)) {
        std::stable_sort(manifest.entries.begin(), manifest.entries.end(), [](const FocusManifestEntry &left, const FocusManifestEntry &right) {
            const int sectionOrder = left.section.localeAwareCompare(right.section);
            if (sectionOrder != 0) {
                return sectionOrder < 0;
            }
            return left.order < right.order;
        });
    }
    return manifest;
}

static QSet<QString> slugSetFromConfig(const KConfigGroup &group, const char *key)
{
    QSet<QString> result;
    for (const QString &slug : group.readEntry(key, QStringList())) {
        if (!slug.isEmpty()) {
            result.insert(slug);
        }
    }
    return result;
}

// Section models are per shelf and per Library tab, but their feed preferences are one shared
// user setting. Keep a lightweight registry per config file so a mutation can update every live
// model immediately instead of leaving stale copies that later overwrite one another.
static QHash<QString, QSet<PaperLibrarySectionedModel *>> &sectionedModelsByConfig()
{
    static QHash<QString, QSet<PaperLibrarySectionedModel *>> models;
    return models;
}

PaperLibrarySectionedModel::PaperLibrarySectionedModel(QObject *parent)
    : QAbstractListModel(parent)
    , m_feedConfigPath(paperLibraryConfigFilePath())
{
    const KConfigGroup feed = paperLibraryConfigGroupForPath(m_feedConfigPath, QStringLiteral("CorpusFeed"));
    m_downrankedSlugs = slugSetFromConfig(feed, DOWNRANKED_SLUGS_KEY);
    m_finishedSlugs = slugSetFromConfig(feed, FINISHED_SLUGS_KEY);
    sectionedModelsByConfig()[m_feedConfigPath].insert(this);
    // m_finishedTitles is built once the source model is attached (setSourceModel).
}

PaperLibrarySectionedModel::~PaperLibrarySectionedModel()
{
    auto &models = sectionedModelsByConfig();
    auto it = models.find(m_feedConfigPath);
    if (it == models.end()) {
        return;
    }
    it->remove(this);
    if (it->isEmpty()) {
        models.erase(it);
    }
}

// Process-wide shelf classification cache, keyed by slug. The per-row heuristics (~10 regex sweeps
// over the whole corpus) are a pure function of a record's content, so their result is identical for
// a given slug across EVERY sectioned model and EVERY library tab. Each LibraryView builds its own
// PaperLibraryModel + ~11 sectioned models, so without this the sweep re-ran for every shelf of
// every new tab — the "open a new tab and click the subtabs -> multi-second freeze". UI-thread only,
// so a plain function-local static needs no locking. Cleared when a source model's data changes.
static QHash<QString, quint16> &sharedClassifyCache()
{
    static QHash<QString, quint16> cache;
    return cache;
}

void PaperLibrarySectionedModel::setSourceModel(PaperLibraryModel *model)
{
    if (m_source == model) {
        return;
    }
    if (m_source) {
        m_source->disconnect(this);
    }
    clearRowCache();
    m_classifyCache.clear();
    m_source = model;
    if (m_source) {
        // Source data changed -> BOTH the per-model row cache and the process-wide slug cache are
        // stale; drop them (unlike clearRowCache from downrank/finished/query, which must keep the
        // classification). Clearing the shared cache here (not on plain re-attach) is what keeps it
        // reusable across tabs while staying correct when the catalog is reloaded/edited.
        connect(m_source, &QAbstractItemModel::modelReset, this, [this]() {
            clearRowCache();
            m_classifyCache.clear();
            sharedClassifyCache().clear();
            rebuildFinishedTitles(); // titles come from the source; refresh when it changes
            rebuild();
        });
        connect(m_source, &QAbstractItemModel::dataChanged, this, [this]() {
            clearRowCache();
            m_classifyCache.clear();
            sharedClassifyCache().clear();
            rebuildFinishedTitles();
            rebuild();
        });
    }
    rebuildFinishedTitles();
    rebuild();
}

void PaperLibrarySectionedModel::setSmartFilter(SmartFilter filter)
{
    if (m_smartFilter == filter) {
        return;
    }
    m_smartFilter = filter;
    rebuild();
}

void PaperLibrarySectionedModel::setShelf(SmartFilter filter, SectionMode mode)
{
    if (m_smartFilter == filter && m_sectionMode == mode) {
        return;
    }
    m_smartFilter = filter;
    m_sectionMode = mode;
    rebuild();
}

void PaperLibrarySectionedModel::setSectionMode(SectionMode mode)
{
    if (m_sectionMode == mode) {
        return;
    }
    m_sectionMode = mode;
    rebuild();
}

PaperLibrarySectionedModel::SmartFilter PaperLibrarySectionedModel::smartFilter() const
{
    return m_smartFilter;
}

PaperLibrarySectionedModel::SectionMode PaperLibrarySectionedModel::sectionMode() const
{
    return m_sectionMode;
}

void PaperLibrarySectionedModel::setQuery(const QString &query)
{
    const QString folded = query.trimmed().toCaseFolded();
    if (m_query == folded) {
        return;
    }
    m_query = folded;
    rebuild();
}

void PaperLibrarySectionedModel::setExplicitSourceRows(const QList<int> &sourceRows, const QString &label, const QString &emptyText, bool bypassShelfFilter)
{
    if (m_explicitRowsActive && m_explicitSourceRows == sourceRows && m_explicitRowsLabel == label
        && m_explicitRowsEmptyText == emptyText && m_explicitBypassFilter == bypassShelfFilter) {
        return;
    }
    m_explicitRowsActive = true;
    m_explicitBypassFilter = bypassShelfFilter;
    m_explicitSourceRows = sourceRows;
    m_explicitRowsLabel = label.trimmed();
    m_explicitRowsEmptyText = emptyText.trimmed();
    rebuild();
}

void PaperLibrarySectionedModel::clearExplicitSourceRows()
{
    if (!m_explicitRowsActive) {
        return;
    }
    m_explicitRowsActive = false;
    m_explicitBypassFilter = false;
    m_explicitSourceRows.clear();
    m_explicitRowsLabel.clear();
    m_explicitRowsEmptyText.clear();
    rebuild();
}

bool PaperLibrarySectionedModel::hasExplicitSourceRows() const
{
    return m_explicitRowsActive;
}

int PaperLibrarySectionedModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.count();
}

bool PaperLibrarySectionedModel::canFetchMore(const QModelIndex &parent) const
{
    return !parent.isValid() && m_rows.count() < m_allRows.count();
}

void PaperLibrarySectionedModel::fetchMore(const QModelIndex &parent)
{
    if (!canFetchMore(parent)) {
        return;
    }

    const int first = m_rows.count();
    const int count = qMin(CorpusFetchBatchRows, m_allRows.count() - first);
    beginInsertRows(QModelIndex(), first, first + count - 1);
    for (int offset = 0; offset < count; ++offset) {
        m_rows.append(m_allRows.at(first + offset));
    }
    endInsertRows();
    rebuildPathIndex();
}

QVariant PaperLibrarySectionedModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.count()) {
        return QVariant();
    }
    const Row &row = m_rows.at(index.row());
    if (row.header) {
        switch (role) {
        case Qt::DisplayRole:
            return row.title;
        case SectionHeaderRole:
            return true;
        case PaperLibraryModel::MissingRole:
            return false;
        }
        return QVariant();
    }
    if (role == SectionHeaderRole) {
        return false;
    }
    if (role == SourceRowRole) {
        return row.sourceRow;
    }
    const bool focusRow = row.focusOrder >= 0;
    const QString focusDetailText = focusDetail(row.focusAuthors, row.focusYear, row.focusJournal);
    const QString focusPath = !row.focusPath.isEmpty() ? row.focusPath : (row.sourceRow >= 0 ? storedPathForSourceRow(row.sourceRow) : QString());
    if (role == PdfPathRole) {
        return focusPath;
    }
    if (role == ReadingProgressRole) {
        // Prefer what our own readers recorded for this exact file; fall back to Apple Books, which
        // knows this book by title even though it lives at a different path there.
        const double own = ReadingProgress::fractionForPath(focusPath);
        if (own >= 0.0) {
            return own;
        }
        return ReadingProgress::fractionForTitle(m_source->index(row.sourceRow >= 0 ? row.sourceRow : 0)
                                                     .data(Qt::DisplayRole).toString());
    }
    if (role == CoverPixmapRole) {
        return QVariant::fromValue(m_coverPixmaps.value(focusPath));
    }
    if (role == GeneratedCoverRole) {
        return m_generatedCoverPaths.contains(focusPath);
    }
    if (role == ThumbnailPathRole) {
        if (!row.focusThumbnailPath.isEmpty()) {
            return row.focusThumbnailPath; // a genuine extracted cover always wins
        }
        if (row.sourceRow >= 0 && m_source) {
            const QString slug = m_source->index(row.sourceRow).data(PaperLibraryModel::SlugRole).toString();
            const QString ai = aiCoverPathFor(m_source->corpusDir(), slug);
            if (!ai.isEmpty()) {
                return ai; // AI art beats a PDF render / typographic card for a coverless book
            }
            return inferredCorpusThumbnailPath(m_source->corpusDir(), slug);
        }
        return QString();
    }
    if (role == ThumbnailSourceRole) {
        if (!row.focusThumbnailPath.isEmpty()) {
            return row.focusThumbnailSource;
        }
        if (row.sourceRow >= 0 && m_source) {
            const QString slug = m_source->index(row.sourceRow).data(PaperLibraryModel::SlugRole).toString();
            if (!aiCoverPathFor(m_source->corpusDir(), slug).isEmpty()) {
                return QStringLiteral("paperlibrary-ai-generated");
            }
            if (!inferredCorpusThumbnailPath(m_source->corpusDir(), slug).isEmpty()) {
                return QStringLiteral("paperlibrary-corpus-thumbnail");
            }
        }
        return QString();
    }
    if (row.sourceRow < 0) {
        switch (role) {
        case Qt::DisplayRole:
            return row.title;
        case Qt::ToolTipRole:
            return corpusTileTooltip(row.title, {focusDetailText, row.focusSection, focusReasonPrimary(row.focusReason), focusReasonSecondary(row.focusReason)});
        case PaperLibraryModel::DetailRole:
            return focusDetailText;
        case PaperLibraryModel::SlugRole:
            return row.focusId;
        case PaperLibraryModel::DoiRole:
            return row.focusDoi;
        case PaperLibraryModel::SourceRole:
            return row.focusSource;
        case PaperLibraryModel::AuthorsRole:
            return row.focusAuthors;
        case PaperLibraryModel::YearRole:
            return row.focusYear;
        case PaperLibraryModel::JournalRole:
            return row.focusJournal;
        case PaperLibraryModel::MissingRole:
            return row.focusPath.isEmpty() || !QFileInfo::exists(row.focusPath);
        case PaperLibraryModel::ResolvedPathRole:
            return row.focusPath;
        case PaperLibraryModel::RelatedCountRole:
            return m_source && m_source->hasFreshSemanticGraph() ? 0 : -1;
        case KindRole:
            return row.focusKind.isEmpty() ? QStringLiteral("PDF") : row.focusKind.toUpper();
        case TopicTagsRole:
            return QStringList({row.focusSection, focusManifestShelfName(m_smartFilter)});
        case RelatedQueryRole:
            return row.focusSection;
        case FocusRole:
            return row.focusSection;
        case ThumbnailSeedRole:
            return row.focusSection.isEmpty() ? focusManifestShelfName(m_smartFilter) : row.focusSection;
        case ShelfIntentRole:
            return focusReasonPrimary(row.focusReason);
        case RelationHintRole:
            return focusReasonSecondary(row.focusReason);
        case PriorityHintRole:
            return row.focusSection;
        }
        return QVariant();
    }

    const QModelIndex sourceIndex = m_source ? m_source->index(row.sourceRow) : QModelIndex();
    if (role == DownrankedRole) {
        return sourceRowDownranked(row.sourceRow);
    }
    if (role == FinishedRole) {
        return sourceRowFinished(row.sourceRow);
    }
    const QString source = sourceIndex.data(PaperLibraryModel::SourceRole).toString().toCaseFolded();
    const QString journal = sourceIndex.data(PaperLibraryModel::JournalRole).toString().toCaseFolded();
    const QString text = sourceIndex.data(PaperLibraryModel::HaystackRole).toString() + QLatin1Char('\n') + source;
    const QString description = sourceIndex.data(PaperLibraryModel::DescriptionRole).toString().trimmed();
    const QStringList librarianTopics = sourceIndex.data(PaperLibraryModel::TopicsRole).toStringList();
    const QString readingLevel = sourceIndex.data(PaperLibraryModel::ReadingLevelRole).toString().trimmed();
    const QString subgenre = sourceIndex.data(PaperLibraryModel::SubgenreRole).toString().trimmed();
    if (role == Qt::DisplayRole && focusRow && !row.title.isEmpty()) {
        // If this feed/imported row maps to a catalog book and its imported title carries obvious
        // download cruft ("Langman Medical Embryology (2018)", "... ( PDFDrive )"), show the clean
        // librarian title instead. Curated feed titles have no such markers and are kept as-is.
        if (row.sourceRow >= 0 && m_source && titleHasImportCruft(row.title)) {
            const QString clean = m_source->index(row.sourceRow).data(Qt::DisplayRole).toString().trimmed();
            if (!clean.isEmpty()) {
                return clean;
            }
        }
        return row.title;
    }
    if (role == PaperLibraryModel::DetailRole && focusRow && !focusDetailText.isEmpty()) {
        return focusDetailText;
    }
    if (role == ShelfIntentRole) {
        if (focusRow && !row.focusReason.isEmpty()) {
            return focusReasonPrimary(row.focusReason);
        }
        if (!description.isEmpty()) {
            return description;
        }
        return corpusShelfIntentFor(m_smartFilter, sourceIndex, text, source, journal);
    }
    if (role == RelationHintRole) {
        if (focusRow && !row.focusReason.isEmpty()) {
            const QString secondary = focusReasonSecondary(row.focusReason);
            return secondary.isEmpty() ? row.focusSection : secondary;
        }
        if (!subgenre.isEmpty()) {
            return subgenre;
        }
        return corpusRelationHintFor(m_smartFilter, sourceIndex, text, source, journal);
    }
    if (role == PriorityHintRole) {
        if (focusRow && !row.focusSection.isEmpty()) {
            return row.focusSection;
        }
        if (!readingLevel.isEmpty()) {
            return readingLevel;
        }
        return corpusPriorityHintFor(sourceIndex, text, source, journal);
    }
    if (role == Qt::ToolTipRole) {
        return QVariant();
    }
    if (role == KindRole) {
        if (focusRow && !row.focusKind.isEmpty()) {
            return row.focusKind.toUpper();
        }
        if (recordMatchesTextbook(text, source)) {
            return QStringLiteral("TEXTBOOK");
        }
        if (recordMatchesBook(text, source, journal)) {
            return QStringLiteral("BOOK");
        }
        if (source.startsWith(QLatin1String("guideline:")) || source.startsWith(QLatin1String("localevidence:"))) {
            return QStringLiteral("GUIDE");
        }
        return QStringLiteral("PAPER");
    }
    if (role == TopicTagsRole) {
        if (!librarianTopics.isEmpty()) {
            QStringList tags = librarianTopics;
            if (focusRow && !row.focusSection.isEmpty() && !tags.contains(row.focusSection)) {
                tags.append(row.focusSection);
            }
            return tags;
        }
        if (focusRow) {
            QStringList tags;
            if (m_smartFilter == Mnd) {
                const QString mndTopic = mndTopicSectionFor(text);
                if (!mndTopic.isEmpty() && mndTopic != QLatin1String("Core MND / ALS")) {
                    tags.append(mndTopic);
                }
                const QString relation = corpusRelationHintFor(m_smartFilter, sourceIndex, text, source, journal);
                if (!relation.isEmpty() && !tags.contains(relation)) {
                    tags.append(relation);
                }
                if (!row.focusSection.isEmpty() && !tags.contains(row.focusSection)) {
                    tags.append(row.focusSection);
                }
            }
            if (tags.isEmpty() && !row.focusSection.isEmpty()) {
                tags.append(row.focusSection);
            }
            const QString primary = focusReasonPrimary(row.focusReason);
            if (!primary.isEmpty() && !tags.contains(primary)) {
                tags.append(primary);
            }
            return tags;
        }
        const QString year = sourceIndex.data(PaperLibraryModel::YearRole).toString();
        if (isBookPresentationShelf(m_smartFilter)) {
            return bookShelfTopicTagsFor(m_smartFilter, sourceIndex, text, source, journal);
        }
        const QString focus = focusBucketFor(sourceIndex, text, source, journal);
        const QString topic = topicBucketFor(text, source, journal);
        QStringList tags;
        if (!focus.isEmpty() && focus != QLatin1String("General Research")) {
            tags.append(focus);
        }
        if (!topic.isEmpty() && topic != focus) {
            tags.append(topic);
        }
        if (tags.size() < 2 && !year.isEmpty() && year != QLatin1String("None")) {
            tags.append(year);
        }
        return tags.isEmpty() ? QStringList({topic}) : tags;
    }
    if (role == RelatedQueryRole) {
        if (focusRow && !row.focusSection.isEmpty()) {
            return row.focusSection;
        }
        return relatedQueryFor(text, source, journal);
    }
    if (role == FocusRole) {
        if (focusRow && !row.focusSection.isEmpty()) {
            return row.focusSection;
        }
        return focusBucketFor(sourceIndex, text, source, journal);
    }
    if (role == ThumbnailSeedRole) {
        if (focusRow && !row.focusSection.isEmpty()) {
            return row.focusSection;
        }
        return thumbnailSeedFor(sourceIndex, text, source, journal);
    }
    return m_source ? m_source->data(m_source->index(row.sourceRow), role) : QVariant();
}

QString PaperLibrarySectionedModel::resolvePath(const QModelIndex &index) const
{
    if (!m_source || !index.isValid() || index.row() < 0 || index.row() >= m_rows.count()) {
        return QString();
    }
    const Row &row = m_rows.at(index.row());
    if (row.header) {
        return QString();
    }
    if (row.sourceRow < 0) {
        return QFileInfo::exists(row.focusPath) ? row.focusPath : QString();
    }
    if (!row.focusPath.isEmpty() && QFileInfo::exists(row.focusPath)) {
        return row.focusPath;
    }
    return m_source->resolvePdfPath(row.sourceRow);
}

void PaperLibrarySectionedModel::setCoverForPath(const QString &path, const QVariant &cover, bool generated)
{
    if (path.isEmpty() || !cover.isValid()) {
        return;
    }
    m_coverPixmaps.insert(path, cover);
    if (generated) {
        m_generatedCoverPaths.insert(path);
    } else {
        m_generatedCoverPaths.remove(path);
    }

    const QList<int> roles = {CoverPixmapRole, GeneratedCoverRole};
    const QList<int> changedRows = m_rowsByPath.value(path);
    for (const int row : changedRows) {
        const QModelIndex changed = index(row);
        Q_EMIT dataChanged(changed, changed, roles);
    }
}

QString PaperLibrarySectionedModel::pathForRow(const Row &row) const
{
    return !row.focusPath.isEmpty() ? row.focusPath : storedPathForSourceRow(row.sourceRow);
}

QString PaperLibrarySectionedModel::storedPathForSourceRow(int sourceRow) const
{
    if (!m_source || sourceRow < 0 || sourceRow >= m_source->rowCount()) {
        return QString();
    }
    return m_source->index(sourceRow).data(PaperLibraryModel::ResolvedPathRole).toString();
}

// Non-fiction book genres from the closed librarian vocabulary (Fiction is handled
// separately; Academic/Unknown are not book non-fiction shelves).
static bool isNonfictionBookGenre(const QString &genre)
{
    return genre.compare(QLatin1String("Nonfiction"), Qt::CaseInsensitive) == 0
        || genre.compare(QLatin1String("Reference"), Qt::CaseInsensitive) == 0
        || genre.compare(QLatin1String("Textbook"), Qt::CaseInsensitive) == 0
        || genre.compare(QLatin1String("Manual"), Qt::CaseInsensitive) == 0;
}

// A genre is authoritative for shelves ONLY if it maps to one. "Unknown", "Academic",
// a typo, or any unrecognized value must NOT suppress the heuristic fallback — otherwise
// one bad/stale backend value would hide a book from Fiction AND Nonfiction.
static bool genreDrivesShelves(const QString &genre)
{
    return genre.compare(QLatin1String("Fiction"), Qt::CaseInsensitive) == 0
        || isNonfictionBookGenre(genre);
}

// Is this record a book? Prefer the authoritative librarian record_kind ('book'/'paper'
// from catalog.jsonl); fall back to the text heuristic only when the flag is absent
// (an older catalog, or a row with no librarian record). This is what lets genre-classified
// books stop vanishing — recordMatchesBook alone was too fragile to gate the shelves on.
/** Genres that override a record_kind of "paper", because only a book carries them.
    "Manual" is absent: 310 rows carry it and they are practice guidelines and procedure
    standards, not books. "Fiction" is absent too -- no live row is genre=Fiction over
    record_kind=paper, so admitting it would buy nothing and would undo the guard that
    keeps a paper mislabelled Fiction off the Fiction shelf. */
static bool isUnambiguousBookGenre(const QString &genre)
{
    return genre.compare(QLatin1String("Nonfiction"), Qt::CaseInsensitive) == 0
        || genre.compare(QLatin1String("Reference"), Qt::CaseInsensitive) == 0
        || genre.compare(QLatin1String("Textbook"), Qt::CaseInsensitive) == 0;
}

// Feeds that serve papers, clinical guidelines, drug labels and reports -- never books. The
// librarian sometimes tags a conference abstract "Reference", so a book genre from one of these
// must NOT rescue it into Books (this is the ALS/MND leak: europepmc/harvest abstracts).
static bool isPaperFeedSource(const QString &source)
{
    static const QStringList feeds = {QStringLiteral("europepmc"), QStringLiteral("pmc"),
                                      QStringLiteral("harvest"),   QStringLiteral("guideline"),
                                      QStringLiteral("localevidence"), QStringLiteral("gov-report"),
                                      QStringLiteral("cochrane"),  QStringLiteral("crossref"),
                                      QStringLiteral("semantic")};
    const QString prefix = source.section(QLatin1Char(':'), 0, 0).trimmed();
    for (const QString &feed : feeds) {
        if (prefix.compare(feed, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

static bool recordIsBook(const QString &recordKind, const QString &genre, const QString &text, const QString &source,
                         const QString &journal)
{
    if (recordKind.compare(QLatin1String("book"), Qt::CaseInsensitive) == 0) {
        return true;
    }
    // The librarian assigns record_kind and genre in one pass and they disagree on ~140 rows:
    // record_kind="paper" over a book genre. The genre is right often enough to rescue a real book
    // (Elements of Information Theory, Demonic Males) that record_kind hid -- but ONLY from sources
    // that actually serve books. From a paper/clinical feed a "Reference" tag is the librarian
    // mislabelling an abstract, so a book genre there is not trusted.
    if (isUnambiguousBookGenre(genre) && !isPaperFeedSource(source)) {
        return true;
    }
    if (recordKind.compare(QLatin1String("paper"), Qt::CaseInsensitive) == 0) {
        return false;
    }
    return recordMatchesBook(text, source, journal);
}

bool PaperLibrarySectionedModel::acceptsSourceRow(int sourceRow) const
{
    if (!m_source || sourceRow < 0 || sourceRow >= m_source->rowCount()) {
        return false;
    }
    const QModelIndex index = m_source->index(sourceRow);
    const QString haystack = index.data(PaperLibraryModel::HaystackRole).toString();
    const QString source = index.data(PaperLibraryModel::SourceRole).toString().toCaseFolded();
    const QString journal = index.data(PaperLibraryModel::JournalRole).toString().toCaseFolded();
    // Authoritative librarian genre (from catalog.jsonl). When present it decides
    // Fiction/Nonfiction directly; when empty we fall back to the text heuristics.
    // Only a confident genre is exported, so a present genre is trustworthy.
    const QString genre = index.data(PaperLibraryModel::GenreRole).toString();
    const QString recordKind = index.data(PaperLibraryModel::RecordKindRole).toString();
    const QString text = haystack + QLatin1Char('\n') + source;
    const bool isBook = recordIsBook(recordKind, genre, text, source, journal);
    // A finished book belongs on the Finished shelf, not cluttering the "what to read" shelves.
    if ((m_smartFilter == Books || m_smartFilter == Fiction || m_smartFilter == Nonfiction
         || m_smartFilter == Textbooks)
        && sourceRowFinished(sourceRow)) {
        return false;
    }
    switch (m_smartFilter) {
    case Papers:
        return !isBook;
    case Books:
        return isBook;
    case Finished:
        return sourceRowFinished(sourceRow);
    case Textbooks:
        // The librarian's genre is authoritative when it is present: 178 rows are marked
        // Textbook, and the heuristic below neither finds all of them nor confines itself
        // to books (any title containing "textbook of " matched, paper or not).
        if (genreDrivesShelves(genre)) {
            return isBook && genre.compare(QLatin1String("Textbook"), Qt::CaseInsensitive) == 0;
        }
        return recordMatchesTextbook(text, source);
    case Medicine:
        return recordMatchesMedicine(text) && recordMatchesTextbook(text, source);
    case Psychiatry:
        return recordMatchesPsychiatry(text);
    case Mnd:
        return recordMatchesMnd(text);
    case Work:
        return recordMatchesBeyondBayes(text, source, journal) || recordMatchesPeerReview(text, source);
    case Anthropology:
        return recordMatchesAnthropology(text);
    case Politics:
        return recordMatchesPolitics(text);
    case Fiction:
        // Both branches are book shelves. The fallback runs for every row whose genre is
        // not one of Fiction/Nonfiction/Reference/Textbook/Manual -- notably "Academic",
        // ~95% of the corpus -- so without isBook it admitted papers on a text match.
        if (genreDrivesShelves(genre)) {
            return isBook && genre.compare(QLatin1String("Fiction"), Qt::CaseInsensitive) == 0;
        }
        return isBook && recordMatchesFiction(text);
    case Nonfiction:
        if (genreDrivesShelves(genre)) {
            return isBook && isNonfictionBookGenre(genre);
        }
        return isBook && recordMatchesNonfiction(text, source, journal);
    }
    return false;
}

bool PaperLibrarySectionedModel::sourceRowDownranked(int sourceRow) const
{
    if (!m_source || sourceRow < 0 || sourceRow >= m_source->rowCount()) {
        return false;
    }
    const QString slug = m_source->index(sourceRow).data(PaperLibraryModel::SlugRole).toString();
    return !slug.isEmpty() && m_downrankedSlugs.contains(slug);
}

QSet<QString> PaperLibrarySectionedModel::readDownrankedSlugs() const
{
    return slugSetFromConfig(paperLibraryConfigGroupForPath(m_feedConfigPath, QStringLiteral("CorpusFeed")), DOWNRANKED_SLUGS_KEY);
}

void PaperLibrarySectionedModel::saveDownrankedSlugs(const QSet<QString> &downrankedSlugs) const
{
    QStringList slugs;
    slugs.reserve(downrankedSlugs.size());
    for (const QString &slug : downrankedSlugs) {
        slugs.append(slug);
    }
    slugs.sort();
    KConfigGroup group = paperLibraryConfigGroupForPath(m_feedConfigPath, QStringLiteral("CorpusFeed"));
    group.writeEntry(DOWNRANKED_SLUGS_KEY, slugs);
    group.sync();
}

void PaperLibrarySectionedModel::applyDownrankedSlugs(const QSet<QString> &downrankedSlugs)
{
    if (m_downrankedSlugs == downrankedSlugs) {
        return;
    }
    m_downrankedSlugs = downrankedSlugs;
    clearRowCache();
    rebuild();
}

void PaperLibrarySectionedModel::broadcastDownrankedSlugs(const QSet<QString> &downrankedSlugs)
{
    // Copy before rebuilding, then re-check membership: code reacting to one model's reset can
    // synchronously destroy another receiver that has not been visited yet.
    auto &registry = sectionedModelsByConfig();
    const QSet<PaperLibrarySectionedModel *> models = sectionedModelsByConfig().value(m_feedConfigPath);
    for (PaperLibrarySectionedModel *model : models) {
        const auto live = registry.constFind(m_feedConfigPath);
        if (model && live != registry.cend() && live->contains(model)) {
            model->applyDownrankedSlugs(downrankedSlugs);
        }
    }
}

void PaperLibrarySectionedModel::setDownranked(const QModelIndex &index, bool downranked)
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.count() || !m_source) {
        return;
    }
    const Row &row = m_rows.at(index.row());
    if (row.header || row.sourceRow < 0) {
        return;
    }
    const QString slug = m_source->index(row.sourceRow).data(PaperLibraryModel::SlugRole).toString();
    if (slug.isEmpty()) {
        return;
    }

    // Merge against the persisted source of truth at mutation time. Another shelf/tab may have
    // changed it since this model was built; writing this model's stale snapshot would clobber that
    // change even if broadcasting were otherwise correct.
    QSet<QString> latest = readDownrankedSlugs();
    const bool changed = downranked ? !latest.contains(slug) : latest.contains(slug);
    if (!changed) {
        broadcastDownrankedSlugs(latest);
        return;
    }
    if (downranked) {
        latest.insert(slug);
    } else {
        latest.remove(slug);
    }
    saveDownrankedSlugs(latest);
    broadcastDownrankedSlugs(latest);
}

static QString finishedTitleKey(const QString &title)
{
    QString key;
    key.reserve(title.size());
    for (const QChar ch : title) {
        if (ch.isLetterOrNumber()) {
            key.append(ch.toCaseFolded());
        }
    }
    return key;
}

void PaperLibrarySectionedModel::rebuildFinishedTitles()
{
    // A book often has several catalog rows (an EPUB and a PDF, different acquisitions), each with
    // its own slug. Marking one finished must retire them all, so map the finished slugs to titles
    // and match on the title too. Small set, so scan the source once when the finished set changes.
    m_finishedTitles.clear();
    if (!m_source || m_finishedSlugs.isEmpty()) {
        return;
    }
    const int rows = m_source->rowCount();
    for (int row = 0; row < rows; ++row) {
        const QModelIndex index = m_source->index(row);
        if (m_finishedSlugs.contains(index.data(PaperLibraryModel::SlugRole).toString())) {
            const QString key = finishedTitleKey(index.data(Qt::DisplayRole).toString());
            if (!key.isEmpty()) {
                m_finishedTitles.insert(key);
            }
        }
    }
}

bool PaperLibrarySectionedModel::sourceRowFinished(int sourceRow) const
{
    if (!m_source || sourceRow < 0 || sourceRow >= m_source->rowCount()) {
        return false;
    }
    const QModelIndex index = m_source->index(sourceRow);
    const QString slug = index.data(PaperLibraryModel::SlugRole).toString();
    if (!slug.isEmpty() && m_finishedSlugs.contains(slug)) {
        return true;
    }
    return titleIsFinished(index.data(Qt::DisplayRole).toString());
}

// True when @p title matches a finished book by title. A finished book often has several catalog
// rows (and file-backed feed copies) with DIFFERENT titles -- "Means of Ascent" vs "The Years of
// Lyndon Johnson: Means of Ascent" -- so beyond an exact normalised-key match, one title CONTAINING
// the other counts too, guarded by a minimum length so a short title can't sweep up unrelated books.
// Used both for corpus rows and for file-backed feed rows that carry no corpus slug.
bool PaperLibrarySectionedModel::titleIsFinished(const QString &title) const
{
    const QString titleKey = finishedTitleKey(title);
    if (titleKey.isEmpty()) {
        return false;
    }
    if (m_finishedTitles.contains(titleKey)) {
        return true;
    }
    constexpr int MinContainsLen = 10; // "means of ascent" -> "meansofascent" (13), safely specific
    for (const QString &finishedKey : m_finishedTitles) {
        if (finishedKey.size() >= MinContainsLen && titleKey.contains(finishedKey)) {
            return true;
        }
        if (titleKey.size() >= MinContainsLen && finishedKey.contains(titleKey)) {
            return true;
        }
    }
    return false;
}

void PaperLibrarySectionedModel::saveFinishedSlugs() const
{
    QStringList slugs;
    slugs.reserve(m_finishedSlugs.size());
    for (const QString &slug : m_finishedSlugs) {
        slugs.append(slug);
    }
    slugs.sort();
    KConfigGroup group = paperLibraryConfigGroupForPath(m_feedConfigPath, QStringLiteral("CorpusFeed"));
    group.writeEntry(FINISHED_SLUGS_KEY, slugs);
    group.sync();
}

void PaperLibrarySectionedModel::reloadFinishedSlugs()
{
    // The finished set is shared through config; each shelf's model keeps its own copy, so a
    // mark made on (say) the Books model must be picked up here before the Finished shelf is
    // shown. Re-read, and only rebuild when the set actually changed AND this model's
    // membership depends on it (the Finished shelf).
    const QSet<QString> latest = slugSetFromConfig(paperLibraryConfigGroupForPath(m_feedConfigPath, QStringLiteral("CorpusFeed")), FINISHED_SLUGS_KEY);
    if (latest == m_finishedSlugs) {
        return;
    }
    m_finishedSlugs = latest;
    rebuildFinishedTitles();
    // Finished changing affects the Finished shelf AND the reading shelves it is excluded from.
    if (m_smartFilter == Finished || m_smartFilter == Books || m_smartFilter == Fiction
        || m_smartFilter == Nonfiction || m_smartFilter == Textbooks) {
        clearRowCache();
        rebuild();
    }
}

void PaperLibrarySectionedModel::setFinished(const QModelIndex &index, bool finished)
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.count() || !m_source) {
        return;
    }
    const Row &row = m_rows.at(index.row());
    if (row.header) {
        return;
    }
    QString slug;
    if (row.sourceRow >= 0) {
        slug = m_source->index(row.sourceRow).data(PaperLibraryModel::SlugRole).toString();
    } else if (!row.title.isEmpty()) {
        // A file-backed feed book (an imported reading item with no corpus row of its own) has
        // sourceRow < 0 and thus no slug. Bind it to its corpus twin by title, so marking it
        // finished lands it on the (corpus-backed) Finished shelf and drops it from the reading feed.
        const int twin = m_source->rowForAnyTitle(row.title);
        if (twin >= 0) {
            slug = m_source->index(twin).data(PaperLibraryModel::SlugRole).toString();
        }
    }
    if (slug.isEmpty()) {
        return;
    }
    const bool changed = finished ? !m_finishedSlugs.contains(slug) : m_finishedSlugs.contains(slug);
    if (!changed) {
        return;
    }
    if (finished) {
        m_finishedSlugs.insert(slug);
    } else {
        m_finishedSlugs.remove(slug);
    }
    saveFinishedSlugs();
    // Keep the by-title finished set current so title-based detection (a file-backed feed book, a
    // differently-titled edition) sees this mark without waiting for a source reload.
    rebuildFinishedTitles();
    // Reflect the change on the CURRENT shelf right away: a finished book leaves the reading feed and
    // appears on Finished. A single warm rebuild on an explicit user action is fine -- classification
    // is cached, so this is not the cold whole-corpus sweep the mark path once triggered. Other live
    // shelves pick it up from config via reloadFinishedSlugs() when next shown.
    if (m_smartFilter == Finished || m_smartFilter == Books || m_smartFilter == Fiction
        || m_smartFilter == Nonfiction || m_smartFilter == Textbooks) {
        clearRowCache();
        rebuild();
    }
}

QString PaperLibrarySectionedModel::cacheKey() const
{
    // The finished set is part of what a shelf shows (finished books are excluded from the reading
    // shelves), so it MUST be in the key -- otherwise a rebuild that runs before the finished titles
    // are known caches rows that still contain them, and later rebuilds return that stale cache.
    QStringList finished(m_finishedTitles.constBegin(), m_finishedTitles.constEnd());
    finished.sort();
    return QStringLiteral("%1|%2|%3|%4")
        .arg(static_cast<int>(m_smartFilter))
        .arg(static_cast<int>(m_sectionMode))
        .arg(m_query)
        .arg(qHash(finished.join(QLatin1Char('\x1f'))));
}

void PaperLibrarySectionedModel::clearRowCache()
{
    m_rowCache.clear();
    m_rowsByPath.clear();
}

void PaperLibrarySectionedModel::rebuildPathIndex()
{
    m_rowsByPath.clear();
    for (int row = 0; row < m_rows.count(); ++row) {
        const Row &sectionRow = m_rows.at(row);
        if (sectionRow.header) {
            continue;
        }
        const QString path = pathForRow(sectionRow);
        if (!path.isEmpty()) {
            m_rowsByPath[path].append(row);
        }
    }
}

void PaperLibrarySectionedModel::resetVisibleRows()
{
    const int visibleCount = qMin(InitialCorpusRows, m_allRows.count());
    m_rows = m_allRows.mid(0, visibleCount);
}

void PaperLibrarySectionedModel::rebuild()
{
    TelemetryScope op(QStringLiteral("shelf_rebuild"));
    const QString key = cacheKey();
    beginResetModel();
    if (!m_explicitRowsActive) {
        const auto cached = m_rowCache.constFind(key);
        if (cached != m_rowCache.cend()) {
            m_allRows = cached.value();
            resetVisibleRows();
            rebuildPathIndex();
            endResetModel();
            return;
        }
    }

    m_rows.clear();
    m_allRows.clear();
    if (!m_source) {
        m_rowsByPath.clear();
        endResetModel();
        return;
    }

    QSet<int> emittedSourceRows;
    QSet<QString> emittedManifestPaths;
    QSet<QString> emittedWorkKeys;
    QSet<QString> emittedBookTitleKeys;
    const auto duplicateBookTitleKey = [this](const QString &title, const QString &author) -> QString {
        // Collapse same-title editions/copies on any BOOK shelf (cover-only presentation), not just
        // the feed shelves -- otherwise three separate "The Psychiatric Interview" files all show on
        // Textbooks. (isBookPresentationShelf is deliberately left alone -- it also drives tags.)
        switch (m_smartFilter) {
        case Books:
        case Fiction:
        case Nonfiction:
        case Textbooks:
        case Medicine:
        case Finished:
            break;
        default:
            return QString();
        }
        const QString t = normalizedDuplicateWorkText(title);
        if (t.isEmpty()) {
            return QString();
        }
        // Merge on the title alone when it is distinctive: either long, or carrying a number
        // ("Galatea 2.2", "Fahrenheit 451" -> one specific work). Crucially this merges two copies
        // even when one lacks an author, which title+author cannot (the "Galatea 2.2" duplicate: one
        // copy had no author, so its author-qualified key never matched the other's).
        static const QRegularExpression hasDigit(QStringLiteral("[0-9]"));
        if (t.size() >= 16 || t.contains(hasDigit)) {
            return t;
        }
        // A short, generic title ("Meditations", "Poems") needs the author, so different works that
        // share it (Aurelius vs Descartes) stay separate; without an author it is too generic to merge.
        const QString a = normalizedDuplicateWorkText(author);
        return a.isEmpty() ? QString() : t + QStringLiteral(" | ") + a;
    };

    if (m_explicitRowsActive) {
        for (const int sourceRow : std::as_const(m_explicitSourceRows)) {
            // Explicit rows are curated by the caller. Search results stay shelf-scoped, but
            // "adjacent documents" pass bypassShelfFilter so book neighbours aren't dropped by
            // the Papers shelf's !isBook rule. Always bounds-check the row either way.
            if (sourceRow < 0 || !m_source || sourceRow >= m_source->rowCount()
                || emittedSourceRows.contains(sourceRow)) {
                continue;
            }
            if (!m_explicitBypassFilter && !acceptsSourceRow(sourceRow)) {
                continue;
            }
            const QString workKey = sourceRowDuplicateWorkKey(m_source, sourceRow);
            if (!workKey.isEmpty()) {
                if (emittedWorkKeys.contains(workKey)) {
                    continue;
                }
                emittedWorkKeys.insert(workKey);
            }
            Row row;
            row.sourceRow = sourceRow;
            m_rows.append(row);
            emittedSourceRows.insert(sourceRow);
        }

        if (m_rows.isEmpty()) {
            Row row;
            row.sourceRow = -1;
            row.title = m_explicitRowsEmptyText.isEmpty() ? QStringLiteral("No matching documents") : m_explicitRowsEmptyText;
            row.focusKind = QStringLiteral("search");
            row.focusSection = m_explicitRowsLabel.isEmpty() ? QStringLiteral("Search") : m_explicitRowsLabel;
            row.focusReason = QStringLiteral("No indexed corpus records match this search in the current shelf.");
            row.focusOrder = 0;
            m_rows.append(row);
        }

        m_allRows = m_rows;
        resetVisibleRows();
        rebuildPathIndex();
        endResetModel();
        return;
    }

    const FocusManifest focusManifest = loadFocusManifest(m_smartFilter, m_source->corpusDir());
    if (focusManifest.found) {
        for (const FocusManifestEntry &entry : focusManifest.entries) {
            int sourceRow = -1;
            const QString idKey = focusLookupKey(entry.id);
            const QString doiKey = focusLookupKey(entry.doi);
            const QString pathKey = focusPathLookupKey(entry.path);
            if (!idKey.isEmpty()) {
                sourceRow = m_source->rowForLookupSlug(idKey);
            }
            if (sourceRow < 0 && !doiKey.isEmpty()) {
                sourceRow = m_source->rowForLookupDoi(doiKey);
            }
            if (sourceRow < 0 && !pathKey.isEmpty()) {
                sourceRow = m_source->rowForLookupPath(pathKey);
            }

            // A finished book belongs on the Finished shelf, not the reading feed. The feed
            // (focus manifest) is a separate render path from the section grid, so it needs its
            // own guard -- without this, marking a book finished never removes it from the feed. A
            // file-backed feed book (sourceRow < 0, an imported reading item) has no corpus slug, so
            // fall back to matching its finished-state by title.
            // A finished book belongs on the Finished shelf, not the reading feed. Check three ways,
            // because the feed entry is often an IMPORTED reading copy whose md5 id is not a corpus
            // slug: (1) the entry's own manifest id is in the finished set (the imported copy the
            // user actually marked finished -- this is the case sourceRowFinished/titleIsFinished
            // both miss), (2) it resolves to a finished corpus row, or (3) its title matches a
            // finished title. Without (1), finished Caro/Graeber reading copies leaked onto Books.
            if ((m_smartFilter == Books || m_smartFilter == Fiction || m_smartFilter == Nonfiction
                 || m_smartFilter == Textbooks)
                && (m_finishedSlugs.contains(entry.id.trimmed())
                    || (sourceRow >= 0 && sourceRowFinished(sourceRow))
                    || titleIsFinished(entry.title))) {
                continue;
            }

            QString queryText = QStringList({entry.title, entry.authors, entry.year, entry.journal, entry.source, entry.doi, entry.id, entry.reason, entry.section})
                                    .join(QLatin1Char('\n'))
                                    .toCaseFolded();
            if (sourceRow >= 0) {
                queryText += QLatin1Char('\n') + m_source->index(sourceRow).data(PaperLibraryModel::HaystackRole).toString();
            }
            if (!m_query.isEmpty() && !queryText.contains(m_query)) {
                continue;
            }

            if (sourceRow >= 0) {
                if (emittedSourceRows.contains(sourceRow)) {
                    continue;
                }
                emittedSourceRows.insert(sourceRow);
            } else if (!pathKey.isEmpty()) {
                if (emittedManifestPaths.contains(pathKey)) {
                    continue;
                }
                emittedManifestPaths.insert(pathKey);
            } else {
                continue;
            }

            QString workKey = sourceRow >= 0 ? sourceRowDuplicateWorkKey(m_source, sourceRow) : QString();
            if (workKey.isEmpty()) {
                if (!entry.doi.trimmed().isEmpty()) {
                    workKey = QStringLiteral("doi|%1").arg(entry.doi.trimmed().toCaseFolded());
                } else {
                    workKey = canonicalDuplicateWorkKey(entry.title,
                                                        entry.authors,
                                                        QStringList({entry.year, entry.journal, entry.source, entry.reason, entry.section, entry.id}).join(QLatin1Char(' ')));
                }
            }
            if (!workKey.isEmpty()) {
                if (emittedWorkKeys.contains(workKey)) {
                    continue;
                }
                emittedWorkKeys.insert(workKey);
            }

            Row row;
            row.sourceRow = sourceRow;
            row.title = entry.title;
            row.focusId = entry.id;
            row.focusDoi = entry.doi;
            row.focusAuthors = entry.authors;
            row.focusYear = entry.year;
            row.focusJournal = entry.journal;
            row.focusSource = entry.source;
            row.focusKind = entry.kind;
            row.focusSection = entry.sectionLabel;
            row.focusReason = entry.reason;
            row.focusThumbnailPath = entry.thumbnailPath;
            row.focusThumbnailSource = entry.thumbnailSource;
            row.focusPath = entry.path;
            // A feed entry that names no file of its own but resolves to a catalog row inherits that
            // row's PDF path -- otherwise it has no stable identity to key its cover/thumbnail by (an
            // empty path is skipped by the cover loader), and no file to open.
            if (row.focusPath.isEmpty() && sourceRow >= 0) {
                row.focusPath = m_source->resolvePdfPath(sourceRow);
            }
            row.focusOrder = entry.order;
            row.focusScore = entry.score;
            const QString bookTitleKey = duplicateBookTitleKey(
                row.title, sourceRow >= 0 ? m_source->index(sourceRow).data(PaperLibraryModel::AuthorsRole).toString() : row.focusAuthors);
            if (!bookTitleKey.isEmpty()) {
                if (emittedBookTitleKeys.contains(bookTitleKey)) {
                    continue;
                }
                emittedBookTitleKeys.insert(bookTitleKey);
            }
            m_rows.append(row);
        }
    }

    struct SortKey {
        bool downranked = false;
        int shelfScore = 0;
        bool pinned = false;
        int accessCount = 0;
        QString lastAccessed;
        bool missing = false;
        int citedBy = 0;
        QString added;
        QString title;
    };

    QHash<QString, QList<int>> rowsBySection;
    QHash<int, SortKey> sortKeysBySourceRow;
    QStringList sectionOrder;
    const int sourceRows = m_source->rowCount();
    sortKeysBySourceRow.reserve(sourceRows);
    for (int row = 0; row < sourceRows; ++row) {
        const QModelIndex index = m_source->index(row);
        const QString haystack = index.data(PaperLibraryModel::HaystackRole).toString();
        if (!m_query.isEmpty() && !haystack.contains(m_query)) {
            continue;
        }
        // The Finished shelf's membership is purely the finished-slug set. Skip the ~10 per-row
        // classification heuristics for the 18k rows that aren't finished — running them for the
        // whole corpus cost ~3s uncached and was the mark/finished-shelf hang.
        if (m_smartFilter == Finished && !sourceRowFinished(row)) {
            continue;
        }

        const QString source = index.data(PaperLibraryModel::SourceRole).toString().toCaseFolded();
        const QString journal = index.data(PaperLibraryModel::JournalRole).toString().toCaseFolded();
        // Authoritative librarian genre (only confident genres are exported); when present
        // it decides Fiction/Nonfiction, otherwise fall back to the text heuristics.
        const QString genre = index.data(PaperLibraryModel::GenreRole).toString();
        const QString recordKind = index.data(PaperLibraryModel::RecordKindRole).toString();
        const QString text = haystack + QLatin1Char('\n') + source;
        // Classification is stable per row; cache the regex sweep by source row so a no-query
        // rebuild (downrank/finished re-sort) doesn't re-run ~10 heuristics over 18k rows (~3s).
        bool isBook, isTextbook, isMedicine, isPsychiatry, isWork, isAnthropology, isPolitics, isFiction, isNonfiction,
            isTextbookShelf;
        quint16 classBits = 0;
        const auto cachedClass = m_classifyCache.constFind(row);
        if (cachedClass != m_classifyCache.cend()) {
            classBits = cachedClass.value(); // L1: this model already classified this row
        } else {
            // L2: another sectioned model or library tab may already have swept this slug. Reusing
            // it avoids re-running ~10 regex heuristics over the whole corpus for every new tab.
            const QString slug = index.data(PaperLibraryModel::SlugRole).toString();
            const auto shared = slug.isEmpty() ? sharedClassifyCache().cend() : sharedClassifyCache().constFind(slug);
            if (shared != sharedClassifyCache().cend()) {
                classBits = shared.value();
            } else {
                isBook = recordIsBook(recordKind, genre, text, source, journal);
                isTextbook = recordMatchesTextbook(text, source);
                isMedicine = recordMatchesMedicine(text);
                isPsychiatry = recordMatchesPsychiatry(text);
                isWork = recordMatchesBeyondBayes(text, source, journal) || recordMatchesPeerReview(text, source);
                isAnthropology = recordMatchesAnthropology(text);
                isPolitics = recordMatchesPolitics(text);
                const bool genreShelf = genreDrivesShelves(genre);
                // isBook gates BOTH branches: Fiction and Non-fiction are book shelves,
                // and the fallback text match otherwise admits academic papers (a journal
                // named "...Anthropology" matches, as does a title containing "fiction").
                isFiction = genreShelf ? (isBook && genre.compare(QLatin1String("Fiction"), Qt::CaseInsensitive) == 0)
                                       : (isBook && recordMatchesFiction(text));
                isNonfiction = genreShelf ? (isBook && isNonfictionBookGenre(genre))
                                          : (isBook && recordMatchesNonfiction(text, source, journal));
                // The Textbooks shelf gets its own flag. isTextbook stays the raw heuristic
                // because Medicine composes it (isMedicine && isTextbook); binding the genre
                // into it would quietly redefine the Medicine shelf too.
                isTextbookShelf = genreShelf
                    ? (isBook && genre.compare(QLatin1String("Textbook"), Qt::CaseInsensitive) == 0)
                    : recordMatchesTextbook(text, source);
                classBits = quint16((isBook ? 0x1 : 0) | (isTextbook ? 0x2 : 0) | (isMedicine ? 0x4 : 0)
                                    | (isPsychiatry ? 0x8 : 0) | (isWork ? 0x10 : 0) | (isAnthropology ? 0x20 : 0)
                                    | (isPolitics ? 0x40 : 0) | (isFiction ? 0x80 : 0) | (isNonfiction ? 0x100 : 0)
                                    | (isTextbookShelf ? 0x200 : 0));
                if (!slug.isEmpty()) {
                    sharedClassifyCache().insert(slug, classBits);
                }
            }
            m_classifyCache.insert(row, classBits);
        }
        isBook = classBits & 0x1; isTextbook = classBits & 0x2; isMedicine = classBits & 0x4; isPsychiatry = classBits & 0x8;
        isWork = classBits & 0x10; isAnthropology = classBits & 0x20; isPolitics = classBits & 0x40;
        isFiction = classBits & 0x80; isNonfiction = classBits & 0x100; isTextbookShelf = classBits & 0x200;
        const bool isDownranked = sourceRowDownranked(row);
        // A finished book belongs on the Finished shelf, not the "what to read" shelves.
        if ((m_smartFilter == Books || m_smartFilter == Fiction || m_smartFilter == Nonfiction
             || m_smartFilter == Textbooks)
            && sourceRowFinished(row)) {
            continue;
        }
        switch (m_smartFilter) {
        case Papers:
            if (isBook) {
                continue;
            }
            break;
        case Books:
            // "Books" is the general reading shelf (fiction + trade non-fiction). Medical science,
            // textbooks, and psychiatry have their own shelves (Medicine / Textbooks), so keep them
            // -- and the medical papers the isBook heuristic occasionally admits -- off Books.
            if (!isBook || isMedicine || isTextbookShelf || isPsychiatry) {
                continue;
            }
            break;
        case Finished:
            if (!sourceRowFinished(row)) {
                continue;
            }
            break;
        case Textbooks:
            if (!isTextbookShelf) {
                continue;
            }
            break;
        case Medicine:
            if (!isMedicine || !isTextbook) {
                continue;
            }
            break;
        case Psychiatry:
            if (!isPsychiatry) {
                continue;
            }
            break;
        case Mnd:
            if (!recordMatchesMnd(text)) {
                continue;
            }
            break;
        case Anthropology:
            if (!isAnthropology) {
                continue;
            }
            break;
        case Politics:
            if (!isPolitics) {
                continue;
            }
            break;
        case Work:
            if (!isWork) {
                continue;
            }
            break;
        case Fiction:
            if (!isFiction) {
                continue;
            }
            break;
        case Nonfiction:
            if (!isNonfiction) {
                continue;
            }
            break;
        }

        QString section;
        if (m_sectionMode == ReadNext && isDownranked) {
            section = QStringLiteral("Less Relevant");
        } else if (m_sectionMode == ReadNext && m_smartFilter == Papers) {
            section = papersReadNextSectionFor(index, text, source, journal);
        } else if (m_sectionMode == ReadNext && m_smartFilter == Mnd) {
            if (index.data(PaperLibraryModel::PinnedRole).toBool()) {
                section = QStringLiteral("Pinned");
            } else if (index.data(PaperLibraryModel::AccessCountRole).toInt() > 0) {
                section = QStringLiteral("Continue Reading");
            } else if (source == QLatin1String("md-project-review-set")) {
                section = QStringLiteral("MD Project Review Set");
            } else {
                section = mndTopicSectionFor(text);
            }
        } else if (m_sectionMode == ReadNext && m_smartFilter == Textbooks) {
            section = textbookTopicSectionFor(text);
        } else if (m_sectionMode == ReadNext && m_smartFilter == Medicine) {
            section = medicineReadNextSectionFor(index, text);
        } else if (m_sectionMode == ReadNext && m_smartFilter == Psychiatry) {
            if (index.data(PaperLibraryModel::PinnedRole).toBool()) {
                section = QStringLiteral("Pinned");
            } else if (index.data(PaperLibraryModel::AccessCountRole).toInt() > 0) {
                section = QStringLiteral("Continue Reading");
            } else {
                section = psychiatryTopicSectionFor(text);
            }
        } else if (m_sectionMode == ReadNext && m_smartFilter == Books) {
            section = bookTopicSectionFor(text);
        } else if (m_sectionMode == ReadNext && (m_smartFilter == Work || m_smartFilter == Anthropology || m_smartFilter == Politics)) {
            section = focusBucketFor(index, text, source, journal);
        } else if (m_sectionMode == ReadNext && m_smartFilter == Fiction) {
            section = QStringLiteral("Fiction");
        } else if (m_sectionMode == ReadNext && m_smartFilter == Nonfiction) {
            if (recordMatchesAnthropology(text)) {
                section = QStringLiteral("Anthropology");
            } else if (recordMatchesPolitics(text)) {
                section = QStringLiteral("Politics");
            } else if (recordMatchesCaroLbj(text)) {
                section = QStringLiteral("Biography & History");
            } else {
                section = QStringLiteral("Non-fiction");
            }
        } else {
            switch (m_sectionMode) {
            case ReadNext:
                section = readNextSectionFor(index, text, source, journal);
                break;
            case ByTopic:
                section = topicBucketFor(text, source, journal);
                break;
            case ByProject:
                section = projectBucketFor(text, source, journal);
                break;
            case ByType:
                section = publicationKindFor(text, source, journal);
                break;
            case BySource:
                section = sourceBucketFor(source);
                break;
            case ByYear:
                section = index.data(PaperLibraryModel::YearRole).toString().trimmed();
                if (section.isEmpty() || section == QLatin1String("None")) {
                    section = QStringLiteral("Undated");
                }
                break;
            case ByJournal:
                section = index.data(PaperLibraryModel::JournalRole).toString().trimmed();
                if (section.isEmpty()) {
                    section = QStringLiteral("No Journal");
                }
                break;
            }
        }

        if (!rowsBySection.contains(section)) {
            sectionOrder.append(section);
        }
        rowsBySection[section].append(row);
        SortKey sortKey;
        sortKey.downranked = isDownranked;
        sortKey.shelfScore = sourceRowShelfPriorityScore(m_source, row, m_smartFilter);
        sortKey.pinned = index.data(PaperLibraryModel::PinnedRole).toBool();
        sortKey.accessCount = index.data(PaperLibraryModel::AccessCountRole).toInt();
        sortKey.lastAccessed = index.data(PaperLibraryModel::LastAccessedRole).toString();
        sortKey.missing = index.data(PaperLibraryModel::MissingRole).toBool();
        sortKey.citedBy = index.data(PaperLibraryModel::CitedByCountRole).toInt();
        sortKey.added = index.data(PaperLibraryModel::AddedRole).toString();
        sortKey.title = index.data(Qt::DisplayRole).toString();
        sortKeysBySourceRow.insert(row, sortKey);
    }

    std::sort(sectionOrder.begin(), sectionOrder.end(), [this](const QString &a, const QString &b) {
        if (m_sectionMode == ByYear) {
            if (a == QLatin1String("Undated")) {
                return false;
            }
            if (b == QLatin1String("Undated")) {
                return true;
            }
            return a > b;
        }
        const int leftRank = sectionRank(a);
        const int rightRank = sectionRank(b);
        if (leftRank != rightRank) {
            return leftRank < rightRank;
        }
        return a.localeAwareCompare(b) < 0;
    });

    for (const QString &section : std::as_const(sectionOrder)) {
        QList<int> sourceRowsForSection = rowsBySection.value(section);
        std::stable_sort(sourceRowsForSection.begin(), sourceRowsForSection.end(), [&sortKeysBySourceRow](int leftRow, int rightRow) {
            const auto leftIt = sortKeysBySourceRow.constFind(leftRow);
            const auto rightIt = sortKeysBySourceRow.constFind(rightRow);
            if (leftIt == sortKeysBySourceRow.cend() || rightIt == sortKeysBySourceRow.cend()) {
                return leftRow < rightRow;
            }
            const SortKey &left = leftIt.value();
            const SortKey &right = rightIt.value();
            if (left.downranked != right.downranked) {
                return !left.downranked;
            }
            if (left.shelfScore != right.shelfScore) {
                return left.shelfScore < right.shelfScore;
            }
            if (left.pinned != right.pinned) {
                return left.pinned;
            }
            if (left.accessCount != right.accessCount) {
                return left.accessCount > right.accessCount;
            }
            if (left.lastAccessed != right.lastAccessed) {
                if (left.lastAccessed.isEmpty() != right.lastAccessed.isEmpty()) {
                    return !left.lastAccessed.isEmpty();
                }
                return left.lastAccessed > right.lastAccessed;
            }
            if (left.missing != right.missing) {
                return !left.missing;
            }
            if (left.citedBy != right.citedBy) {
                return left.citedBy > right.citedBy;
            }
            if (left.added != right.added) {
                return left.added > right.added;
            }
            const int titleCompare = left.title.localeAwareCompare(right.title);
            if (titleCompare != 0) {
                return titleCompare < 0;
            }
            return leftRow < rightRow;
        });
        for (const int sourceRow : sourceRowsForSection) {
            if (emittedSourceRows.contains(sourceRow)) {
                continue;
            }
            const QString bookTitleKey = duplicateBookTitleKey(m_source->index(sourceRow).data(Qt::DisplayRole).toString(),
                                                               m_source->index(sourceRow).data(PaperLibraryModel::AuthorsRole).toString());
            if (!bookTitleKey.isEmpty() && emittedBookTitleKeys.contains(bookTitleKey)) {
                continue;
            }
            const QString workKey = sourceRowDuplicateWorkKey(m_source, sourceRow);
            if (!workKey.isEmpty()) {
                if (emittedWorkKeys.contains(workKey)) {
                    continue;
                }
                emittedWorkKeys.insert(workKey);
            }
            if (!bookTitleKey.isEmpty()) {
                emittedBookTitleKeys.insert(bookTitleKey);
            }
            Row row;
            row.sourceRow = sourceRow;
            m_rows.append(row);
            emittedSourceRows.insert(sourceRow);
        }
    }

    if (m_rows.isEmpty() && focusManifest.found) {
        Row row;
        row.sourceRow = -1;
        row.title = m_query.isEmpty() ? QStringLiteral("No local documents yet") : QStringLiteral("No matching documents");
        row.focusKind = QStringLiteral("setup");
        row.focusSection = focusManifestShelfName(m_smartFilter);
        row.focusReason = m_query.isEmpty() ? QStringLiteral("This focus shelf has no matching local files yet; add records or restore files into the PaperLibrary corpus.")
                                            : QStringLiteral("No manifest records match the current search.");
        row.focusOrder = 0;
        m_rows.append(row);
    }

    m_allRows = m_rows;
    // Each distinct search query caches a full row list; bound it so a long typing session
    // doesn't grow the cache without limit (the classification cache below makes a cache miss
    // cheap now anyway). 48 covers the section-mode + a healthy run of recent queries.
    if (m_rowCache.size() >= 48) {
        m_rowCache.clear();
    }
    m_rowCache.insert(key, m_allRows);
    resetVisibleRows();
    rebuildPathIndex();
    endResetModel();
}
