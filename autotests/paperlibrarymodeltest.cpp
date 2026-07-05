/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <QTest>

#include <QDir>
#include <QFile>
#include <QJsonArray>
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

static SyntheticRecord mndDiagnosisRecord()
{
    return {QStringLiteral("10-9999-synthetic-mnd-diagnosis-16"),
            QStringLiteral("10.9999/synthetic.mnd.diagnosis"),
            QString(),
            QStringLiteral("sample2026mnddiagnosis"),
            QStringLiteral("Awaji ALS Criteria and Electrodiagnosis in Amyotrophic Lateral Sclerosis"),
            QStringLiteral("Casey Clinician"),
            QStringLiteral("2026"),
            QStringLiteral("Journal of Synthetic Neurology"),
            161616,
            QStringLiteral("synthetic"),
            QStringLiteral("2026-04-02T00:00:00+00:00")};
}

static SyntheticRecord mndHyperexcitabilityRecord()
{
    return {QStringLiteral("10-9999-synthetic-mnd-excitability-17"),
            QStringLiteral("10.9999/synthetic.mnd.excitability"),
            QString(),
            QStringLiteral("sample2026mndexcitability"),
            QStringLiteral("Cortical Hyperexcitability Precedes Lower Motor Neuron Dysfunction in ALS"),
            QStringLiteral("Morgan Motor"),
            QStringLiteral("2026"),
            QStringLiteral("Journal of Synthetic Neurophysiology"),
            171717,
            QStringLiteral("synthetic"),
            QStringLiteral("2026-04-03T00:00:00+00:00")};
}

static SyntheticRecord mndDiagnosticBiomarkerRecord()
{
    return {QStringLiteral("10-9999-synthetic-mnd-diagnostic-biomarker-20"),
            QStringLiteral("10.9999/synthetic.mnd.diagnostic.biomarker"),
            QString(),
            QStringLiteral("sample2026mnddiagnosticbiomarker"),
            QStringLiteral("Diagnostic Accuracy of Serum Neurofilament Light Chain in ALS"),
            QStringLiteral("Bailey Biomarker"),
            QStringLiteral("2026"),
            QStringLiteral("Journal of Synthetic Biomarkers"),
            202020,
            QStringLiteral("synthetic"),
            QStringLiteral("2026-04-05T00:00:00+00:00")};
}

static SyntheticRecord mndThresholdTrackingRecord()
{
    return {QStringLiteral("10-9999-synthetic-mnd-threshold-tracking-21"),
            QStringLiteral("10.9999/synthetic.mnd.threshold.tracking"),
            QString(),
            QStringLiteral("sample2026mndthresholdtracking"),
            QStringLiteral("Threshold Tracking Nerve Conduction Study of Split-Hand Impairment in ALS"),
            QStringLiteral("Elliot Electrode"),
            QStringLiteral("2026"),
            QStringLiteral("Journal of Synthetic Neurophysiology"),
            212121,
            QStringLiteral("synthetic"),
            QStringLiteral("2026-04-06T00:00:00+00:00")};
}

static SyntheticRecord mndTreatmentRecord()
{
    return {QStringLiteral("10-9999-synthetic-mnd-treatment-18"),
            QStringLiteral("10.9999/synthetic.mnd.treatment"),
            QString(),
            QStringLiteral("sample2026mndtreatment"),
            QStringLiteral("Riluzole Treatment Trial Design in Amyotrophic Lateral Sclerosis"),
            QStringLiteral("Taylor Trialist"),
            QStringLiteral("2026"),
            QStringLiteral("Journal of Synthetic Trials"),
            181818,
            QStringLiteral("synthetic"),
            QStringLiteral("2026-04-04T00:00:00+00:00")};
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
    void testMndPaperTopicInferencePrefersPaperSpecificSignals();
    void testPapersShelfSurfacesInterestNoveltyAndEngagement();
    void testFocusManifestDrivesWorkShelf();
    void testReadingManifestDrivesReadingShelves();
    void testCaroBiographyDoesNotMatchPsychiatry();
    void testSectionedModelSuppressesDuplicateWorks();
    void testImportedBookMetadataIsCleanedAndReclassified();
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
                                            mndDiagnosisRecord(),
                                            mndHyperexcitabilityRecord(),
                                            mndTreatmentRecord(),
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
    QCOMPARE(sections.rowCount(), 4);
    QCOMPARE(sections.data(sections.index(0), Qt::DisplayRole).toString(), QStringLiteral("Neurofilament Biomarkers in Amyotrophic Lateral Sclerosis"));
    QCOMPARE(sections.data(sections.index(0), PaperLibrarySectionedModel::PdfPathRole).toString(), mndPdfPath);
    QCOMPARE(sections.data(sections.index(0), PaperLibrarySectionedModel::ShelfIntentRole).toString(), QStringLiteral("MD project core paper"));
    QCOMPARE(sections.data(sections.index(0), PaperLibrarySectionedModel::RelationHintRole).toString(), QStringLiteral("Linked to MD project review set"));
    QCOMPARE(sections.data(sections.index(0), PaperLibrarySectionedModel::PriorityHintRole).toString(), QStringLiteral("MD project review set"));
    QCOMPARE(sections.data(sections.index(1), Qt::DisplayRole).toString(), QStringLiteral("Awaji ALS Criteria and Electrodiagnosis in Amyotrophic Lateral Sclerosis"));
    QCOMPARE(sections.data(sections.index(1), PaperLibrarySectionedModel::FocusRole).toString(), QStringLiteral("Diagnosis & Criteria"));
    QCOMPARE(sections.data(sections.index(1), PaperLibrarySectionedModel::ShelfIntentRole).toString(), QStringLiteral("Diagnosis / criteria paper"));
    QCOMPARE(sections.data(sections.index(1), PaperLibrarySectionedModel::RelationHintRole).toString(), QStringLiteral("Use for diagnostic framing"));
    QCOMPARE(sections.data(sections.index(2), Qt::DisplayRole).toString(), QStringLiteral("Cortical Hyperexcitability Precedes Lower Motor Neuron Dysfunction in ALS"));
    QCOMPARE(sections.data(sections.index(2), PaperLibrarySectionedModel::FocusRole).toString(), QStringLiteral("Neurophysiology / Hyperexcitability"));
    QCOMPARE(sections.data(sections.index(3), Qt::DisplayRole).toString(), QStringLiteral("Riluzole Treatment Trial Design in Amyotrophic Lateral Sclerosis"));
    QCOMPARE(sections.data(sections.index(3), PaperLibrarySectionedModel::ShelfIntentRole).toString(), QStringLiteral("Treatment / trial paper"));
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

void PaperLibraryModelTest::testMndPaperTopicInferencePrefersPaperSpecificSignals()
{
    const QString corpusDir = writeCatalog({mndDiagnosticBiomarkerRecord(), mndThresholdTrackingRecord(), mndDiagnosisRecord()});

    PaperLibraryModel model;
    QSignalSpy loadedSpy(&model, &PaperLibraryModel::loaded);
    model.load(corpusDir);
    QVERIFY(loadedSpy.wait(10000));

    PaperLibrarySectionedModel sections;
    sections.setSourceModel(&model);
    sections.setSmartFilter(PaperLibrarySectionedModel::Mnd);

    auto findTitleRow = [&sections](const QString &title) {
        for (int row = 0; row < sections.rowCount(); ++row) {
            if (sections.data(sections.index(row), Qt::DisplayRole).toString() == title) {
                return row;
            }
        }
        return -1;
    };

    const int biomarkerRow = findTitleRow(QStringLiteral("Diagnostic Accuracy of Serum Neurofilament Light Chain in ALS"));
    QVERIFY(biomarkerRow >= 0);
    QModelIndex biomarkerIndex = sections.index(biomarkerRow);
    QCOMPARE(sections.data(biomarkerIndex, PaperLibrarySectionedModel::FocusRole).toString(), QStringLiteral("Biomarkers & Neurofilament"));
    QCOMPARE(sections.data(biomarkerIndex, PaperLibrarySectionedModel::ShelfIntentRole).toString(), QStringLiteral("Biomarker candidate"));
    QCOMPARE(sections.data(biomarkerIndex, PaperLibrarySectionedModel::RelationHintRole).toString(), QStringLiteral("Use for biomarker evidence"));
    QVERIFY(sections.data(biomarkerIndex, PaperLibrarySectionedModel::TopicTagsRole).toStringList().contains(QStringLiteral("Biomarkers & Neurofilament")));

    const int thresholdRow = findTitleRow(QStringLiteral("Threshold Tracking Nerve Conduction Study of Split-Hand Impairment in ALS"));
    QVERIFY(thresholdRow >= 0);
    QModelIndex thresholdIndex = sections.index(thresholdRow);
    QCOMPARE(sections.data(thresholdIndex, PaperLibrarySectionedModel::FocusRole).toString(), QStringLiteral("Neurophysiology / Hyperexcitability"));
    QCOMPARE(sections.data(thresholdIndex, PaperLibrarySectionedModel::ShelfIntentRole).toString(), QStringLiteral("Electrophysiology / excitability paper"));
    QCOMPARE(sections.data(thresholdIndex, PaperLibrarySectionedModel::RelationHintRole).toString(), QStringLiteral("Use for electrophysiology evidence"));
    QVERIFY(sections.data(thresholdIndex, PaperLibrarySectionedModel::TopicTagsRole).toStringList().contains(QStringLiteral("Neurophysiology / Hyperexcitability")));

    const int awajiRow = findTitleRow(QStringLiteral("Awaji ALS Criteria and Electrodiagnosis in Amyotrophic Lateral Sclerosis"));
    QVERIFY(awajiRow >= 0);
    QCOMPARE(sections.data(sections.index(awajiRow), PaperLibrarySectionedModel::FocusRole).toString(), QStringLiteral("Diagnosis & Criteria"));
}

void PaperLibraryModelTest::testPapersShelfSurfacesInterestNoveltyAndEngagement()
{
    SyntheticRecord engaged = widgetRecord();
    engaged.slug = QStringLiteral("10-9999-synthetic-engaged-paper-22");
    engaged.doi = QStringLiteral("10.9999/synthetic.engaged.paper");
    engaged.title = QStringLiteral("Previously Opened Study of Clinical Measurement");
    engaged.authors = QStringLiteral("Riley Reader");
    engaged.journal = QStringLiteral("Journal of Synthetic Clinical Science");
    engaged.source = QStringLiteral("unpaywall");
    engaged.addedTs = QStringLiteral("2026-01-01T00:00:00+00:00");

    SyntheticRecord activeWork = gadgetRecord();
    activeWork.slug = QStringLiteral("10-9999-synthetic-beyond-bayes-paper-23");
    activeWork.doi = QStringLiteral("10.9999/synthetic.beyond.bayes.paper");
    activeWork.title = QStringLiteral("Dimensionality Bound for High-Dimensional Coherence");
    activeWork.authors = QStringLiteral("Robin Reviewer");
    activeWork.journal = QStringLiteral("Synthetic Methods");
    activeWork.source = QStringLiteral("highdimensional");
    activeWork.addedTs = QStringLiteral("2026-01-02T00:00:00+00:00");

    SyntheticRecord rotation = psychiatryRecord();
    rotation.slug = QStringLiteral("10-9999-synthetic-rotation-paper-24");
    rotation.doi = QStringLiteral("10.9999/synthetic.rotation.paper");
    rotation.title = QStringLiteral("Adolescent Psychiatry Consultation Patterns During Paediatric Admission");
    rotation.source = QStringLiteral("unpaywall");
    rotation.addedTs = QStringLiteral("2026-01-03T00:00:00+00:00");

    SyntheticRecord methods = gadgetRecord();
    methods.slug = QStringLiteral("10-9999-synthetic-methods-paper-25");
    methods.doi = QStringLiteral("10.9999/synthetic.methods.paper");
    methods.title = QStringLiteral("Bayesian Prediction Model Calibration for Diagnostic Studies");
    methods.authors = QStringLiteral("Morgan Methods");
    methods.journal = QStringLiteral("Synthetic Epidemiology");
    methods.source = QStringLiteral("unpaywall");
    methods.addedTs = QStringLiteral("2026-01-04T00:00:00+00:00");

    SyntheticRecord novelty = gadgetRecord();
    novelty.slug = QStringLiteral("10-9999-synthetic-novelty-paper-26");
    novelty.doi = QStringLiteral("10.9999/synthetic.novelty.paper");
    novelty.title = QStringLiteral("Bioelectric Anticipatory Systems in Neural Morphogenesis");
    novelty.authors = QStringLiteral("Avery Adjacent");
    novelty.journal = QStringLiteral("BioSystems");
    novelty.source = QStringLiteral("unpaywall");
    novelty.addedTs = QStringLiteral("2026-01-05T00:00:00+00:00");

    SyntheticRecord generic = gadgetRecord();
    generic.slug = QStringLiteral("10-9999-synthetic-generic-paper-27");
    generic.doi = QStringLiteral("10.9999/synthetic.generic.paper");
    generic.title = QStringLiteral("Generic Widget Oscillations in Placeholder Materials");
    generic.authors = QStringLiteral("Grey Generic");
    generic.journal = QStringLiteral("Annals of Placeholder Science");
    generic.source = QStringLiteral("unpaywall");
    generic.addedTs = QStringLiteral("2026-07-01T00:00:00+00:00");

    QList<SyntheticRecord> records = {generic, novelty, methods, rotation, mndRecord(), activeWork, engaged};
    for (int i = 0; i < 10; ++i) {
        SyntheticRecord extraMnd = mndRecord();
        extraMnd.slug = QStringLiteral("10-9999-synthetic-extra-mnd-paper-%1").arg(i);
        extraMnd.doi = QStringLiteral("10.9999/synthetic.extra.mnd.%1").arg(i);
        extraMnd.title = QStringLiteral("Amyotrophic Lateral Sclerosis Adjacent Cohort Paper %1").arg(i + 1);
        extraMnd.addedTs = QStringLiteral("2026-01-%1T00:00:00+00:00").arg(10 + i, 2, 10, QLatin1Char('0'));
        records.append(extraMnd);
    }
    const QString corpusDir = writeCatalog(records);
    for (const SyntheticRecord &record : records) {
        QVERIFY(!touchFile(QStringLiteral("pdfs/%1.pdf").arg(record.slug)).isEmpty());
    }

    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("papertest_interest_fixture"));
        db.setDatabaseName(m_dir->filePath(QStringLiteral("catalog.db")));
        QVERIFY(db.open());
        QSqlQuery query(db);
        QVERIFY(query.exec(QStringLiteral("CREATE TABLE papers (slug TEXT PRIMARY KEY, pdf_path TEXT, pdf_evicted INTEGER DEFAULT 0, last_accessed TEXT, access_count INTEGER DEFAULT 0, pinned INTEGER DEFAULT 0, cited_by_count INTEGER)")));
        QVERIFY(query.exec(QStringLiteral("INSERT INTO papers VALUES('10-9999-synthetic-engaged-paper-22', '%1', 0, '2026-07-01T00:00:00+00:00', 3, 0, 5)")
                               .arg(m_dir->filePath(QStringLiteral("pdfs/10-9999-synthetic-engaged-paper-22.pdf")))));
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("papertest_interest_fixture"));

    PaperLibraryModel model;
    QSignalSpy loadedSpy(&model, &PaperLibraryModel::loaded);
    model.load(corpusDir);
    QVERIFY(loadedSpy.wait(10000));

    PaperLibrarySectionedModel sections;
    sections.setSourceModel(&model);
    sections.setSmartFilter(PaperLibrarySectionedModel::Papers);
    sections.setSectionMode(PaperLibrarySectionedModel::ReadNext);

    auto findTitleRow = [&sections](const QString &title) {
        for (int row = 0; row < sections.rowCount(); ++row) {
            if (sections.data(sections.index(row), Qt::DisplayRole).toString() == title) {
                return row;
            }
        }
        return -1;
    };

    const int engagedRow = findTitleRow(engaged.title);
    const int activeRow = findTitleRow(activeWork.title);
    const int mndRow = findTitleRow(QStringLiteral("Neurofilament Biomarkers in Amyotrophic Lateral Sclerosis"));
    const int rotationRow = findTitleRow(rotation.title);
    const int methodsRow = findTitleRow(methods.title);
    const int noveltyRow = findTitleRow(novelty.title);
    const int genericRow = findTitleRow(generic.title);
    QVERIFY(engagedRow >= 0);
    QVERIFY(activeRow >= 0);
    QVERIFY(mndRow >= 0);
    QVERIFY(rotationRow >= 0);
    QVERIFY(methodsRow >= 0);
    QVERIFY(noveltyRow >= 0);
    QVERIFY(genericRow >= 0);

    QVERIFY(engagedRow < activeRow);
    QVERIFY(activeRow < mndRow);
    QVERIFY(mndRow < rotationRow);
    QVERIFY(rotationRow < methodsRow);
    QVERIFY(methodsRow < noveltyRow);
    QVERIFY(noveltyRow < genericRow);

    QModelIndex engagedIndex = sections.index(engagedRow);
    QCOMPARE(sections.data(engagedIndex, PaperLibrarySectionedModel::ShelfIntentRole).toString(), QStringLiteral("Continue reading"));
    QCOMPARE(sections.data(engagedIndex, PaperLibrarySectionedModel::RelationHintRole).toString(), QStringLiteral("Opened before; keep warm"));

    QModelIndex activeIndex = sections.index(activeRow);
    QCOMPARE(sections.data(activeIndex, PaperLibrarySectionedModel::ShelfIntentRole).toString(), QStringLiteral("Active work reading"));
    QCOMPARE(sections.data(activeIndex, PaperLibrarySectionedModel::RelationHintRole).toString(), QStringLiteral("Connected to Beyond Bayes / revisions"));

    QModelIndex noveltyIndex = sections.index(noveltyRow);
    QCOMPARE(sections.data(noveltyIndex, PaperLibrarySectionedModel::ShelfIntentRole).toString(), QStringLiteral("Novel adjacent idea"));
    QCOMPARE(sections.data(noveltyIndex, PaperLibrarySectionedModel::RelationHintRole).toString(), QStringLiteral("Explore adjacent ideas"));

    int visibleMndPapers = 0;
    for (int row = 0; row < sections.rowCount(); ++row) {
        const QString title = sections.data(sections.index(row), Qt::DisplayRole).toString();
        if (title.contains(QStringLiteral("Amyotrophic Lateral Sclerosis")) || title.contains(QStringLiteral("Neurofilament Biomarkers"))) {
            ++visibleMndPapers;
        }
    }
    QCOMPARE(visibleMndPapers, 6);
}

void PaperLibraryModelTest::testFocusManifestDrivesWorkShelf()
{
    SyntheticRecord work = widgetRecord();
    work.slug = QStringLiteral("10-9999-synthetic-beyond-bayes-work");
    work.doi = QStringLiteral("10.9999/synthetic.beyond.bayes.work");
    work.title = QStringLiteral("Beyond Bayes Revision Notes for High-Dimensional Inference");
    work.authors = QStringLiteral("Robin Reviewer");
    work.journal = QStringLiteral("Synthetic Methods");
    work.source = QStringLiteral("unpaywall");

    SyntheticRecord falsePositive = gadgetRecord();
    falsePositive.slug = QStringLiteral("10-9999-synthetic-biosystems-false-positive");
    falsePositive.doi = QStringLiteral("10.9999/synthetic.biosystems.false.positive");
    falsePositive.title = QStringLiteral("Bayesian prediction model for hospital readmission");
    falsePositive.journal = QStringLiteral("BioSystems");
    falsePositive.source = QStringLiteral("aa_fast_download");

    const QString corpusDir = writeCatalog({work, falsePositive});
    const QString curatedPdf = touchFile(QStringLiteral("pdfs/10-9999-synthetic-beyond-bayes-work.pdf"));
    const QString looseResponse = touchFile(QStringLiteral("loose/Response_to_reviewers.pdf"));
    const QString backgroundPdf = touchFile(QStringLiteral("pdfs/background-method.pdf"));

    QDir(corpusDir).mkpath(QStringLiteral("focus/Work"));
    QJsonArray manifest;
    QJsonObject background;
    background.insert(QStringLiteral("id"), QStringLiteral("file-background-method"));
    background.insert(QStringLiteral("title"), QStringLiteral("Background Method Paper"));
    background.insert(QStringLiteral("path"), backgroundPdf);
    background.insert(QStringLiteral("kind"), QStringLiteral("pdf"));
    background.insert(QStringLiteral("score"), 999.0);
    background.insert(QStringLiteral("reason"), QStringLiteral("background method"));
    background.insert(QStringLiteral("shelf"), QStringLiteral("Work"));
    background.insert(QStringLiteral("section"), QStringLiteral("03-background-methods"));
    manifest.append(background);

    QJsonObject curated;
    curated.insert(QStringLiteral("id"), work.slug);
    curated.insert(QStringLiteral("title"), QStringLiteral("Curated Beyond Bayes Draft"));
    curated.insert(QStringLiteral("path"), curatedPdf);
    curated.insert(QStringLiteral("kind"), QStringLiteral("pdf"));
    curated.insert(QStringLiteral("authors"), work.authors);
    curated.insert(QStringLiteral("year"), work.year);
    curated.insert(QStringLiteral("journal"), work.journal);
    curated.insert(QStringLiteral("source"), work.source);
    curated.insert(QStringLiteral("doi"), work.doi);
    curated.insert(QStringLiteral("score"), 120.0);
    curated.insert(QStringLiteral("reason"), QStringLiteral("Beyond Bayes manuscript; Bayesian/FEP literature"));
    curated.insert(QStringLiteral("shelf"), QStringLiteral("Work"));
    curated.insert(QStringLiteral("section"), QStringLiteral("00-beyond-bayes-revision"));
    manifest.append(curated);

    QJsonObject loose;
    loose.insert(QStringLiteral("id"), QStringLiteral("file-synthetic-review-response"));
    loose.insert(QStringLiteral("title"), QStringLiteral("Reviewer Response"));
    loose.insert(QStringLiteral("path"), looseResponse);
    loose.insert(QStringLiteral("kind"), QStringLiteral("pdf"));
    loose.insert(QStringLiteral("score"), 80.0);
    loose.insert(QStringLiteral("reason"), QStringLiteral("review/revision work"));
    loose.insert(QStringLiteral("shelf"), QStringLiteral("Work"));
    loose.insert(QStringLiteral("section"), QStringLiteral("00-beyond-bayes-revision"));
    manifest.append(loose);

    QFile manifestFile(QDir(corpusDir).filePath(QStringLiteral("focus/Work/manifest.json")));
    QVERIFY(manifestFile.open(QIODevice::WriteOnly));
    manifestFile.write(QJsonDocument(manifest).toJson(QJsonDocument::Compact));
    manifestFile.close();

    PaperLibraryModel model;
    QSignalSpy loadedSpy(&model, &PaperLibraryModel::loaded);
    model.load(corpusDir);
    QVERIFY(loadedSpy.wait(10000));

    PaperLibrarySectionedModel sections;
    sections.setSourceModel(&model);
    sections.setSmartFilter(PaperLibrarySectionedModel::Work);

    QCOMPARE(sections.rowCount(), 3);
    QCOMPARE(sections.data(sections.index(0), Qt::DisplayRole).toString(), QStringLiteral("Curated Beyond Bayes Draft"));
    QCOMPARE(sections.data(sections.index(0), PaperLibrarySectionedModel::FocusRole).toString(), QStringLiteral("Beyond Bayes Revision"));
    QCOMPARE(sections.data(sections.index(0), PaperLibrarySectionedModel::ShelfIntentRole).toString(), QStringLiteral("Beyond Bayes manuscript"));
    QCOMPARE(sections.data(sections.index(0), PaperLibrarySectionedModel::RelationHintRole).toString(), QStringLiteral("Bayesian/FEP literature"));
    QCOMPARE(sections.data(sections.index(1), Qt::DisplayRole).toString(), QStringLiteral("Reviewer Response"));
    QCOMPARE(sections.resolvePath(sections.index(1)), looseResponse);
    QCOMPARE(sections.data(sections.index(2), Qt::DisplayRole).toString(), QStringLiteral("Background Method Paper"));
    QCOMPARE(sections.resolvePath(sections.index(2)), backgroundPdf);

    QStringList visibleTitles;
    for (int row = 0; row < sections.rowCount(); ++row) {
        visibleTitles.append(sections.data(sections.index(row), Qt::DisplayRole).toString());
    }
    QVERIFY(!visibleTitles.contains(falsePositive.title));
}

void PaperLibraryModelTest::testReadingManifestDrivesReadingShelves()
{
    const QString corpusDir = writeCatalog({});
    const QString fictionEpub = touchFile(QStringLiteral("reading/A_Game_Of_Thrones.epub"));
    const QString currentNonfictionEpub = touchFile(QStringLiteral("reading/Everything_Was_Forever.epub"));
    const QString caroEpub = touchFile(QStringLiteral("reading/The_Path_To_Power.epub"));
    const QString graeberPdf = touchFile(QStringLiteral("reading/Bullshit_Jobs.pdf"));
    const QString unrelatedPdf = touchFile(QStringLiteral("reading/Unrelated_Methods_Paper.pdf"));

    QDir(corpusDir).mkpath(QStringLiteral("focus/Reading"));
    QJsonArray manifest;
    auto appendReadingEntry = [&manifest](const QString &id, const QString &title, const QString &path, const QString &kind, const QString &source, const QString &reason, const QString &section) {
        QJsonObject object;
        object.insert(QStringLiteral("id"), id);
        object.insert(QStringLiteral("title"), title);
        object.insert(QStringLiteral("path"), path);
        object.insert(QStringLiteral("kind"), kind);
        object.insert(QStringLiteral("source"), source);
        object.insert(QStringLiteral("reason"), reason);
        object.insert(QStringLiteral("shelf"), QStringLiteral("Reading"));
        object.insert(QStringLiteral("section"), section);
        manifest.append(object);
    };
    appendReadingEntry(QStringLiteral("file-fiction-current"), QStringLiteral("A Game Of Thrones 52314094"), fictionEpub, QStringLiteral("epub"), QStringLiteral("app-recent"), QStringLiteral("current fiction; recently opened"), QStringLiteral("00-fiction-current"));
    appendReadingEntry(QStringLiteral("file-nonfiction-current"), QStringLiteral("Everything Was Forever Until It Was No More 2f314c74"), currentNonfictionEpub, QStringLiteral("epub"), QStringLiteral("app-recent"), QStringLiteral("current nonfiction; recently opened"), QStringLiteral("03-nonfiction-current"));
    appendReadingEntry(QStringLiteral("file-caro"), QStringLiteral("The Path to Power"), caroEpub, QStringLiteral("epub"), QStringLiteral("book:epub"), QStringLiteral("Caro/LBJ nonfiction"), QStringLiteral("01-nonfiction-politics"));
    appendReadingEntry(QStringLiteral("file-graeber"), QStringLiteral("Bullshit Jobs"), graeberPdf, QStringLiteral("pdf"), QStringLiteral("book:pdf"), QStringLiteral("anthropology/nonfiction"), QStringLiteral("02-nonfiction-anthropology"));
    appendReadingEntry(QStringLiteral("file-unrelated"), QStringLiteral("Unrelated Methods Paper"), unrelatedPdf, QStringLiteral("pdf"), QStringLiteral("synthetic"), QStringLiteral("methods paper"), QStringLiteral("04-other"));

    QFile manifestFile(QDir(corpusDir).filePath(QStringLiteral("focus/Reading/manifest.json")));
    QVERIFY(manifestFile.open(QIODevice::WriteOnly));
    manifestFile.write(QJsonDocument(manifest).toJson(QJsonDocument::Compact));
    manifestFile.close();

    PaperLibraryModel model;
    QSignalSpy loadedSpy(&model, &PaperLibraryModel::loaded);
    model.load(corpusDir);
    QVERIFY(loadedSpy.wait(10000));

    PaperLibrarySectionedModel sections;
    sections.setSourceModel(&model);

    sections.setSmartFilter(PaperLibrarySectionedModel::Books);
    QCOMPARE(sections.rowCount(), 3);
    QCOMPARE(sections.data(sections.index(0), Qt::DisplayRole).toString(), QStringLiteral("A Game of Thrones"));
    QCOMPARE(sections.data(sections.index(0), PaperLibrarySectionedModel::KindRole).toString(), QStringLiteral("EPUB"));
    QCOMPARE(sections.resolvePath(sections.index(0)), fictionEpub);
    QCOMPARE(sections.data(sections.index(1), Qt::DisplayRole).toString(), QStringLiteral("Everything Was Forever Until It Was No More"));
    QCOMPARE(sections.data(sections.index(2), Qt::DisplayRole).toString(), QStringLiteral("The Path to Power"));

    sections.setSmartFilter(PaperLibrarySectionedModel::Fiction);
    QCOMPARE(sections.rowCount(), 1);
    QCOMPARE(sections.data(sections.index(0), Qt::DisplayRole).toString(), QStringLiteral("A Game of Thrones"));
    QCOMPARE(sections.data(sections.index(0), PaperLibrarySectionedModel::FocusRole).toString(), QStringLiteral("Fiction Current"));
    QCOMPARE(sections.data(sections.index(0), PaperLibrarySectionedModel::ShelfIntentRole).toString(), QStringLiteral("current fiction"));

    sections.setSmartFilter(PaperLibrarySectionedModel::Nonfiction);
    QCOMPARE(sections.rowCount(), 3);
    QStringList nonfictionTitles;
    for (int row = 0; row < sections.rowCount(); ++row) {
        nonfictionTitles.append(sections.data(sections.index(row), Qt::DisplayRole).toString());
    }
    QVERIFY(!nonfictionTitles.contains(QStringLiteral("A Game of Thrones")));
    QVERIFY(nonfictionTitles.contains(QStringLiteral("Everything Was Forever Until It Was No More")));
    QVERIFY(nonfictionTitles.contains(QStringLiteral("The Path to Power")));
    QVERIFY(nonfictionTitles.contains(QStringLiteral("Bullshit Jobs")));
    QVERIFY(!nonfictionTitles.contains(QStringLiteral("Unrelated Methods Paper")));
    QCOMPARE(sections.resolvePath(sections.index(nonfictionTitles.indexOf(QStringLiteral("Bullshit Jobs")))), graeberPdf);
}

void PaperLibraryModelTest::testCaroBiographyDoesNotMatchPsychiatry()
{
    SyntheticRecord genericNonfiction = gadgetRecord();
    genericNonfiction.slug = QStringLiteral("md5-synthetic-nonfiction-history-19");
    genericNonfiction.title = QStringLiteral("Nonfiction History Essays");
    genericNonfiction.authors = QStringLiteral("Casey Essayist");
    genericNonfiction.year = QStringLiteral("2020");
    genericNonfiction.journal = QStringLiteral("(book)");
    genericNonfiction.source = QStringLiteral("book:epub");

    const QString corpusDir = writeCatalog({caroRecord(), psychiatryRecord(), genericNonfiction});

    PaperLibraryModel model;
    QSignalSpy loadedSpy(&model, &PaperLibraryModel::loaded);
    model.load(corpusDir);
    QVERIFY(loadedSpy.wait(10000));

    PaperLibrarySectionedModel sections;
    sections.setSourceModel(&model);

    sections.setSmartFilter(PaperLibrarySectionedModel::Psychiatry);
    QCOMPARE(sections.rowCount(), 1);
    QCOMPARE(sections.data(sections.index(0), Qt::DisplayRole).toString(), QStringLiteral("Major Depression and Suicide Risk in Adolescent Psychiatry"));

    sections.setSmartFilter(PaperLibrarySectionedModel::Fiction);
    QCOMPARE(sections.rowCount(), 0);

    sections.setSmartFilter(PaperLibrarySectionedModel::Nonfiction);
    QCOMPARE(sections.rowCount(), 2);
    QStringList nonfictionTitles;
    for (int row = 0; row < sections.rowCount(); ++row) {
        nonfictionTitles.append(sections.data(sections.index(row), Qt::DisplayRole).toString());
    }
    const int caroRow = nonfictionTitles.indexOf(QStringLiteral("The Path to Power"));
    QVERIFY(caroRow >= 0);
    QVERIFY(nonfictionTitles.contains(QStringLiteral("Nonfiction History Essays")));
    QCOMPARE(sections.data(sections.index(caroRow), PaperLibrarySectionedModel::FocusRole).toString(), QStringLiteral("Politics"));
    QCOMPARE(sections.data(sections.index(caroRow), PaperLibrarySectionedModel::ShelfIntentRole).toString(), QStringLiteral("Political biography"));
    QCOMPARE(sections.data(sections.index(caroRow), PaperLibrarySectionedModel::RelationHintRole).toString(), QStringLiteral("Linked to LBJ / US power"));
    QVERIFY(sections.data(sections.index(caroRow), PaperLibrarySectionedModel::TopicTagsRole).toStringList().contains(QStringLiteral("Politics")));
}

void PaperLibraryModelTest::testSectionedModelSuppressesDuplicateWorks()
{
    SyntheticRecord dawnNoisy = anthropologyRecord();
    dawnNoisy.slug = QStringLiteral("md5-synthetic-dawn-noisy");
    dawnNoisy.title = QStringLiteral("The Dawn of Everything A New History of Humanity David Graeber, David Wengrow First american edition, 2021 Farrar, Straus and Giroux Anna's Archive");
    dawnNoisy.authors.clear();
    dawnNoisy.year.clear();
    dawnNoisy.source = QStringLiteral("book:pdf");

    SyntheticRecord dawnClean = anthropologyRecord();
    dawnClean.slug = QStringLiteral("md5-synthetic-dawn-clean");
    dawnClean.title = QStringLiteral("The Dawn of Everything");
    dawnClean.authors = QStringLiteral("David Graeber; David Wengrow");
    dawnClean.year = QStringLiteral("2021");
    dawnClean.source = QStringLiteral("book:epub");
    dawnClean.addedTs = QStringLiteral("2026-05-01T00:00:00+00:00");

    SyntheticRecord debtSlug = anthropologyRecord();
    debtSlug.slug = QStringLiteral("md5-synthetic-ref13-graeber");
    debtSlug.title = QStringLiteral("ref13 graeber 2011");
    debtSlug.authors.clear();
    debtSlug.year.clear();
    debtSlug.source = QStringLiteral("book:pdf");

    SyntheticRecord debtClean = anthropologyRecord();
    debtClean.slug = QStringLiteral("md5-synthetic-debt-clean");
    debtClean.title = QStringLiteral("David Graeber Debt The First 5,000 Years Melville House (2011)");
    debtClean.authors.clear();
    debtClean.year.clear();
    debtClean.source = QStringLiteral("book:epub");

    SyntheticRecord bullshitShort = anthropologyRecord();
    bullshitShort.slug = QStringLiteral("md5-synthetic-bullshit-jobs-short");
    bullshitShort.title = QStringLiteral("Bullshit Jobs");
    bullshitShort.authors = QStringLiteral("David Graeber");
    bullshitShort.year = QStringLiteral("2018");
    bullshitShort.source = QStringLiteral("book:epub");

    SyntheticRecord bullshitLong = anthropologyRecord();
    bullshitLong.slug = QStringLiteral("md5-synthetic-bullshit-jobs-long");
    bullshitLong.title = QStringLiteral("David Graeber Bullshit Jobs A Theory Simon Schuster (2018)");
    bullshitLong.authors.clear();
    bullshitLong.year.clear();
    bullshitLong.source = QStringLiteral("book:pdf");

    SyntheticRecord whyWork = anthropologyRecord();
    whyWork.slug = QStringLiteral("md5-synthetic-why-work");
    whyWork.title = QStringLiteral("Why Work? Arguments for the Leisure Society");
    whyWork.authors = QStringLiteral("David Graeber; Stanley Aronowitz");
    whyWork.year = QStringLiteral("2018");
    whyWork.source = QStringLiteral("book:pdf");

    SyntheticRecord dawnCommentary = anthropologyRecord();
    dawnCommentary.slug = QStringLiteral("md5-synthetic-dawn-commentary");
    dawnCommentary.title = QStringLiteral("Introduction to Special Issue: Leading Scholars Comment on Dawn of Everything");
    dawnCommentary.authors = QStringLiteral("Daniel Hoyer");
    dawnCommentary.year = QStringLiteral("2022");
    dawnCommentary.journal = QStringLiteral("Synthetic Cliodynamics");
    dawnCommentary.source = QStringLiteral("imported");

    SyntheticRecord powerBroker = politicsRecord();
    powerBroker.slug = QStringLiteral("md5-synthetic-power-broker");
    powerBroker.title = QStringLiteral("The Power Broker Political Biography");
    powerBroker.authors = QStringLiteral("Robert A. Caro");
    powerBroker.year = QStringLiteral("1974");
    powerBroker.source = QStringLiteral("book:epub");

    const QString corpusDir = writeCatalog({dawnNoisy, dawnClean, debtSlug, debtClean, bullshitShort, bullshitLong, whyWork, dawnCommentary, caroRecord(), powerBroker});

    PaperLibraryModel model;
    QSignalSpy loadedSpy(&model, &PaperLibraryModel::loaded);
    model.load(corpusDir);
    QVERIFY(loadedSpy.wait(10000));

    PaperLibrarySectionedModel sections;
    sections.setSourceModel(&model);
    sections.setSmartFilter(PaperLibrarySectionedModel::Nonfiction);

    QStringList nonfictionTitles;
    for (int row = 0; row < sections.rowCount(); ++row) {
        nonfictionTitles.append(sections.data(sections.index(row), Qt::DisplayRole).toString());
    }

    QCOMPARE(nonfictionTitles.count(QStringLiteral("The Dawn of Everything")), 1);
    QCOMPARE(nonfictionTitles.count(QStringLiteral("Debt: The First 5,000 Years")), 1);
    QCOMPARE(nonfictionTitles.count(QStringLiteral("Bullshit Jobs")), 1);
    QVERIFY(nonfictionTitles.contains(QStringLiteral("Why Work?")));
    QVERIFY2(nonfictionTitles.contains(QStringLiteral("Introduction to Special Issue: Leading Scholars Comment on Dawn of Everything")), qPrintable(nonfictionTitles.join(QLatin1Char('\n'))));
    QVERIFY(nonfictionTitles.contains(QStringLiteral("The Path to Power")));
    QVERIFY(nonfictionTitles.contains(QStringLiteral("The Power Broker")));
}

void PaperLibraryModelTest::testImportedBookMetadataIsCleanedAndReclassified()
{
    SyntheticRecord warHistory = gadgetRecord();
    warHistory.slug = QStringLiteral("md5-synthetic-war-history-19");
    warHistory.title = QStringLiteral("1941 The America That Went To War");
    warHistory.authors = QStringLiteral("William M. Christie");
    warHistory.year = QStringLiteral("2015");
    warHistory.journal = QStringLiteral("(book)");
    warHistory.source = QStringLiteral("book:epub");
    warHistory.citeKey = QStringLiteral("christie1941war");

    SyntheticRecord noisyGraeber = anthropologyRecord();
    noisyGraeber.slug = QStringLiteral("md5-synthetic-graeber-noisy");
    noisyGraeber.title = QStringLiteral("The Dawn of Everything A New History of Humanity David Graeber, David Wengrow First american edition, 2021 11 09 Farrar, Straus and Giroux 9780141991061 80b8ab295de7310780edd18c6cbb768c Anna's Archive");

    SyntheticRecord ref13Graeber = anthropologyRecord();
    ref13Graeber.slug = QStringLiteral("md5-synthetic-ref13-graeber");
    ref13Graeber.title = QStringLiteral("ref13 graeber 2011");
    ref13Graeber.authors.clear();
    ref13Graeber.year.clear();
    ref13Graeber.journal = QStringLiteral("(book)");
    ref13Graeber.source = QStringLiteral("book:pdf");

    SyntheticRecord sandCounty = gadgetRecord();
    sandCounty.slug = QStringLiteral("md5-synthetic-sand-county");
    sandCounty.title = QStringLiteral("Aldo Leopold_ A Sand County Almanac & Other Writings on");
    sandCounty.authors = QStringLiteral("Aldo Leopold; Curt Meine");
    sandCounty.year = QStringLiteral("2013");
    sandCounty.journal = QStringLiteral("(book)");
    sandCounty.source = QStringLiteral("book:epub");

    SyntheticRecord parable = gadgetRecord();
    parable.slug = QStringLiteral("md5-synthetic-parable");
    parable.title = QStringLiteral("Octavia Butler - Parable 01");
    parable.authors = QStringLiteral("Octavia Butler");
    parable.year = QStringLiteral("1993");
    parable.journal = QStringLiteral("(book)");
    parable.source = QStringLiteral("book:epub");

    SyntheticRecord noisyPsychBook = gadgetRecord();
    noisyPsychBook.slug = QStringLiteral("md5-synthetic-noisy-psych-book");
    noisyPsychBook.title = QStringLiteral("The psychiatric interview    Carlat, Daniel J     Books@Ovid , Fourth edition , 2017    Lippincott Williams & Wilkins Wolters Kluwer    isbn13 9781496327710    18f8cca535c356506ff7a04809905f5c    Anna’s Archive");
    noisyPsychBook.authors.clear();
    noisyPsychBook.year.clear();
    noisyPsychBook.journal = QStringLiteral("(book)");
    noisyPsychBook.source = QStringLiteral("book:epub");

    SyntheticRecord authorTitleBook = gadgetRecord();
    authorTitleBook.slug = QStringLiteral("md5-synthetic-author-title-book");
    authorTitleBook.title = QStringLiteral("Susan P. Mattern   The Prince of Medicine  Galen in the Roman Empire (2013, Oxford University Press)   libgen.lc");
    authorTitleBook.authors.clear();
    authorTitleBook.year.clear();
    authorTitleBook.journal = QStringLiteral("(book)");
    authorTitleBook.source = QStringLiteral("book:epub");

    SyntheticRecord commaAuthorBook = gadgetRecord();
    commaAuthorBook.slug = QStringLiteral("md5-synthetic-comma-author-book");
    commaAuthorBook.title = QStringLiteral("Craig Stanford, John S. Allen, Susan C. Anton   Biological Anthropology  The Natural History of Humankind, 3rd Edition   (2011, Pearson Education, Inc.)   libgen.lc");
    commaAuthorBook.authors.clear();
    commaAuthorBook.year.clear();
    commaAuthorBook.journal = QStringLiteral("(book)");
    commaAuthorBook.source = QStringLiteral("book:pdf");

    SyntheticRecord titleSubtitleAuthorBook = gadgetRecord();
    titleSubtitleAuthorBook.slug = QStringLiteral("md5-synthetic-title-subtitle-author-book");
    titleSubtitleAuthorBook.title = QStringLiteral("Mad in America   Bad Science, Bad Medicine, and the Enduring Mistreatment of the Mentally Ill    Robert Whitaker    Third trade paperback edition, New York, 2019    Basic Books    isbn13 9780738203850    245fe4fef83fca5ed4146ca49f76d704    Anna’s Archive");
    titleSubtitleAuthorBook.authors.clear();
    titleSubtitleAuthorBook.year.clear();
    titleSubtitleAuthorBook.journal = QStringLiteral("(book)");
    titleSubtitleAuthorBook.source = QStringLiteral("book:epub");

    SyntheticRecord singleSpacedAuthorTitleBook = gadgetRecord();
    singleSpacedAuthorTitleBook.slug = QStringLiteral("md5-synthetic-single-spaced-author-title-book");
    singleSpacedAuthorTitleBook.title = QStringLiteral("Richard Wrangham The Goodness Paradox The Strange Relationship Between Virtue and Violence in Human Evolution Pantheon Books (29 Jan 2019)");
    singleSpacedAuthorTitleBook.authors.clear();
    singleSpacedAuthorTitleBook.year.clear();
    singleSpacedAuthorTitleBook.journal = QStringLiteral("(book)");
    singleSpacedAuthorTitleBook.source = QStringLiteral("book:epub");

    SyntheticRecord pathomaStyleBook = gadgetRecord();
    pathomaStyleBook.slug = QStringLiteral("md5-synthetic-pathoma-style-book");
    pathomaStyleBook.title = QStringLiteral("Sattar Husain Fundamentals of Pathology (2021)");
    pathomaStyleBook.authors.clear();
    pathomaStyleBook.year.clear();
    pathomaStyleBook.journal = QStringLiteral("(book)");
    pathomaStyleBook.source = QStringLiteral("book:pdf");

    SyntheticRecord multiAuthorNoCommaBook = gadgetRecord();
    multiAuthorNoCommaBook.slug = QStringLiteral("md5-synthetic-multi-author-no-comma-book");
    multiAuthorNoCommaBook.title = QStringLiteral("Thomas M. Cover Joy A. Thomas Elements of Information Theory Wiley (2012)");
    multiAuthorNoCommaBook.authors.clear();
    multiAuthorNoCommaBook.year.clear();
    multiAuthorNoCommaBook.journal = QStringLiteral("(book)");
    multiAuthorNoCommaBook.source = QStringLiteral("book:epub");

    SyntheticRecord debtAuthorTitleBook = gadgetRecord();
    debtAuthorTitleBook.slug = QStringLiteral("md5-synthetic-debt-author-title-book");
    debtAuthorTitleBook.title = QStringLiteral("David Graeber Debt The First 5,000 Years Melville House (2011)");
    debtAuthorTitleBook.authors.clear();
    debtAuthorTitleBook.year.clear();
    debtAuthorTitleBook.journal = QStringLiteral("(book)");
    debtAuthorTitleBook.source = QStringLiteral("book:epub");

    SyntheticRecord moneyAuthorTitleBook = gadgetRecord();
    moneyAuthorTitleBook.slug = QStringLiteral("md5-synthetic-money-author-title-book");
    moneyAuthorTitleBook.title = QStringLiteral("Richard Seaford Money and the Early Greek Mind Homer, Philosophy, Tragedy Cambridge University Press (2004)");
    moneyAuthorTitleBook.authors.clear();
    moneyAuthorTitleBook.year.clear();
    moneyAuthorTitleBook.journal = QStringLiteral("(book)");
    moneyAuthorTitleBook.source = QStringLiteral("book:pdf");

    SyntheticRecord cyberneticsAuthorTitleBook = gadgetRecord();
    cyberneticsAuthorTitleBook.slug = QStringLiteral("md5-synthetic-cybernetics-author-title-book");
    cyberneticsAuthorTitleBook.title = QStringLiteral("Norbert Wiener Cybernetics, or Control and Communication in the Animal and the Machine MIT Press (2019)");
    cyberneticsAuthorTitleBook.authors.clear();
    cyberneticsAuthorTitleBook.year.clear();
    cyberneticsAuthorTitleBook.journal = QStringLiteral("(book)");
    cyberneticsAuthorTitleBook.source = QStringLiteral("book:pdf");

    SyntheticRecord dawnCommentary = gadgetRecord();
    dawnCommentary.slug = QStringLiteral("10-9999-synthetic-dawn-commentary");
    dawnCommentary.title = QStringLiteral("Introduction to Special Issue: Leading Scholars Comment on Dawn of Everything");
    dawnCommentary.authors = QStringLiteral("Daniel Hoyer");
    dawnCommentary.year = QStringLiteral("2022");
    dawnCommentary.journal = QStringLiteral("Synthetic Cliodynamics");
    dawnCommentary.source = QStringLiteral("imported");

    const QString corpusDir =
        writeCatalog({warHistory,
                      noisyGraeber,
                      ref13Graeber,
                      sandCounty,
                      parable,
                      noisyPsychBook,
                      authorTitleBook,
                      commaAuthorBook,
                      titleSubtitleAuthorBook,
                      singleSpacedAuthorTitleBook,
                      pathomaStyleBook,
                      multiAuthorNoCommaBook,
                      debtAuthorTitleBook,
                      moneyAuthorTitleBook,
                      cyberneticsAuthorTitleBook,
                      dawnCommentary,
                      psychiatryRecord()});

    PaperLibraryModel model;
    QSignalSpy loadedSpy(&model, &PaperLibraryModel::loaded);
    model.load(corpusDir);
    QVERIFY(loadedSpy.wait(10000));

    PaperLibrarySectionedModel sections;
    sections.setSourceModel(&model);

    sections.setSmartFilter(PaperLibrarySectionedModel::Psychiatry);
    QStringList psychiatryTitles;
    for (int row = 0; row < sections.rowCount(); ++row) {
        psychiatryTitles.append(sections.data(sections.index(row), Qt::DisplayRole).toString());
    }
    QVERIFY(psychiatryTitles.contains(QStringLiteral("Major Depression and Suicide Risk in Adolescent Psychiatry")));
    QVERIFY(psychiatryTitles.contains(QStringLiteral("The psychiatric interview")));

    sections.setSmartFilter(PaperLibrarySectionedModel::Nonfiction);
    QStringList nonfictionTitles;
    for (int row = 0; row < sections.rowCount(); ++row) {
        nonfictionTitles.append(sections.data(sections.index(row), Qt::DisplayRole).toString());
    }
    QVERIFY2(nonfictionTitles.contains(QStringLiteral("1941: The America That Went to War")), qPrintable(nonfictionTitles.join(QLatin1Char('\n'))));
    QVERIFY(nonfictionTitles.contains(QStringLiteral("The Dawn of Everything")));
    QVERIFY(nonfictionTitles.contains(QStringLiteral("Debt: The First 5,000 Years")));
    QVERIFY(nonfictionTitles.contains(QStringLiteral("A Sand County Almanac & Other Writings on Ecology and Conservation")));
    QVERIFY(nonfictionTitles.contains(QStringLiteral("Introduction to Special Issue: Leading Scholars Comment on Dawn of Everything")));
    QVERIFY(!nonfictionTitles.contains(QStringLiteral("1941")));
    QVERIFY(!nonfictionTitles.contains(QStringLiteral("ref13 graeber 2011")));
    QVERIFY(!nonfictionTitles.join(QLatin1Char('\n')).contains(QStringLiteral("Anna")));
    QVERIFY(!nonfictionTitles.join(QLatin1Char('\n')).contains(QStringLiteral("libgen")));
    QVERIFY(!nonfictionTitles.join(QLatin1Char('\n')).contains(QStringLiteral("978")));

    sections.setSmartFilter(PaperLibrarySectionedModel::Fiction);
    QStringList fictionTitles;
    for (int row = 0; row < sections.rowCount(); ++row) {
        fictionTitles.append(sections.data(sections.index(row), Qt::DisplayRole).toString());
    }
    QVERIFY(fictionTitles.contains(QStringLiteral("Parable of the Sower")));

    auto rowForTitle = [&model](const QString &title) {
        for (int row = 0; row < model.rowCount(); ++row) {
            if (model.data(model.index(row), Qt::DisplayRole).toString() == title) {
                return row;
            }
        }
        return -1;
    };
    auto rowForTitleAndAuthor = [&model](const QString &title, const QString &author) {
        for (int row = 0; row < model.rowCount(); ++row) {
            if (model.data(model.index(row), Qt::DisplayRole).toString() == title && model.data(model.index(row), PaperLibraryModel::AuthorsRole).toString() == author) {
                return row;
            }
        }
        return -1;
    };
    const int psychBookRow = rowForTitle(QStringLiteral("The psychiatric interview"));
    QVERIFY(psychBookRow >= 0);
    QCOMPARE(model.data(model.index(psychBookRow), PaperLibraryModel::AuthorsRole).toString(), QStringLiteral("Carlat, Daniel J"));
    QCOMPARE(model.data(model.index(psychBookRow), PaperLibraryModel::YearRole).toString(), QStringLiteral("2017"));

    const int princeRow = rowForTitle(QStringLiteral("The Prince of Medicine Galen in the Roman Empire"));
    QStringList allTitles;
    for (int row = 0; row < model.rowCount(); ++row) {
        allTitles.append(model.data(model.index(row), Qt::DisplayRole).toString());
    }
    QVERIFY2(princeRow >= 0, qPrintable(allTitles.join(QLatin1Char('\n'))));
    QCOMPARE(model.data(model.index(princeRow), PaperLibraryModel::AuthorsRole).toString(), QStringLiteral("Susan P. Mattern"));
    QCOMPARE(model.data(model.index(princeRow), PaperLibraryModel::YearRole).toString(), QStringLiteral("2013"));

    const int anthropologyRow = rowForTitle(QStringLiteral("Biological Anthropology The Natural History of Humankind, 3rd Edition"));
    QVERIFY2(anthropologyRow >= 0, qPrintable(allTitles.join(QLatin1Char('\n'))));
    QCOMPARE(model.data(model.index(anthropologyRow), PaperLibraryModel::AuthorsRole).toString(), QStringLiteral("Craig Stanford, John S. Allen, Susan C. Anton"));
    QCOMPARE(model.data(model.index(anthropologyRow), PaperLibraryModel::YearRole).toString(), QStringLiteral("2011"));

    const int madRow = rowForTitle(QStringLiteral("Mad in America: Bad Science, Bad Medicine, and the Enduring Mistreatment of the Mentally Ill"));
    QVERIFY2(madRow >= 0, qPrintable(allTitles.join(QLatin1Char('\n'))));
    QCOMPARE(model.data(model.index(madRow), PaperLibraryModel::AuthorsRole).toString(), QStringLiteral("Robert Whitaker"));
    QCOMPARE(model.data(model.index(madRow), PaperLibraryModel::YearRole).toString(), QStringLiteral("2019"));

    const int goodnessRow = rowForTitle(QStringLiteral("The Goodness Paradox The Strange Relationship Between Virtue and Violence in Human Evolution"));
    QVERIFY2(goodnessRow >= 0, qPrintable(allTitles.join(QLatin1Char('\n'))));
    QCOMPARE(model.data(model.index(goodnessRow), PaperLibraryModel::AuthorsRole).toString(), QStringLiteral("Richard Wrangham"));
    QCOMPARE(model.data(model.index(goodnessRow), PaperLibraryModel::YearRole).toString(), QStringLiteral("2019"));

    const int pathologyRow = rowForTitle(QStringLiteral("Fundamentals of Pathology"));
    QVERIFY2(pathologyRow >= 0, qPrintable(allTitles.join(QLatin1Char('\n'))));
    QCOMPARE(model.data(model.index(pathologyRow), PaperLibraryModel::AuthorsRole).toString(), QStringLiteral("Sattar Husain"));
    QCOMPARE(model.data(model.index(pathologyRow), PaperLibraryModel::YearRole).toString(), QStringLiteral("2021"));

    const int informationTheoryRow = rowForTitle(QStringLiteral("Elements of Information Theory"));
    QVERIFY2(informationTheoryRow >= 0, qPrintable(allTitles.join(QLatin1Char('\n'))));
    QCOMPARE(model.data(model.index(informationTheoryRow), PaperLibraryModel::AuthorsRole).toString(), QStringLiteral("Thomas M. Cover Joy A. Thomas"));
    QCOMPARE(model.data(model.index(informationTheoryRow), PaperLibraryModel::YearRole).toString(), QStringLiteral("2012"));

    const int debtRow = rowForTitleAndAuthor(QStringLiteral("Debt: The First 5,000 Years"), QStringLiteral("David Graeber"));
    QVERIFY2(debtRow >= 0, qPrintable(allTitles.join(QLatin1Char('\n'))));
    QCOMPARE(model.data(model.index(debtRow), PaperLibraryModel::AuthorsRole).toString(), QStringLiteral("David Graeber"));
    QCOMPARE(model.data(model.index(debtRow), PaperLibraryModel::YearRole).toString(), QStringLiteral("2011"));

    const int moneyRow = rowForTitle(QStringLiteral("Money and the Early Greek Mind Homer, Philosophy, Tragedy"));
    QVERIFY2(moneyRow >= 0, qPrintable(allTitles.join(QLatin1Char('\n'))));
    QCOMPARE(model.data(model.index(moneyRow), PaperLibraryModel::AuthorsRole).toString(), QStringLiteral("Richard Seaford"));
    QCOMPARE(model.data(model.index(moneyRow), PaperLibraryModel::YearRole).toString(), QStringLiteral("2004"));

    const int cyberneticsRow = rowForTitle(QStringLiteral("Cybernetics, or Control and Communication in the Animal and the Machine"));
    QVERIFY2(cyberneticsRow >= 0, qPrintable(allTitles.join(QLatin1Char('\n'))));
    QCOMPARE(model.data(model.index(cyberneticsRow), PaperLibraryModel::AuthorsRole).toString(), QStringLiteral("Norbert Wiener"));
    QCOMPARE(model.data(model.index(cyberneticsRow), PaperLibraryModel::YearRole).toString(), QStringLiteral("2019"));
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
