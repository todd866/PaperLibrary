/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <QTest>

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <KConfigGroup>
#include <KSharedConfig>

#include <memory>

#include "../shell/paperlibrarymodel.h"

// Every record in this file is synthetic: invented titles, authors, DOIs
// and journals. Real corpus records must never be copied into the repo.
struct SyntheticRecord {
    QString slug;
    QString doi;
    QString pmid;
    QString citeKey;
    QString title;
    QString authors;
    QString year;
    QString journal;
    qint64 bytes;
    QString source;
    QString addedTs;
};

static SyntheticRecord widgetRecord()
{
    return {QStringLiteral("10-9999-synthetic-widget-1"),
            QStringLiteral("10.9999/synthetic.widget.1"),
            QStringLiteral("00000001"),
            QStringLiteral("doe2021widget"),
            QStringLiteral("Synthetic Study of Widget Dynamics"),
            QStringLiteral("Jane Q. Placeholder; John A. Sample"),
            QStringLiteral("2021"),
            QStringLiteral("Journal of Imaginary Results"),
            12345,
            QStringLiteral("synthetic"),
            QStringLiteral("2026-02-01T00:00:00+00:00")};
}

static SyntheticRecord gadgetRecord()
{
    return {QStringLiteral("10-9999-synthetic-gadget-2"),
            QStringLiteral("10.9999/synthetic.gadget.2"),
            QString(),
            QStringLiteral("example1999gadget"),
            QStringLiteral("Gadget Oscillations Reconsidered"),
            QStringLiteral("Ada Example"),
            QStringLiteral("1999"),
            QStringLiteral("Annals of Placeholder Science"),
            67890,
            QStringLiteral("synthetic"),
            QStringLiteral("2026-01-15T00:00:00+00:00")};
}

static SyntheticRecord untitledRecord()
{
    return {QStringLiteral("10-9999-synthetic-sprocket-3"), QString(), QString(), QString(), QString(), QString(), QString(), QString(), 0, QStringLiteral("synthetic"), QStringLiteral("2025-12-31T00:00:00+00:00")};
}

static SyntheticRecord textbookRecord()
{
    return {QStringLiteral("md5-synthetic-textbook-4"),
            QString(),
            QString(),
            QString(),
            QStringLiteral("Fundamentals of Widget Physiology"),
            QStringLiteral("Taylor Teaching"),
            QStringLiteral("2024"),
            QStringLiteral("(book)"),
            54321,
            QStringLiteral("book:pdf"),
            QStringLiteral("2026-03-01T00:00:00+00:00")};
}

static SyntheticRecord mndRecord()
{
    return {QStringLiteral("10-9999-synthetic-mnd-5"),
            QStringLiteral("10.9999/synthetic.mnd.5"),
            QString(),
            QStringLiteral("sample2026mnd"),
            QStringLiteral("Neurofilament Biomarkers in Amyotrophic Lateral Sclerosis"),
            QStringLiteral("Casey Clinician"),
            QStringLiteral("2026"),
            QStringLiteral("Journal of Synthetic Neurology"),
            98765,
            QStringLiteral("md-project-review-set"),
            QStringLiteral("2026-04-01T00:00:00+00:00")};
}

static SyntheticRecord psychiatryRecord()
{
    return {QStringLiteral("10-9999-synthetic-psychiatry-6"),
            QStringLiteral("10.9999/synthetic.psychiatry.6"),
            QString(),
            QStringLiteral("sample2026psychiatry"),
            QStringLiteral("Major Depression and Suicide Risk in Adolescent Psychiatry"),
            QStringLiteral("Morgan Mind"),
            QStringLiteral("2026"),
            QStringLiteral("Journal of Synthetic Psychiatry"),
            112233,
            QStringLiteral("synthetic"),
            QStringLiteral("2026-04-15T00:00:00+00:00")};
}

static SyntheticRecord clinicalExamTextbookRecord()
{
    return {QStringLiteral("md5-synthetic-clinical-exam-10"),
            QString(),
            QString(),
            QString(),
            QStringLiteral("Talley and O'Connor's Clinical Examination"),
            QStringLiteral("Nicholas Talley; Simon O'Connor"),
            QStringLiteral("2017"),
            QStringLiteral("(book)"),
            101010,
            QStringLiteral("book:pdf"),
            QStringLiteral("2026-05-01T00:00:00+00:00")};
}

static SyntheticRecord neuroTextbookRecord()
{
    return {QStringLiteral("md5-synthetic-neuroanatomy-11"),
            QString(),
            QString(),
            QString(),
            QStringLiteral("Neuroanatomy: An Illustrated Colour Text"),
            QStringLiteral("A. R. Crossman"),
            QStringLiteral("2019"),
            QStringLiteral("(book)"),
            111111,
            QStringLiteral("book:pdf"),
            QStringLiteral("2026-05-02T00:00:00+00:00")};
}

static SyntheticRecord paedsTextbookRecord()
{
    return {QStringLiteral("md5-synthetic-paeds-12"),
            QString(),
            QString(),
            QString(),
            QStringLiteral("Nelson Textbook of Pediatrics"),
            QStringLiteral("Placeholder Editor"),
            QStringLiteral("2024"),
            QStringLiteral("(book)"),
            121212,
            QStringLiteral("book:pdf"),
            QStringLiteral("2026-05-03T00:00:00+00:00")};
}

static SyntheticRecord obgynTextbookRecord()
{
    return {QStringLiteral("md5-synthetic-obgyn-13"),
            QString(),
            QString(),
            QString(),
            QStringLiteral("Obstetrics and Gynaecology for Medical Students"),
            QStringLiteral("Placeholder Editor"),
            QStringLiteral("2024"),
            QStringLiteral("(book)"),
            131313,
            QStringLiteral("book:pdf"),
            QStringLiteral("2026-05-04T00:00:00+00:00")};
}

static SyntheticRecord psychiatryTextbookRecord()
{
    return {QStringLiteral("md5-synthetic-psychiatry-textbook-14"),
            QString(),
            QString(),
            QString(),
            QStringLiteral("Shorter Oxford Textbook of Psychiatry"),
            QStringLiteral("Placeholder Psychiatrist"),
            QStringLiteral("2024"),
            QStringLiteral("(book)"),
            141414,
            QStringLiteral("book:pdf"),
            QStringLiteral("2026-05-05T00:00:00+00:00")};
}

static SyntheticRecord patientSafetyTextbookRecord()
{
    return {QStringLiteral("md5-synthetic-patient-safety-15"),
            QString(),
            QString(),
            QString(),
            QStringLiteral("Human Error and Patient Safety"),
            QStringLiteral("Placeholder Safety"),
            QStringLiteral("2020"),
            QStringLiteral("(book)"),
            151515,
            QStringLiteral("book:pdf"),
            QStringLiteral("2026-05-06T00:00:00+00:00")};
}

static SyntheticRecord anthropologyRecord()
{
    return {QStringLiteral("md5-synthetic-anthropology-7"),
            QString(),
            QString(),
            QString(),
            QStringLiteral("Anthropology of Debt and Exchange"),
            QStringLiteral("Alex Fieldwork"),
            QStringLiteral("2011"),
            QStringLiteral("(book)"),
            445566,
            QStringLiteral("book:pdf"),
            QStringLiteral("2026-04-20T00:00:00+00:00")};
}

static SyntheticRecord politicsRecord()
{
    return {QStringLiteral("md5-synthetic-politics-8"),
            QString(),
            QString(),
            QString(),
            QStringLiteral("Politics and Power in Presidential Biography"),
            QStringLiteral("River Historian"),
            QStringLiteral("2012"),
            QStringLiteral("(book)"),
            556677,
            QStringLiteral("book:pdf"),
            QStringLiteral("2026-04-25T00:00:00+00:00")};
}

static SyntheticRecord caroRecord()
{
    return {QStringLiteral("md5-synthetic-caro-9"),
            QString(),
            QString(),
            QString(),
            QStringLiteral("The Path to Power"),
            QStringLiteral("Robert A. Caro"),
            QStringLiteral("1982"),
            QStringLiteral("(book)"),
            667788,
            QStringLiteral("book:pdf"),
            QStringLiteral("2026-04-30T00:00:00+00:00")};
}

static QByteArray recordLine(const SyntheticRecord &record)
{
    QJsonObject object;
    object.insert(QStringLiteral("slug"), record.slug);
    object.insert(QStringLiteral("doi"), record.doi);
    object.insert(QStringLiteral("md5"), QString());
    object.insert(QStringLiteral("pmid"), record.pmid);
    object.insert(QStringLiteral("cite_key"), record.citeKey);
    object.insert(QStringLiteral("title"), record.title);
    object.insert(QStringLiteral("authors"), record.authors);
    object.insert(QStringLiteral("year"), record.year);
    object.insert(QStringLiteral("journal"), record.journal);
    object.insert(QStringLiteral("bytes"), record.bytes);
    object.insert(QStringLiteral("source"), record.source);
    object.insert(QStringLiteral("added_ts"), record.addedTs);
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}

class PaperLibraryModelTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();

    void testParseCatalogFields();
    void testParseSkipsMalformedLines();
    void testSortNewestFirst();
    void testCorpusExists();
    void testConfiguredCorpusDir();
    void testAsyncLoadPopulatesModel();
    void testLoadMissingCorpusYieldsEmptyModel();
    void testFilterModel();
    void testResolveWithoutDatabase();
    void testDatabaseEnrichment();
    void testDatabaseInPathWithSpecialCharacters();
    void testSectionedModelSmartShelves();
    void testCaroBiographyDoesNotMatchPsychiatry();
    void testReloadIfChanged();
    void testDestroyDuringLoadReclaimsWorker();
    void testDestroyAfterLoad();

private:
    QString writeCatalog(const QList<SyntheticRecord> &records);
    QString touchFile(const QString &relativePath);

    std::unique_ptr<QTemporaryDir> m_dir;
};

void PaperLibraryModelTest::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
}

void PaperLibraryModelTest::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    qputenv("PAPERLIBRARY_CONFIG_PATH", QFile::encodeName(m_dir->filePath(QStringLiteral("paperlibraryrc"))));
}

QString PaperLibraryModelTest::writeCatalog(const QList<SyntheticRecord> &records)
{
    QFile catalog(m_dir->filePath(QStringLiteral("catalog.jsonl")));
    if (!catalog.open(QIODevice::WriteOnly)) {
        return QString();
    }
    for (const SyntheticRecord &record : records) {
        catalog.write(recordLine(record));
    }
    catalog.close();
    return m_dir->path();
}

QString PaperLibraryModelTest::touchFile(const QString &relativePath)
{
    const QString path = m_dir->filePath(relativePath);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return QString();
    }
    file.write("x");
    return path;
}

void PaperLibraryModelTest::testParseCatalogFields()
{
    const QList<PaperLibraryModel::Record> records = PaperLibraryModel::parseCatalog(recordLine(widgetRecord()) + recordLine(untitledRecord()));
    QCOMPARE(records.count(), 2);

    const PaperLibraryModel::Record &record = records.first();
    QCOMPARE(record.slug, QStringLiteral("10-9999-synthetic-widget-1"));
    QCOMPARE(record.doi, QStringLiteral("10.9999/synthetic.widget.1"));
    QCOMPARE(record.pmid, QStringLiteral("00000001"));
    QCOMPARE(record.citeKey, QStringLiteral("doe2021widget"));
    QCOMPARE(record.title, QStringLiteral("Synthetic Study of Widget Dynamics"));
    QCOMPARE(record.authors, QStringLiteral("Jane Q. Placeholder; John A. Sample"));
    QCOMPARE(record.year, QStringLiteral("2021"));
    QCOMPARE(record.journal, QStringLiteral("Journal of Imaginary Results"));
    QCOMPARE(record.bytes, Q_INT64_C(12345));
    QCOMPARE(record.source, QStringLiteral("synthetic"));
    QCOMPARE(record.addedTs, QStringLiteral("2026-02-01T00:00:00+00:00"));

    // The haystack carries every searchable field, case-folded
    QVERIFY(record.haystack.contains(QStringLiteral("synthetic study of widget dynamics")));
    QVERIFY(record.haystack.contains(QStringLiteral("jane q. placeholder")));
    QVERIFY(record.haystack.contains(QStringLiteral("journal of imaginary results")));
    QVERIFY(record.haystack.contains(QStringLiteral("2021")));
    QVERIFY(record.haystack.contains(QStringLiteral("doe2021widget")));
    QVERIFY(record.haystack.contains(QStringLiteral("10.9999/synthetic.widget.1")));
    QVERIFY(record.haystack.contains(QStringLiteral("10-9999-synthetic-widget-1")));

    // Empty fields parse as empty strings, not noise
    QCOMPARE(records.last().title, QString());
    QCOMPARE(records.last().bytes, Q_INT64_C(0));
}

void PaperLibraryModelTest::testParseSkipsMalformedLines()
{
    const QByteArray jsonl = recordLine(widgetRecord()) + "this is not json\n" + "\n" + "[1,2,3]\n" + recordLine(gadgetRecord());
    const QList<PaperLibraryModel::Record> records = PaperLibraryModel::parseCatalog(jsonl);
    QCOMPARE(records.count(), 2);
    QCOMPARE(records.first().slug, QStringLiteral("10-9999-synthetic-widget-1"));
    QCOMPARE(records.last().slug, QStringLiteral("10-9999-synthetic-gadget-2"));
}

void PaperLibraryModelTest::testSortNewestFirst()
{
    QList<PaperLibraryModel::Record> records = PaperLibraryModel::parseCatalog(recordLine(untitledRecord()) + recordLine(widgetRecord()) + recordLine(gadgetRecord()));
    PaperLibraryModel::sortRecords(records);
    QCOMPARE(records.at(0).slug, QStringLiteral("10-9999-synthetic-widget-1"));   // 2026-02-01
    QCOMPARE(records.at(1).slug, QStringLiteral("10-9999-synthetic-gadget-2"));   // 2026-01-15
    QCOMPARE(records.at(2).slug, QStringLiteral("10-9999-synthetic-sprocket-3")); // 2025-12-31
}

void PaperLibraryModelTest::testCorpusExists()
{
    QVERIFY(!PaperLibraryModel::corpusExists(m_dir->path()));
    QVERIFY(!PaperLibraryModel::corpusExists(m_dir->filePath(QStringLiteral("nowhere"))));
    writeCatalog({widgetRecord()});
    QVERIFY(PaperLibraryModel::corpusExists(m_dir->path()));
}

void PaperLibraryModelTest::testConfiguredCorpusDir()
{
    KConfigGroup group = KSharedConfig::openConfig(QString::fromLocal8Bit(qgetenv("PAPERLIBRARY_CONFIG_PATH")), KConfig::SimpleConfig)->group(QStringLiteral("General"));

    group.deleteEntry("PaperLibraryPath");
    group.sync();
    QCOMPARE(PaperLibraryModel::configuredCorpusDir(), QDir::homePath() + QStringLiteral("/Projects/PaperLibrary"));

    group.writeEntry("PaperLibraryPath", m_dir->path());
    group.sync();
    QCOMPARE(PaperLibraryModel::configuredCorpusDir(), m_dir->path());

    group.deleteEntry("PaperLibraryPath");
    group.sync();
}

void PaperLibraryModelTest::testAsyncLoadPopulatesModel()
{
    const QString corpusDir = writeCatalog({gadgetRecord(), widgetRecord(), untitledRecord()});

    PaperLibraryModel model;
    QVERIFY(!model.isLoaded());
    QSignalSpy loadedSpy(&model, &PaperLibraryModel::loaded);
    model.load(corpusDir);
    QVERIFY(loadedSpy.wait(10000));
    QCOMPARE(loadedSpy.first().first().toInt(), 3);
    QVERIFY(model.isLoaded());
    QCOMPARE(model.rowCount(), 3);

    // Newest first; the title is the display text, the slug its fallback
    QCOMPARE(model.data(model.index(0), Qt::DisplayRole).toString(), QStringLiteral("Synthetic Study of Widget Dynamics"));
    QCOMPARE(model.data(model.index(2), Qt::DisplayRole).toString(), QStringLiteral("10-9999-synthetic-sprocket-3"));

    // The secondary row: authors · year · journal, empty fields skipped
    QCOMPARE(model.data(model.index(0), PaperLibraryModel::DetailRole).toString(), QStringLiteral("Jane Q. Placeholder; John A. Sample · 2021 · Journal of Imaginary Results"));
    QCOMPARE(model.data(model.index(2), PaperLibraryModel::DetailRole).toString(), QString());

    QCOMPARE(model.data(model.index(1), PaperLibraryModel::SlugRole).toString(), QStringLiteral("10-9999-synthetic-gadget-2"));
}

void PaperLibraryModelTest::testLoadMissingCorpusYieldsEmptyModel()
{
    PaperLibraryModel model;
    QSignalSpy loadedSpy(&model, &PaperLibraryModel::loaded);
    model.load(m_dir->filePath(QStringLiteral("nowhere")));
    QVERIFY(loadedSpy.wait(10000));
    QCOMPARE(loadedSpy.first().first().toInt(), 0);
    QCOMPARE(model.rowCount(), 0);
}

void PaperLibraryModelTest::testFilterModel()
{
    const QString corpusDir = writeCatalog({widgetRecord(), gadgetRecord(), untitledRecord()});

    PaperLibraryModel model;
    QSignalSpy loadedSpy(&model, &PaperLibraryModel::loaded);
    model.load(corpusDir);
    QVERIFY(loadedSpy.wait(10000));

    PaperLibraryFilterModel filter;
    filter.setSourceModel(&model);
    QCOMPARE(filter.rowCount(), 3);

    // Case-insensitive substring over each searchable field
    const QPair<QString, QString> expectations[] = {
        {QStringLiteral("WIDGET DYNAMICS"), QStringLiteral("10-9999-synthetic-widget-1")},          // title
        {QStringLiteral("ada example"), QStringLiteral("10-9999-synthetic-gadget-2")},              // authors
        {QStringLiteral("Placeholder Science"), QStringLiteral("10-9999-synthetic-gadget-2")},      // journal
        {QStringLiteral("1999"), QStringLiteral("10-9999-synthetic-gadget-2")},                     // year
        {QStringLiteral("doe2021"), QStringLiteral("10-9999-synthetic-widget-1")},                  // cite_key
        {QStringLiteral("10.9999/synthetic.gadget"), QStringLiteral("10-9999-synthetic-gadget-2")}, // doi
        {QStringLiteral("sprocket"), QStringLiteral("10-9999-synthetic-sprocket-3")},               // slug
    };
    for (const auto &expectation : expectations) {
        filter.setQuery(expectation.first);
        QCOMPARE(filter.rowCount(), 1);
        QCOMPARE(filter.data(filter.index(0, 0), PaperLibraryModel::SlugRole).toString(), expectation.second);
    }

    filter.setQuery(QStringLiteral("zeppelin"));
    QCOMPARE(filter.rowCount(), 0);

    filter.setQuery(QString());
    QCOMPARE(filter.rowCount(), 3);
}

void PaperLibraryModelTest::testResolveWithoutDatabase()
{
    const QString pdfPath = touchFile(QStringLiteral("pdfs/10-9999-synthetic-widget-1.pdf"));
    QVERIFY(!pdfPath.isEmpty());
    const QString corpusDir = writeCatalog({widgetRecord(), gadgetRecord()});

    PaperLibraryModel model;
    QSignalSpy loadedSpy(&model, &PaperLibraryModel::loaded);
    model.load(corpusDir);
    QVERIFY(loadedSpy.wait(10000));

    // Row 0 = widget (newer): its derived pdfs/<slug>.pdf exists
    QCOMPARE(model.resolvePdfPath(0), pdfPath);
    QCOMPARE(model.data(model.index(0), PaperLibraryModel::MissingRole).toBool(), false);

    // Row 1 = gadget: no file and no database — unknown, not greyed,
    // and activation-time resolution comes back empty
    QCOMPARE(model.resolvePdfPath(1), QString());
    QCOMPARE(model.data(model.index(1), PaperLibraryModel::MissingRole).toBool(), false);
}

void PaperLibraryModelTest::testDatabaseEnrichment()
{
    // The widget record's PDF lives at a non-canonical location only the
    // database knows; the gadget record is evicted; the sprocket record is
    // absent from the database and has no derived file either
    const QString customPdf = touchFile(QStringLiteral("books/synthetic-widget-shelved.pdf"));
    QVERIFY(!customPdf.isEmpty());
    const QString corpusDir = writeCatalog({widgetRecord(), gadgetRecord(), untitledRecord()});

    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("papertest_fixture"));
        db.setDatabaseName(m_dir->filePath(QStringLiteral("catalog.db")));
        QVERIFY(db.open());
        QSqlQuery query(db);
        QVERIFY(query.exec(QStringLiteral("CREATE TABLE papers (slug TEXT PRIMARY KEY, pdf_path TEXT, pdf_evicted INTEGER DEFAULT 0, last_accessed TEXT, access_count INTEGER DEFAULT 0, pinned INTEGER DEFAULT 0, cited_by_count INTEGER)")));
        QVERIFY(query.exec(QStringLiteral("INSERT INTO papers VALUES('10-9999-synthetic-widget-1', '%1', 0, NULL, 0, 0, 150)").arg(customPdf)));
        QVERIFY(query.exec(QStringLiteral("INSERT INTO papers VALUES('10-9999-synthetic-gadget-2', NULL, 1, NULL, 0, 0, NULL)")));
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("papertest_fixture"));

    PaperLibraryModel model;
    QSignalSpy loadedSpy(&model, &PaperLibraryModel::loaded);
    model.load(corpusDir);
    QVERIFY(loadedSpy.wait(10000));
    QCOMPARE(model.rowCount(), 3);

    // Widget: found through the database's pdf_path
    QCOMPARE(model.resolvePdfPath(0), customPdf);
    QCOMPARE(model.data(model.index(0), PaperLibraryModel::MissingRole).toBool(), false);

    // Gadget: evicted — known missing, greyed
    QCOMPARE(model.data(model.index(1), PaperLibraryModel::MissingRole).toBool(), true);
    QCOMPARE(model.resolvePdfPath(1), QString());

    // Sprocket: the database answered and nothing resolves — missing
    QCOMPARE(model.data(model.index(2), PaperLibraryModel::MissingRole).toBool(), true);
}

// The catalog.db is opened through a file: URI. When the corpus lives under a
// directory whose name contains characters that are significant in a URI
// (spaces, %, ?, #), a naively concatenated URI is malformed and the database
// silently fails to open — leaving records mislabelled Unknown instead of
// resolved from the database. The path must be percent-encoded.
void PaperLibraryModelTest::testDatabaseInPathWithSpecialCharacters()
{
    const QString weird = m_dir->filePath(QStringLiteral("weird ? # % dir"));
    QVERIFY(QDir().mkpath(weird));

    // A PDF at a non-canonical location only the database knows about
    const QString customPdf = weird + QStringLiteral("/synthetic-widget-shelved.pdf");
    {
        QFile pdf(customPdf);
        QVERIFY(pdf.open(QIODevice::WriteOnly));
        pdf.write("x");
    }

    {
        QFile catalog(weird + QStringLiteral("/catalog.jsonl"));
        QVERIFY(catalog.open(QIODevice::WriteOnly));
        catalog.write(recordLine(widgetRecord()));
        catalog.write(recordLine(gadgetRecord()));
    }

    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("papertest_weird_fixture"));
        db.setDatabaseName(weird + QStringLiteral("/catalog.db"));
        QVERIFY(db.open());
        QSqlQuery query(db);
        QVERIFY(query.exec(QStringLiteral("CREATE TABLE papers (slug TEXT PRIMARY KEY, pdf_path TEXT, pdf_evicted INTEGER DEFAULT 0, last_accessed TEXT, access_count INTEGER DEFAULT 0, pinned INTEGER DEFAULT 0, cited_by_count INTEGER)")));
        QVERIFY(query.exec(QStringLiteral("INSERT INTO papers VALUES('10-9999-synthetic-widget-1', '%1', 0, NULL, 0, 0, NULL)").arg(customPdf)));
        QVERIFY(query.exec(QStringLiteral("INSERT INTO papers VALUES('10-9999-synthetic-gadget-2', NULL, 1, NULL, 0, 0, NULL)")));
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("papertest_weird_fixture"));

    PaperLibraryModel model;
    QSignalSpy loadedSpy(&model, &PaperLibraryModel::loaded);
    model.load(weird);
    QVERIFY(loadedSpy.wait(10000));
    QCOMPARE(model.rowCount(), 2);

    // Widget (row 0, newest): only the database knows customPdf, so resolving
    // it proves the database opened through the special-character path
    QCOMPARE(model.resolvePdfPath(0), customPdf);
    QCOMPARE(model.data(model.index(0), PaperLibraryModel::MissingRole).toBool(), false);

    // Gadget (row 1): the database answered "evicted" — greyed, not Unknown
    QCOMPARE(model.data(model.index(1), PaperLibraryModel::MissingRole).toBool(), true);
}

void PaperLibraryModelTest::testSectionedModelSmartShelves()
{
    const QString mndPdfPath = touchFile(QStringLiteral("pdfs/10-9999-synthetic-mnd-5.pdf"));
    QVERIFY(!mndPdfPath.isEmpty());
    const QString corpusDir = writeCatalog({widgetRecord(),
                                            gadgetRecord(),
                                            textbookRecord(),
                                            mndRecord(),
                                            psychiatryRecord(),
                                            clinicalExamTextbookRecord(),
                                            neuroTextbookRecord(),
                                            paedsTextbookRecord(),
                                            obgynTextbookRecord(),
                                            psychiatryTextbookRecord(),
                                            patientSafetyTextbookRecord(),
                                            anthropologyRecord(),
                                            politicsRecord()});

    PaperLibraryModel model;
    QSignalSpy loadedSpy(&model, &PaperLibraryModel::loaded);
    model.load(corpusDir);
    QVERIFY(loadedSpy.wait(10000));

    PaperLibrarySectionedModel sections;
    sections.setSourceModel(&model);

    sections.setSmartFilter(PaperLibrarySectionedModel::Textbooks);
    QCOMPARE(sections.rowCount(), 7); // Focus mode is packed: no header rows
    QVERIFY(!sections.data(sections.index(0), PaperLibrarySectionedModel::SectionHeaderRole).toBool());

    sections.setSectionMode(PaperLibrarySectionedModel::ByTopic);
    QCOMPARE(sections.rowCount(), 7); // grouped modes still emit tiles only
    QVERIFY(!sections.data(sections.index(0), PaperLibrarySectionedModel::SectionHeaderRole).toBool());

    sections.setSectionMode(PaperLibrarySectionedModel::ReadNext);
    sections.setSmartFilter(PaperLibrarySectionedModel::Mnd);
    QCOMPARE(sections.rowCount(), 1);
    QCOMPARE(sections.data(sections.index(0), Qt::DisplayRole).toString(), QStringLiteral("Neurofilament Biomarkers in Amyotrophic Lateral Sclerosis"));
    QCOMPARE(sections.data(sections.index(0), PaperLibrarySectionedModel::PdfPathRole).toString(), mndPdfPath);
    QCOMPARE(sections.data(sections.index(0), PaperLibrarySectionedModel::ShelfIntentRole).toString(), QStringLiteral("MD project core paper"));
    QCOMPARE(sections.data(sections.index(0), PaperLibrarySectionedModel::RelationHintRole).toString(), QStringLiteral("Linked to MD project review set"));
    QCOMPARE(sections.data(sections.index(0), PaperLibrarySectionedModel::PriorityHintRole).toString(), QStringLiteral("MD project review set"));
    sections.setCoverForPath(mndPdfPath, QStringLiteral("cover-token"), false);
    QCOMPARE(sections.data(sections.index(0), PaperLibrarySectionedModel::CoverPixmapRole).toString(), QStringLiteral("cover-token"));
    QCOMPARE(sections.data(sections.index(0), PaperLibrarySectionedModel::GeneratedCoverRole).toBool(), false);

    sections.setSmartFilter(PaperLibrarySectionedModel::Medicine);
    QCOMPARE(sections.rowCount(), 7); // Medicine is medical textbooks, not every medical paper
    QStringList medicineTitles;
    for (int row = 0; row < sections.rowCount(); ++row) {
        medicineTitles.append(sections.data(sections.index(row), Qt::DisplayRole).toString());
    }
    QCOMPARE(medicineTitles.at(0), QStringLiteral("Talley and O'Connor's Clinical Examination"));
    QCOMPARE(medicineTitles.at(1), QStringLiteral("Neuroanatomy: An Illustrated Colour Text"));
    QCOMPARE(medicineTitles.at(2), QStringLiteral("Nelson Textbook of Pediatrics"));
    QCOMPARE(medicineTitles.at(3), QStringLiteral("Obstetrics and Gynaecology for Medical Students"));
    QCOMPARE(medicineTitles.at(4), QStringLiteral("Shorter Oxford Textbook of Psychiatry"));
    QVERIFY(medicineTitles.indexOf(QStringLiteral("Human Error and Patient Safety")) > medicineTitles.indexOf(QStringLiteral("Fundamentals of Widget Physiology")));
    QCOMPARE(sections.data(sections.index(0), PaperLibrarySectionedModel::ShelfIntentRole).toString(), QStringLiteral("Clinical rotation reference"));
    QCOMPARE(sections.data(sections.index(0), PaperLibrarySectionedModel::RelationHintRole).toString(), QStringLiteral("For clinical placement"));

    sections.setSmartFilter(PaperLibrarySectionedModel::Psychiatry);
    QCOMPARE(sections.rowCount(), 2);
    QStringList psychiatryTitles;
    for (int row = 0; row < sections.rowCount(); ++row) {
        psychiatryTitles.append(sections.data(sections.index(row), Qt::DisplayRole).toString());
    }
    QVERIFY(psychiatryTitles.contains(QStringLiteral("Major Depression and Suicide Risk in Adolescent Psychiatry")));
    QVERIFY(psychiatryTitles.contains(QStringLiteral("Shorter Oxford Textbook of Psychiatry")));
    QCOMPARE(sections.data(sections.index(0), PaperLibrarySectionedModel::FocusRole).toString(), QStringLiteral("Psychiatry"));
    QCOMPARE(sections.data(sections.index(0), PaperLibrarySectionedModel::ThumbnailSeedRole).toString(), QStringLiteral("Psychiatry"));
    QCOMPARE(sections.data(sections.index(0), PaperLibrarySectionedModel::ShelfIntentRole).toString(), QStringLiteral("Psychiatry training"));
    QVERIFY(sections.data(sections.index(0), PaperLibrarySectionedModel::TopicTagsRole).toStringList().contains(QStringLiteral("Psychiatry")));

    sections.setSmartFilter(PaperLibrarySectionedModel::Anthropology);
    QCOMPARE(sections.rowCount(), 1);
    QCOMPARE(sections.data(sections.index(0), PaperLibrarySectionedModel::FocusRole).toString(), QStringLiteral("Anthropology"));
    QCOMPARE(sections.data(sections.index(0), Qt::DisplayRole).toString(), QStringLiteral("Anthropology of Debt and Exchange"));

    sections.setSmartFilter(PaperLibrarySectionedModel::Politics);
    QCOMPARE(sections.rowCount(), 1);
    QCOMPARE(sections.data(sections.index(0), PaperLibrarySectionedModel::FocusRole).toString(), QStringLiteral("Politics"));
    QCOMPARE(sections.data(sections.index(0), Qt::DisplayRole).toString(), QStringLiteral("Politics and Power in Presidential Biography"));

    sections.setSmartFilter(PaperLibrarySectionedModel::Papers);
    sections.setSectionMode(PaperLibrarySectionedModel::ByProject);
    QStringList visibleTitles;
    for (int row = 0; row < sections.rowCount(); ++row) {
        if (!sections.data(sections.index(row), PaperLibrarySectionedModel::SectionHeaderRole).toBool()) {
            visibleTitles.append(sections.data(sections.index(row), Qt::DisplayRole).toString());
        }
    }
    QVERIFY(visibleTitles.contains(QStringLiteral("Neurofilament Biomarkers in Amyotrophic Lateral Sclerosis")));
    QVERIFY(!visibleTitles.contains(QStringLiteral("Fundamentals of Widget Physiology"))); // Books/Textbooks are not duplicated into Papers

    sections.setSectionMode(PaperLibrarySectionedModel::ReadNext);
    auto findTitleRow = [&sections](const QString &title) {
        for (int row = 0; row < sections.rowCount(); ++row) {
            if (!sections.data(sections.index(row), PaperLibrarySectionedModel::SectionHeaderRole).toBool()
                && sections.data(sections.index(row), Qt::DisplayRole).toString() == title) {
                return row;
            }
        }
        return -1;
    };
    const QString mndTitle = QStringLiteral("Neurofilament Biomarkers in Amyotrophic Lateral Sclerosis");
    int mndRow = findTitleRow(mndTitle);
    QVERIFY(mndRow >= 0);
    sections.setDownranked(sections.index(mndRow), true);
    mndRow = findTitleRow(mndTitle);
    QVERIFY(mndRow >= 0);
    QCOMPARE(sections.data(sections.index(mndRow), PaperLibrarySectionedModel::DownrankedRole).toBool(), true);
    QCOMPARE(sections.data(sections.index(sections.rowCount() - 1), Qt::DisplayRole).toString(), mndTitle);
}

void PaperLibraryModelTest::testCaroBiographyDoesNotMatchPsychiatry()
{
    const QString corpusDir = writeCatalog({caroRecord(), psychiatryRecord()});

    PaperLibraryModel model;
    QSignalSpy loadedSpy(&model, &PaperLibraryModel::loaded);
    model.load(corpusDir);
    QVERIFY(loadedSpy.wait(10000));

    PaperLibrarySectionedModel sections;
    sections.setSourceModel(&model);

    sections.setSmartFilter(PaperLibrarySectionedModel::Psychiatry);
    QCOMPARE(sections.rowCount(), 1);
    QCOMPARE(sections.data(sections.index(0), Qt::DisplayRole).toString(), QStringLiteral("Major Depression and Suicide Risk in Adolescent Psychiatry"));

    sections.setSmartFilter(PaperLibrarySectionedModel::Nonfiction);
    QCOMPARE(sections.rowCount(), 1);
    QCOMPARE(sections.data(sections.index(0), Qt::DisplayRole).toString(), QStringLiteral("The Path to Power"));
    QCOMPARE(sections.data(sections.index(0), PaperLibrarySectionedModel::FocusRole).toString(), QStringLiteral("Politics"));
    QCOMPARE(sections.data(sections.index(0), PaperLibrarySectionedModel::ShelfIntentRole).toString(), QStringLiteral("Political biography"));
    QCOMPARE(sections.data(sections.index(0), PaperLibrarySectionedModel::RelationHintRole).toString(), QStringLiteral("Linked to LBJ / US power"));
    QVERIFY(sections.data(sections.index(0), PaperLibrarySectionedModel::TopicTagsRole).toStringList().contains(QStringLiteral("Politics")));
}

void PaperLibraryModelTest::testReloadIfChanged()
{
    const QString corpusDir = writeCatalog({widgetRecord()});

    PaperLibraryModel model;
    QSignalSpy loadedSpy(&model, &PaperLibraryModel::loaded);
    model.load(corpusDir);
    QVERIFY(loadedSpy.wait(10000));
    QCOMPARE(model.rowCount(), 1);

    // Same mtime: nothing to do
    model.reloadIfChanged();
    QTest::qWait(100);
    QCOMPARE(loadedSpy.count(), 1);

    // A grown catalog with a moved mtime is picked up
    writeCatalog({widgetRecord(), gadgetRecord()});
    QFile catalog(m_dir->filePath(QStringLiteral("catalog.jsonl")));
    QVERIFY(catalog.open(QIODevice::ReadWrite));
    QVERIFY(catalog.setFileTime(QDateTime::currentDateTime().addSecs(10), QFileDevice::FileModificationTime));
    catalog.close();

    model.reloadIfChanged();
    QVERIFY(loadedSpy.wait(10000));
    QCOMPARE(loadedSpy.count(), 2);
    QCOMPARE(model.rowCount(), 2);
}

// Destroying the model while the parse worker is still in flight must reclaim
// the worker thread deterministically. The finished() handler that would
// deleteLater() the thread runs on the event loop, which never spins between
// load() and delete here — so if the destructor relied solely on that handler
// (whose receiver is being torn down) the QThread would leak.
void PaperLibraryModelTest::testDestroyDuringLoadReclaimsWorker()
{
    const QString corpusDir = writeCatalog({widgetRecord(), gadgetRecord(), untitledRecord()});

    auto *model = new PaperLibraryModel;
    model->load(corpusDir);

    QThread *worker = model->m_worker;
    QVERIFY(worker); // load() spun up a worker and has not yet cleaned it up

    bool workerDestroyed = false;
    QObject::connect(worker, &QObject::destroyed, [&workerDestroyed]() { workerDestroyed = true; });

    // No event-loop spin has occurred, so the finished() handler cannot have
    // run: the destructor is solely responsible for reclaiming the worker.
    delete model;
    QVERIFY(workerDestroyed);
}

// Destroying the model after a completed load, once the event loop has run the
// finished() handler, must not crash or double-free.
void PaperLibraryModelTest::testDestroyAfterLoad()
{
    const QString corpusDir = writeCatalog({widgetRecord(), gadgetRecord()});

    auto *model = new PaperLibraryModel;
    QSignalSpy loadedSpy(model, &PaperLibraryModel::loaded);
    model->load(corpusDir);
    QVERIFY(loadedSpy.wait(10000));
    // Let the queued finished() handler run and deleteLater() the worker
    QTRY_VERIFY(model->m_worker == nullptr);

    delete model;  // destructor is a no-op on the already-reclaimed worker
    QVERIFY(true); // reached here without crashing
}

QTEST_GUILESS_MAIN(PaperLibraryModelTest)
#include "paperlibrarymodeltest.moc"
