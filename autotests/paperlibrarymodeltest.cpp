/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <QTest>

#include <QDir>
#include <QCryptographicHash>
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
    QString genre = {};
    QString recordKind = {};
    QString normTitle = {};
    QString description = {};
    QStringList topics = {};
    QString readingLevel = {};
    QString subgenre = {};
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
    if (!record.genre.isEmpty()) {
        object.insert(QStringLiteral("genre"), record.genre);
    }
    if (!record.recordKind.isEmpty()) {
        object.insert(QStringLiteral("record_kind"), record.recordKind);
    }
    if (!record.normTitle.isEmpty()) {
        object.insert(QStringLiteral("norm_title"), record.normTitle);
    }
    if (!record.description.isEmpty()) {
        object.insert(QStringLiteral("description"), record.description);
    }
    if (!record.topics.isEmpty()) {
        object.insert(QStringLiteral("topics"), QJsonArray::fromStringList(record.topics));
    }
    if (!record.readingLevel.isEmpty()) {
        object.insert(QStringLiteral("reading_level"), record.readingLevel);
    }
    if (!record.subgenre.isEmpty()) {
        object.insert(QStringLiteral("subgenre"), record.subgenre);
    }
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}

static void writeCorpusHealthState(const QString &corpusDir,
                                   bool healthy,
                                   int catalogRows,
                                   bool searchFresh,
                                   bool graphFresh,
                                   const QStringList &issues = {})
{
    QFile catalog(QDir(corpusDir).filePath(QStringLiteral("catalog.jsonl")));
    QVERIFY(catalog.open(QIODevice::ReadOnly));
    const QByteArray catalogBytes = catalog.readAll();
    catalog.close();
    const QString manifestHash = QString::fromLatin1(
        QCryptographicHash::hash(catalogBytes, QCryptographicHash::Sha256).toHex());
    const QString catalogRevision = QStringLiteral("sha256:test-catalog");
    const QString manifestSourceRevision = QStringLiteral("sha256:test-manifest-source");
    QJsonObject manifestMetadata{
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("catalog_revision"), catalogRevision},
        {QStringLiteral("manifest_source_revision"), manifestSourceRevision},
        {QStringLiteral("rows"), catalogRows},
        {QStringLiteral("manifest_sha256"), manifestHash},
    };
    QFile metadataFile(QDir(corpusDir).filePath(QStringLiteral("catalog.meta.json")));
    QVERIFY(metadataFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QVERIFY(metadataFile.write(QJsonDocument(manifestMetadata).toJson(QJsonDocument::Compact)) > 0);
    metadataFile.close();

    QJsonArray issueArray;
    for (const QString &issue : issues) {
        issueArray.append(issue);
    }
    QJsonObject artifacts;
    artifacts.insert(QStringLiteral("manifest"), QJsonObject{
        {QStringLiteral("fresh"), true},
        {QStringLiteral("sha256"), manifestHash},
        {QStringLiteral("manifest_source_revision"), manifestSourceRevision},
    });
    artifacts.insert(QStringLiteral("search"), QJsonObject{{QStringLiteral("fresh"), searchFresh}});
    artifacts.insert(QStringLiteral("graph"), QJsonObject{{QStringLiteral("fresh"), graphFresh}});
    QJsonObject state;
    state.insert(QStringLiteral("schema_version"), 1);
    state.insert(QStringLiteral("generated_at"), QStringLiteral("2026-07-10T00:00:00Z"));
    state.insert(QStringLiteral("healthy"), healthy);
    state.insert(QStringLiteral("issues"), issueArray);
    state.insert(QStringLiteral("warnings"), QJsonArray());
    state.insert(QStringLiteral("catalog"), QJsonObject{
        {QStringLiteral("rows"), catalogRows},
        {QStringLiteral("revision"), catalogRevision},
    });
    state.insert(QStringLiteral("artifacts"), artifacts);
    QFile file(QDir(corpusDir).filePath(QStringLiteral("corpus_state.json")));
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(file.write(QJsonDocument(state).toJson(QJsonDocument::Compact)) > 0, true);
    file.close();
}

class PaperLibraryModelTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();

    void testParseCatalogFields();
    void testLibrarianMetadataDrivesTileRoles();
    void testParseSkipsMalformedLines();
    void testSortNewestFirst();
    void testCorpusExists();
    void testConfiguredCorpusDir();
    void testAsyncLoadPopulatesModel();
    void testLoadMissingCorpusYieldsEmptyModel();
    void testFilterModel();
    void testResolveWithoutDatabase();
    void testDatabaseEnrichment();
    void testDatabaseEnrichmentReadsUncheckpointedWal();
    void testDatabaseInPathWithSpecialCharacters();
    void testFullTextSearchUsesCatalogIndex();
    void testFullTextSearchUsesDedicatedSearchDb();
    void testSemanticGraphRelatedRows();
    void testSemanticGraphUsesDedicatedGraphDb();
    void testCorpusHealthReadsPublishedFreshness();
    void testGenreDrivesShelfClassification();
    void testRecordKindRoutesBooksAndNormTitle();
    void testAdjacentExplicitRowsKeepBooks();
    void testExplicitSearchRowsStayTileFirstAndShelfScoped();
    void testSectionedModelSmartShelves();
    void testSectionedModelInfiniteScrollKeepsCorpusBehindFocus();
    void testSectionedModelInfersCorpusThumbnailAssetPath();
    void testMndPaperTopicInferencePrefersPaperSpecificSignals();
    void testPapersShelfSurfacesInterestNoveltyAndEngagement();
    void testFictionAndNonfictionShelvesNeverContainPapers();
    void testBookGenreRescuesARowMislabelledAsAPaper();
    void testAClinicalAbstractFromAPaperFeedNeverReachesBooks();
    void testFinishedBooksAreOffTheReadingShelves();
    void testSettingTheSameLocalBooksDoesNotResetTheModel();
    void testFocusManifestDrivesWorkShelf();
    void testFocusManifestResolvesRelativePath();
    void testFocusManifestInfersThumbnailAssetPath();
    void testReadingManifestDrivesReadingShelves();
    void testAFinishedBookIsExcludedFromTheBookFeed();
    void testTitleLookupFindsACorpusTwinWithABlurb();
    void testAFileBackedFeedBookCanBeMarkedFinishedViaItsCorpusTwin();
    void testFictionShelfRejectsNovelPaperFalsePositives();
    void testCaroBiographyDoesNotMatchPsychiatry();
    void testBookShelfMetadataDoesNotLeakProjectClassifiers();
    void testSectionedModelSuppressesDuplicateWorks();
    void testImportedBookMetadataIsCleanedAndReclassified();
    void testStaleLoadGenerationIsIgnored();
    void testReloadFromLoadedKeepsNewestWorker();
    void testReloadIfChanged();
    void testDestroyDuringLoadReclaimsWorker();
    void testDestroyAfterLoad();
    void testDownrankStateSynchronizesAcrossModels();

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

void PaperLibraryModelTest::testLibrarianMetadataDrivesTileRoles()
{
    SyntheticRecord enriched = widgetRecord();
    enriched.description = QStringLiteral("A backend-authored synopsis of the synthetic evidence.");
    enriched.topics = {QStringLiteral("Causal widgets"), QStringLiteral("Measurement theory")};
    enriched.readingLevel = QStringLiteral("professional");
    enriched.subgenre = QStringLiteral("Evidence synthesis");
    const QString corpusDir = writeCatalog({enriched});

    PaperLibraryModel model;
    QSignalSpy loadedSpy(&model, &PaperLibraryModel::loaded);
    model.load(corpusDir);
    QVERIFY(loadedSpy.wait(10000));
    QCOMPARE(model.rowCount(), 1);
    const QModelIndex sourceIndex = model.index(0);
    QCOMPARE(sourceIndex.data(PaperLibraryModel::DescriptionRole).toString(), enriched.description);
    QCOMPARE(sourceIndex.data(PaperLibraryModel::TopicsRole).toStringList(), enriched.topics);
    QCOMPARE(sourceIndex.data(PaperLibraryModel::ReadingLevelRole).toString(), enriched.readingLevel);
    QCOMPARE(sourceIndex.data(PaperLibraryModel::SubgenreRole).toString(), enriched.subgenre);
    QVERIFY(sourceIndex.data(PaperLibraryModel::HaystackRole).toString().contains(QStringLiteral("measurement theory")));

    PaperLibrarySectionedModel sections;
    sections.setSourceModel(&model);
    sections.setSmartFilter(PaperLibrarySectionedModel::Papers);
    QModelIndex tile;
    for (int row = 0; row < sections.rowCount(); ++row) {
        const QModelIndex candidate = sections.index(row);
        if (!candidate.data(PaperLibrarySectionedModel::SectionHeaderRole).toBool()) {
            tile = candidate;
            break;
        }
    }
    QVERIFY(tile.isValid());
    QCOMPARE(tile.data(PaperLibrarySectionedModel::TopicTagsRole).toStringList(), enriched.topics);
    QCOMPARE(tile.data(PaperLibrarySectionedModel::ShelfIntentRole).toString(), enriched.description);
    QCOMPARE(tile.data(PaperLibrarySectionedModel::PriorityHintRole).toString(), enriched.readingLevel);
    QCOMPARE(tile.data(PaperLibrarySectionedModel::RelationHintRole).toString(), enriched.subgenre);
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

void PaperLibraryModelTest::testDatabaseEnrichmentReadsUncheckpointedWal()
{
    const QString customPdf = touchFile(QStringLiteral("books/wal-only-widget.pdf"));
    QVERIFY(!customPdf.isEmpty());
    const QString corpusDir = writeCatalog({widgetRecord()});
    const QString dbPath = m_dir->filePath(QStringLiteral("catalog.db"));
    const QString connectionName = QStringLiteral("papertest_wal_fixture");

    {
        // Keep the writer open with its committed row only in the WAL. An immutable reader ignores
        // that file and sees the checkpointed schema but no row; a normal read-only connection sees
        // the committed pdf_path immediately.
        QSqlDatabase writer = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        writer.setDatabaseName(dbPath);
        QVERIFY(writer.open());
        QSqlQuery query(writer);
        QVERIFY(query.exec(QStringLiteral("PRAGMA journal_mode=WAL")));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toString().toLower(), QStringLiteral("wal"));
        query.finish();
        QVERIFY(query.exec(QStringLiteral("PRAGMA wal_autocheckpoint=0")));
        query.finish();
        QVERIFY(query.exec(QStringLiteral("CREATE TABLE papers (slug TEXT PRIMARY KEY, pdf_path TEXT, pdf_evicted INTEGER DEFAULT 0, last_accessed TEXT, access_count INTEGER DEFAULT 0, pinned INTEGER DEFAULT 0, cited_by_count INTEGER)")));
        query.finish();
        QVERIFY(query.exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)")));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toInt(), 0);
        query.finish();
        query.prepare(QStringLiteral("INSERT INTO papers VALUES(?, ?, 0, NULL, 7, 0, NULL)"));
        query.addBindValue(widgetRecord().slug);
        query.addBindValue(customPdf);
        QVERIFY(query.exec());
        query.finish();
        QVERIFY(QFileInfo(dbPath + QStringLiteral("-wal")).size() > 0);

        PaperLibraryModel model;
        QSignalSpy loadedSpy(&model, &PaperLibraryModel::loaded);
        model.load(corpusDir);
        QVERIFY(loadedSpy.wait(10000));
        QCOMPARE(model.rowCount(), 1);
        QCOMPARE(model.resolvePdfPath(0), customPdf);
        QCOMPARE(model.data(model.index(0), PaperLibraryModel::AccessCountRole).toInt(), 7);

        writer.close();
    }
    QSqlDatabase::removeDatabase(connectionName);
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

void PaperLibraryModelTest::testFullTextSearchUsesCatalogIndex()
{
    const QString corpusDir = writeCatalog({widgetRecord(), mndRecord(), gadgetRecord()});

    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("papertest_fts_fixture"));
        db.setDatabaseName(m_dir->filePath(QStringLiteral("catalog.db")));
        QVERIFY(db.open());
        QSqlQuery query(db);
        QVERIFY(query.exec(QStringLiteral("CREATE TABLE papers (slug TEXT PRIMARY KEY, pdf_path TEXT, pdf_evicted INTEGER DEFAULT 0, last_accessed TEXT, access_count INTEGER DEFAULT 0, pinned INTEGER DEFAULT 0, cited_by_count INTEGER)")));
        QVERIFY(query.exec(QStringLiteral("INSERT INTO papers(slug,pdf_path,pdf_evicted,last_accessed,access_count,pinned,cited_by_count) VALUES('10-9999-synthetic-widget-1', NULL, 0, NULL, 0, 0, NULL)")));
        QVERIFY(query.exec(QStringLiteral("INSERT INTO papers(slug,pdf_path,pdf_evicted,last_accessed,access_count,pinned,cited_by_count) VALUES('10-9999-synthetic-mnd-5', NULL, 0, NULL, 0, 0, NULL)")));
        QVERIFY(query.exec(QStringLiteral("INSERT INTO papers(slug,pdf_path,pdf_evicted,last_accessed,access_count,pinned,cited_by_count) VALUES('10-9999-synthetic-gadget-2', NULL, 0, NULL, 0, 0, NULL)")));
        QVERIFY(query.exec(QStringLiteral("CREATE TABLE paper_search_rows (rowid INTEGER PRIMARY KEY, slug TEXT NOT NULL UNIQUE)")));
        QVERIFY(query.exec(QStringLiteral("CREATE VIRTUAL TABLE paper_fts USING fts5(title, authors, year, journal, doi, source, body, content='', tokenize='unicode61 remove_diacritics 2', prefix='2 3 4')")));
        QVERIFY(query.exec(QStringLiteral("INSERT INTO paper_search_rows(rowid,slug) VALUES(1,'10-9999-synthetic-widget-1')")));
        QVERIFY(query.exec(QStringLiteral("INSERT INTO paper_search_rows(rowid,slug) VALUES(2,'10-9999-synthetic-mnd-5')")));
        QVERIFY(query.exec(QStringLiteral("INSERT INTO paper_search_rows(rowid,slug) VALUES(3,'10-9999-synthetic-gadget-2')")));
        query.prepare(QStringLiteral("INSERT INTO paper_fts(rowid,title,authors,year,journal,doi,source,body) VALUES(?,?,?,?,?,?,?,?)"));
        query.addBindValue(1);
        query.addBindValue(QStringLiteral("Synthetic Study of Widget Dynamics"));
        query.addBindValue(QStringLiteral("Jane Q. Placeholder"));
        query.addBindValue(QStringLiteral("2021"));
        query.addBindValue(QStringLiteral("Journal of Imaginary Results"));
        query.addBindValue(QStringLiteral("10.9999/synthetic.widget.1"));
        query.addBindValue(QStringLiteral("synthetic"));
        query.addBindValue(QStringLiteral("control body without the target phrase"));
        QVERIFY(query.exec());
        query.prepare(QStringLiteral("INSERT INTO paper_fts(rowid,title,authors,year,journal,doi,source,body) VALUES(?,?,?,?,?,?,?,?)"));
        query.addBindValue(2);
        query.addBindValue(QStringLiteral("Neurofilament Biomarkers in Amyotrophic Lateral Sclerosis"));
        query.addBindValue(QStringLiteral("Casey Clinician"));
        query.addBindValue(QStringLiteral("2026"));
        query.addBindValue(QStringLiteral("Journal of Synthetic Neurology"));
        query.addBindValue(QStringLiteral("10.9999/synthetic.mnd.5"));
        query.addBindValue(QStringLiteral("md-project-review-set"));
        query.addBindValue(QStringLiteral("serum neurofilament light chain biomarkers in ALS diagnosis"));
        QVERIFY(query.exec());
        query.prepare(QStringLiteral("INSERT INTO paper_fts(rowid,title,authors,year,journal,doi,source,body) VALUES(?,?,?,?,?,?,?,?)"));
        query.addBindValue(3);
        query.addBindValue(QStringLiteral("Gadget Oscillations Reconsidered"));
        query.addBindValue(QStringLiteral("Ada Example"));
        query.addBindValue(QStringLiteral("1999"));
        query.addBindValue(QStringLiteral("Annals of Placeholder Science"));
        query.addBindValue(QStringLiteral("10.9999/synthetic.gadget.2"));
        query.addBindValue(QStringLiteral("synthetic"));
        query.addBindValue(QStringLiteral("another control body"));
        QVERIFY(query.exec());
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("papertest_fts_fixture"));

    PaperLibraryModel model;
    QSignalSpy loadedSpy(&model, &PaperLibraryModel::loaded);
    model.load(corpusDir);
    QVERIFY(loadedSpy.wait(10000));
    QVERIFY(model.hasFullTextSearchIndex());

    const QList<int> rows = model.fullTextSearchRows(QStringLiteral("neurofilament light"), 5);
    QCOMPARE(rows.size(), 1);
    QCOMPARE(model.data(model.index(rows.first()), PaperLibraryModel::SlugRole).toString(), QStringLiteral("10-9999-synthetic-mnd-5"));
    QVERIFY(model.fullTextSearchRows(QStringLiteral("zzzzzz-no-hit"), 5).isEmpty());
}

void PaperLibraryModelTest::testSemanticGraphRelatedRows()
{
    const QString corpusDir = writeCatalog({widgetRecord(), mndRecord(), mndDiagnosisRecord(), gadgetRecord()});

    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("papertest_graph_fixture"));
        db.setDatabaseName(m_dir->filePath(QStringLiteral("catalog.db")));
        QVERIFY(db.open());
        QSqlQuery query(db);
        QVERIFY(query.exec(QStringLiteral("CREATE TABLE papers (slug TEXT PRIMARY KEY, pdf_path TEXT, pdf_evicted INTEGER DEFAULT 0, last_accessed TEXT, access_count INTEGER DEFAULT 0, pinned INTEGER DEFAULT 0, cited_by_count INTEGER)")));
        for (const QString &slug : {QStringLiteral("10-9999-synthetic-widget-1"), QStringLiteral("10-9999-synthetic-mnd-5"), QStringLiteral("10-9999-synthetic-mnd-diagnosis-16"), QStringLiteral("10-9999-synthetic-gadget-2")}) {
            QVERIFY(query.exec(QStringLiteral("INSERT INTO papers(slug,pdf_path,pdf_evicted,last_accessed,access_count,pinned,cited_by_count) VALUES('%1', NULL, 0, NULL, 0, 0, NULL)").arg(slug)));
        }
        QVERIFY(query.exec(QStringLiteral("CREATE TABLE related_edges (source_slug TEXT NOT NULL, target_slug TEXT NOT NULL, score REAL NOT NULL, rank INTEGER NOT NULL, kind TEXT NOT NULL, generated_ts TEXT NOT NULL, model TEXT NOT NULL, PRIMARY KEY(source_slug,target_slug,kind))")));
        QVERIFY(query.exec(QStringLiteral("INSERT INTO related_edges VALUES('10-9999-synthetic-mnd-5','10-9999-synthetic-mnd-diagnosis-16',0.91,1,'semantic','2026-07-06T00:00:00Z','synthetic')")));
        QVERIFY(query.exec(QStringLiteral("INSERT INTO related_edges VALUES('10-9999-synthetic-mnd-5','10-9999-synthetic-widget-1',0.72,2,'semantic','2026-07-06T00:00:00Z','synthetic')")));
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("papertest_graph_fixture"));

    PaperLibraryModel model;
    QSignalSpy loadedSpy(&model, &PaperLibraryModel::loaded);
    model.load(corpusDir);
    QVERIFY(loadedSpy.wait(10000));
    QVERIFY(model.hasSemanticGraph());

    const int mndRow = model.rowForLookupSlug(QStringLiteral("10-9999-synthetic-mnd-5"));
    const int diagnosisRow = model.rowForLookupSlug(QStringLiteral("10-9999-synthetic-mnd-diagnosis-16"));
    QVERIFY(mndRow >= 0);
    QVERIFY(diagnosisRow >= 0);
    QCOMPARE(model.data(model.index(mndRow), PaperLibraryModel::RelatedCountRole).toInt(), 2);
    QCOMPARE(model.data(model.index(diagnosisRow), PaperLibraryModel::RelatedCountRole).toInt(), 0);

    const QList<int> rows = model.relatedRowsForSlug(QStringLiteral("10-9999-synthetic-mnd-5"), 10);
    QCOMPARE(rows.size(), 2);
    QCOMPARE(model.data(model.index(rows.at(0)), PaperLibraryModel::SlugRole).toString(), QStringLiteral("10-9999-synthetic-mnd-diagnosis-16"));
    QCOMPARE(model.data(model.index(rows.at(1)), PaperLibraryModel::SlugRole).toString(), QStringLiteral("10-9999-synthetic-widget-1"));
}

// Regression guard: the backend keeps the FTS index in a dedicated search.db (not in
// catalog.db, so the catalog's snapshot stays small). The app must find and use it
// there. Without this, moving the index out of catalog.db silently disables full-text
// search with no failing test.
void PaperLibraryModelTest::testFullTextSearchUsesDedicatedSearchDb()
{
    const QString corpusDir = writeCatalog({widgetRecord(), mndRecord(), gadgetRecord()});

    // Papers metadata stays in catalog.db.
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("papertest_searchdb_papers"));
        db.setDatabaseName(m_dir->filePath(QStringLiteral("catalog.db")));
        QVERIFY(db.open());
        QSqlQuery query(db);
        QVERIFY(query.exec(QStringLiteral("CREATE TABLE papers (slug TEXT PRIMARY KEY, pdf_path TEXT, pdf_evicted INTEGER DEFAULT 0, last_accessed TEXT, access_count INTEGER DEFAULT 0, pinned INTEGER DEFAULT 0, cited_by_count INTEGER)")));
        for (const QString &slug : {QStringLiteral("10-9999-synthetic-widget-1"), QStringLiteral("10-9999-synthetic-mnd-5"), QStringLiteral("10-9999-synthetic-gadget-2")}) {
            QVERIFY(query.exec(QStringLiteral("INSERT INTO papers(slug,pdf_path,pdf_evicted,last_accessed,access_count,pinned,cited_by_count) VALUES('%1', NULL, 0, NULL, 0, 0, NULL)").arg(slug)));
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("papertest_searchdb_papers"));

    // FTS index lives in the dedicated search.db — NOT catalog.db.
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("papertest_searchdb_fts"));
        db.setDatabaseName(m_dir->filePath(QStringLiteral("search.db")));
        QVERIFY(db.open());
        QSqlQuery query(db);
        QVERIFY(query.exec(QStringLiteral("CREATE TABLE paper_search_rows (rowid INTEGER PRIMARY KEY, slug TEXT NOT NULL UNIQUE)")));
        QVERIFY(query.exec(QStringLiteral("CREATE VIRTUAL TABLE paper_fts USING fts5(title, authors, year, journal, doi, source, body, content='', tokenize='unicode61 remove_diacritics 2', prefix='2 3 4')")));
        QVERIFY(query.exec(QStringLiteral("INSERT INTO paper_search_rows(rowid,slug) VALUES(1,'10-9999-synthetic-widget-1')")));
        QVERIFY(query.exec(QStringLiteral("INSERT INTO paper_search_rows(rowid,slug) VALUES(2,'10-9999-synthetic-mnd-5')")));
        QVERIFY(query.exec(QStringLiteral("INSERT INTO paper_search_rows(rowid,slug) VALUES(3,'10-9999-synthetic-gadget-2')")));
        query.prepare(QStringLiteral("INSERT INTO paper_fts(rowid,title,authors,year,journal,doi,source,body) VALUES(?,?,?,?,?,?,?,?)"));
        query.addBindValue(2);
        query.addBindValue(QStringLiteral("Neurofilament Biomarkers in Amyotrophic Lateral Sclerosis"));
        query.addBindValue(QStringLiteral("Casey Clinician"));
        query.addBindValue(QStringLiteral("2026"));
        query.addBindValue(QStringLiteral("Journal of Synthetic Neurology"));
        query.addBindValue(QStringLiteral("10.9999/synthetic.mnd.5"));
        query.addBindValue(QStringLiteral("md-project-review-set"));
        query.addBindValue(QStringLiteral("serum neurofilament light chain biomarkers in ALS diagnosis"));
        QVERIFY(query.exec());
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("papertest_searchdb_fts"));
    writeCorpusHealthState(corpusDir,
                           false,
                           3,
                           true,
                           false,
                           {QStringLiteral("semantic graph is stale (0/3 embedding rows)")});

    PaperLibraryModel model;
    QSignalSpy loadedSpy(&model, &PaperLibraryModel::loaded);
    model.load(corpusDir);
    QVERIFY(loadedSpy.wait(10000));
    QVERIFY(model.hasFullTextSearchIndex());
    QVERIFY(model.hasFreshFullTextSearchIndex());

    const QList<int> rows = model.fullTextSearchRows(QStringLiteral("neurofilament light"), 5);
    QCOMPARE(rows.size(), 1);
    QCOMPARE(model.data(model.index(rows.first()), PaperLibraryModel::SlugRole).toString(), QStringLiteral("10-9999-synthetic-mnd-5"));
}

// Regression guard: the semantic graph lives in a dedicated graph/graph.db. The app must
// find related_edges there for "show adjacent" (Tier-1) and the tile connection badges.
void PaperLibraryModelTest::testSemanticGraphUsesDedicatedGraphDb()
{
    const QString corpusDir = writeCatalog({widgetRecord(), mndRecord(), mndDiagnosisRecord(), gadgetRecord()});

    // Papers metadata stays in catalog.db.
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("papertest_graphdb_papers"));
        db.setDatabaseName(m_dir->filePath(QStringLiteral("catalog.db")));
        QVERIFY(db.open());
        QSqlQuery query(db);
        QVERIFY(query.exec(QStringLiteral("CREATE TABLE papers (slug TEXT PRIMARY KEY, pdf_path TEXT, pdf_evicted INTEGER DEFAULT 0, last_accessed TEXT, access_count INTEGER DEFAULT 0, pinned INTEGER DEFAULT 0, cited_by_count INTEGER)")));
        for (const QString &slug : {QStringLiteral("10-9999-synthetic-widget-1"), QStringLiteral("10-9999-synthetic-mnd-5"), QStringLiteral("10-9999-synthetic-mnd-diagnosis-16"), QStringLiteral("10-9999-synthetic-gadget-2")}) {
            QVERIFY(query.exec(QStringLiteral("INSERT INTO papers(slug,pdf_path,pdf_evicted,last_accessed,access_count,pinned,cited_by_count) VALUES('%1', NULL, 0, NULL, 0, 0, NULL)").arg(slug)));
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("papertest_graphdb_papers"));

    // related_edges lives in the dedicated graph/graph.db — NOT catalog.db.
    QVERIFY(QDir(corpusDir).mkpath(QStringLiteral("graph")));
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("papertest_graphdb_edges"));
        db.setDatabaseName(m_dir->filePath(QStringLiteral("graph/graph.db")));
        QVERIFY(db.open());
        QSqlQuery query(db);
        QVERIFY(query.exec(QStringLiteral("CREATE TABLE related_edges (source_slug TEXT NOT NULL, target_slug TEXT NOT NULL, score REAL NOT NULL, rank INTEGER NOT NULL, kind TEXT NOT NULL, generated_ts TEXT NOT NULL, model TEXT NOT NULL, PRIMARY KEY(source_slug,target_slug,kind))")));
        QVERIFY(query.exec(QStringLiteral("INSERT INTO related_edges VALUES('10-9999-synthetic-mnd-5','10-9999-synthetic-mnd-diagnosis-16',0.91,1,'semantic','2026-07-06T00:00:00Z','synthetic')")));
        QVERIFY(query.exec(QStringLiteral("INSERT INTO related_edges VALUES('10-9999-synthetic-mnd-5','10-9999-synthetic-widget-1',0.72,2,'semantic','2026-07-06T00:00:00Z','synthetic')")));
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("papertest_graphdb_edges"));
    writeCorpusHealthState(corpusDir,
                           false,
                           4,
                           false,
                           true,
                           {QStringLiteral("full-text index is stale (0/4 rows)")});

    PaperLibraryModel model;
    QSignalSpy loadedSpy(&model, &PaperLibraryModel::loaded);
    model.load(corpusDir);
    QVERIFY(loadedSpy.wait(10000));
    QVERIFY(model.hasSemanticGraph());
    QVERIFY(model.hasFreshSemanticGraph());

    const int mndRow = model.rowForLookupSlug(QStringLiteral("10-9999-synthetic-mnd-5"));
    QVERIFY(mndRow >= 0);
    QCOMPARE(model.data(model.index(mndRow), PaperLibraryModel::RelatedCountRole).toInt(), 2);

    const QList<int> rows = model.relatedRowsForSlug(QStringLiteral("10-9999-synthetic-mnd-5"), 10);
    QCOMPARE(rows.size(), 2);
    QCOMPARE(model.data(model.index(rows.at(0)), PaperLibraryModel::SlugRole).toString(), QStringLiteral("10-9999-synthetic-mnd-diagnosis-16"));
    QCOMPARE(model.data(model.index(rows.at(1)), PaperLibraryModel::SlugRole).toString(), QStringLiteral("10-9999-synthetic-widget-1"));
}

void PaperLibraryModelTest::testCorpusHealthReadsPublishedFreshness()
{
    const QString corpusDir = writeCatalog({widgetRecord(), gadgetRecord()});
    {
        QSqlDatabase search = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("papertest_health_search"));
        search.setDatabaseName(QDir(corpusDir).filePath(QStringLiteral("search.db")));
        QVERIFY(search.open());
        QSqlQuery query(search);
        QVERIFY(query.exec(QStringLiteral("CREATE TABLE paper_fts(dummy TEXT)")));
        QVERIFY(query.exec(QStringLiteral("CREATE TABLE paper_search_rows(rowid INTEGER PRIMARY KEY, slug TEXT)")));
        search.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("papertest_health_search"));
    QVERIFY(QDir(corpusDir).mkpath(QStringLiteral("graph")));
    {
        QSqlDatabase graph = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("papertest_health_graph"));
        graph.setDatabaseName(QDir(corpusDir).filePath(QStringLiteral("graph/graph.db")));
        QVERIFY(graph.open());
        QSqlQuery query(graph);
        QVERIFY(query.exec(QStringLiteral("CREATE TABLE related_edges(source_slug TEXT, target_slug TEXT)")));
        graph.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("papertest_health_graph"));
    writeCorpusHealthState(corpusDir, true, 2, true, true);

    PaperLibraryModel model;
    QSignalSpy loadedSpy(&model, &PaperLibraryModel::loaded);
    model.load(corpusDir);
    QVERIFY(loadedSpy.wait(10000));
    PaperLibraryModel::CorpusHealth health = model.corpusHealth();
    QCOMPARE(health.status, PaperLibraryModel::CorpusHealth::Healthy);
    QCOMPARE(health.generatedAt, QStringLiteral("2026-07-10T00:00:00Z"));
    QVERIFY(health.searchFresh);
    QVERIFY(health.graphFresh);

    writeCorpusHealthState(corpusDir,
                           false,
                           2,
                           false,
                           false,
                           {QStringLiteral("full-text index is stale (1/2 rows)"), QStringLiteral("semantic graph is stale (0/2 embedding rows)")});
    model.load(corpusDir);
    QTRY_COMPARE_WITH_TIMEOUT(loadedSpy.count(), 2, 10000);
    health = model.corpusHealth();
    QCOMPARE(health.status, PaperLibraryModel::CorpusHealth::Degraded);
    QVERIFY(!health.searchFresh);
    QVERIFY(!health.graphFresh);
    QCOMPARE(health.issues.size(), 2);

    // Even a previously healthy state cannot certify a different catalog generation.
    writeCorpusHealthState(corpusDir, true, 1, true, true);
    model.load(corpusDir);
    QTRY_COMPARE_WITH_TIMEOUT(loadedSpy.count(), 3, 10000);
    health = model.corpusHealth();
    QCOMPARE(health.status, PaperLibraryModel::CorpusHealth::Degraded);
    QVERIFY(!health.searchFresh);
    QVERIFY(!health.graphFresh);
    QVERIFY(health.issues.constFirst().contains(QStringLiteral("1 of 2")));

    // A same-row-count metadata change cannot inherit an older green state.
    writeCorpusHealthState(corpusDir, true, 2, true, true);
    QFile catalog(QDir(corpusDir).filePath(QStringLiteral("catalog.jsonl")));
    QVERIFY(catalog.open(QIODevice::ReadOnly));
    QByteArray changed = catalog.readAll();
    catalog.close();
    QVERIFY(changed.contains("Widget Dynamics"));
    changed.replace("Widget Dynamics", "Wadget Dynamics");
    QVERIFY(catalog.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(catalog.write(changed), changed.size());
    catalog.close();
    model.load(corpusDir);
    QTRY_COMPARE_WITH_TIMEOUT(loadedSpy.count(), 4, 10000);
    health = model.corpusHealth();
    QCOMPARE(health.status, PaperLibraryModel::CorpusHealth::Degraded);
    QVERIFY(!health.searchFresh);
    QVERIFY(!health.graphFresh);
    QVERIFY(health.issues.join(QLatin1Char(' ')).contains(QStringLiteral("attestation")));
}

void PaperLibraryModelTest::testGenreDrivesShelfClassification()
{
    // A confident librarian genre is authoritative for the Fiction/Nonfiction shelves;
    // an obscure fiction book no allowlist/heuristic would catch is included via genre,
    // while an empty-genre allowlist book still falls back to the text heuristic.
    auto rec = [](const char *slug, const char *title, const char *author, const char *year,
                  const char *source, const char *genre) {
        SyntheticRecord r;
        r.slug = QString::fromLatin1(slug);
        r.title = QString::fromLatin1(title);
        r.authors = QString::fromLatin1(author);
        r.year = QString::fromLatin1(year);
        r.bytes = 1000;
        r.source = QString::fromLatin1(source);
        r.addedTs = QStringLiteral("2026-06-01T00:00:00+00:00");
        r.genre = QString::fromLatin1(genre);
        return r;
    };
    const SyntheticRecord fic = rec("10-9999-obscure-novel", "An Utterly Obscure Speculative Tale", "N. Body", "2021", "aa_book", "Fiction");
    const SyntheticRecord nonfic = rec("10-9999-obscure-nonfic", "A Serious History of Widgets", "H. Storian", "2019", "book:epub", "Nonfiction");
    const SyntheticRecord paper = rec("10-9999-plain-paper", "On the Kinetics of Something", "R. Searcher", "2020", "unpaywall", "");
    const SyntheticRecord allowlistFic = rec("10-9999-hobbit", "The Hobbit, or There and Back Again", "J.R.R. Tolkien", "1937", "book:epub", "");
    // genre="Unknown" must NOT be authoritative — it falls back to the heuristic (Dune allowlist).
    const SyntheticRecord unknownFic = rec("10-9999-unknown-dune", "Dune", "Frank Herbert", "1965", "book:epub", "Unknown");
    // a paper mislabelled genre=Fiction must not reach the book-only Fiction shelf.
    const SyntheticRecord ficPaper = rec("10-9999-fiction-paper", "On Fictional Models of Diffusion", "R. Searcher", "2020", "unpaywall", "Fiction");

    const QString corpusDir = writeCatalog({fic, nonfic, paper, allowlistFic, unknownFic, ficPaper});
    PaperLibraryModel model;
    QSignalSpy loadedSpy(&model, &PaperLibraryModel::loaded);
    model.load(corpusDir);
    QVERIFY(loadedSpy.wait(10000));

    const int ficRow = model.rowForLookupSlug(fic.slug);
    QVERIFY(ficRow >= 0);
    QCOMPARE(model.data(model.index(ficRow), PaperLibraryModel::GenreRole).toString(), QStringLiteral("Fiction"));

    auto slugsInShelf = [&model](PaperLibrarySectionedModel::SmartFilter filter) {
        PaperLibrarySectionedModel sections;
        sections.setSourceModel(&model);
        sections.setSmartFilter(filter);
        QStringList slugs;
        for (int r = 0; r < sections.rowCount(); ++r) {
            const QModelIndex idx = sections.index(r);
            if (sections.data(idx, PaperLibrarySectionedModel::SectionHeaderRole).toBool()) {
                continue;
            }
            const int sourceRow = sections.data(idx, PaperLibrarySectionedModel::SourceRowRole).toInt();
            slugs << model.data(model.index(sourceRow), PaperLibraryModel::SlugRole).toString();
        }
        return slugs;
    };

    const QStringList fiction = slugsInShelf(PaperLibrarySectionedModel::Fiction);
    QVERIFY2(fiction.contains(fic.slug), "genre=Fiction book must be on the Fiction shelf");
    QVERIFY2(fiction.contains(allowlistFic.slug), "empty-genre allowlist fiction must still fall back to the heuristic");
    QVERIFY2(!fiction.contains(nonfic.slug), "genre=Nonfiction must not be Fiction");
    QVERIFY2(!fiction.contains(paper.slug), "a paper must not be on the Fiction shelf");
    QVERIFY2(fiction.contains(unknownFic.slug), "genre=Unknown must fall back to the heuristic, not hide the book");
    QVERIFY2(!fiction.contains(ficPaper.slug), "a paper mislabelled genre=Fiction must not reach the book-only Fiction shelf");

    const QStringList nonfiction = slugsInShelf(PaperLibrarySectionedModel::Nonfiction);
    QVERIFY2(nonfiction.contains(nonfic.slug), "genre=Nonfiction book must be on the Nonfiction shelf");
    QVERIFY2(!nonfiction.contains(fic.slug), "genre=Fiction must not be Nonfiction");
}

void PaperLibraryModelTest::testRecordKindRoutesBooksAndNormTitle()
{
    // The authoritative librarian record_kind decides the Books/Papers split even when the
    // text heuristic would disagree, and norm_title replaces an identifier-shaped raw title.
    auto rec = [](const char *slug, const char *title, const char *source,
                  const char *recordKind, const char *normTitle) {
        SyntheticRecord r;
        r.slug = QString::fromLatin1(slug);
        r.title = QString::fromLatin1(title);
        r.source = QString::fromLatin1(source);
        r.bytes = 1000;
        r.addedTs = QStringLiteral("2026-06-01T00:00:00+00:00");
        r.recordKind = QString::fromLatin1(recordKind);
        r.normTitle = QString::fromLatin1(normTitle);
        return r;
    };
    // A book whose source ("unpaywall") the heuristic would read as a paper — record_kind rescues it.
    const SyntheticRecord sneakyBook = rec("10-9999-rk-book", "A Quiet Monograph", "unpaywall", "book", "");
    // A paper whose raw title is a bare PMID; norm_title carries the real title.
    const SyntheticRecord junkTitlePaper = rec("10-9999-rk-paper", "30850440", "unpaywall", "paper", "Consensus Guidelines for ALS Trials");

    const QString corpusDir = writeCatalog({sneakyBook, junkTitlePaper});
    PaperLibraryModel model;
    QSignalSpy loadedSpy(&model, &PaperLibraryModel::loaded);
    model.load(corpusDir);
    QVERIFY(loadedSpy.wait(10000));

    // norm_title preferred: the identifier title is replaced, a real title is not.
    const int paperRow = model.rowForLookupSlug(junkTitlePaper.slug);
    QVERIFY(paperRow >= 0);
    QCOMPARE(model.data(model.index(paperRow), Qt::DisplayRole).toString(), QStringLiteral("Consensus Guidelines for ALS Trials"));
    QCOMPARE(model.data(model.index(paperRow), PaperLibraryModel::RecordKindRole).toString(), QStringLiteral("paper"));

    auto slugsInShelf = [&model](PaperLibrarySectionedModel::SmartFilter filter) {
        PaperLibrarySectionedModel sections;
        sections.setSourceModel(&model);
        sections.setSmartFilter(filter);
        QStringList slugs;
        for (int r = 0; r < sections.rowCount(); ++r) {
            const QModelIndex idx = sections.index(r);
            if (sections.data(idx, PaperLibrarySectionedModel::SectionHeaderRole).toBool()) {
                continue;
            }
            const int sourceRow = sections.data(idx, PaperLibrarySectionedModel::SourceRowRole).toInt();
            slugs << model.data(model.index(sourceRow), PaperLibraryModel::SlugRole).toString();
        }
        return slugs;
    };

    const QStringList books = slugsInShelf(PaperLibrarySectionedModel::Books);
    QVERIFY2(books.contains(sneakyBook.slug), "record_kind=book must reach the Books shelf despite a paper-like source");
    QVERIFY2(!books.contains(junkTitlePaper.slug), "record_kind=paper must not reach the Books shelf");

    const QStringList papers = slugsInShelf(PaperLibrarySectionedModel::Papers);
    QVERIFY2(papers.contains(junkTitlePaper.slug), "record_kind=paper belongs on the Papers shelf");
    QVERIFY2(!papers.contains(sneakyBook.slug), "record_kind=book must be excluded from the Papers shelf");
}

void PaperLibraryModelTest::testAdjacentExplicitRowsKeepBooks()
{
    // Adjacent documents land on the Papers shelf (whose filter is !isBook). With the
    // shelf filter bypassed (adjacent) a book neighbour survives; without bypass (search)
    // it is dropped — so "show adjacent" no longer silently loses book neighbours.
    SyntheticRecord book;
    book.slug = QStringLiteral("10-9999-adj-book");
    book.title = QStringLiteral("An Adjacent Novel");
    book.source = QStringLiteral("book:epub");
    book.bytes = 1000;
    book.addedTs = QStringLiteral("2026-06-01T00:00:00+00:00");
    SyntheticRecord paper;
    paper.slug = QStringLiteral("10-9999-adj-paper");
    paper.title = QStringLiteral("An Adjacent Paper");
    paper.source = QStringLiteral("unpaywall");
    paper.bytes = 1000;
    paper.addedTs = QStringLiteral("2026-06-01T00:00:00+00:00");

    const QString corpusDir = writeCatalog({book, paper});
    PaperLibraryModel model;
    QSignalSpy loadedSpy(&model, &PaperLibraryModel::loaded);
    model.load(corpusDir);
    QVERIFY(loadedSpy.wait(10000));
    const int bookRow = model.rowForLookupSlug(book.slug);
    const int paperRow = model.rowForLookupSlug(paper.slug);
    QVERIFY(bookRow >= 0 && paperRow >= 0);

    PaperLibrarySectionedModel sections;
    sections.setSourceModel(&model);
    sections.setSmartFilter(PaperLibrarySectionedModel::Papers);
    auto slugs = [&]() {
        QStringList out;
        for (int r = 0; r < sections.rowCount(); ++r) {
            const QModelIndex idx = sections.index(r);
            if (sections.data(idx, PaperLibrarySectionedModel::SectionHeaderRole).toBool()) {
                continue;
            }
            const int sr = sections.data(idx, PaperLibrarySectionedModel::SourceRowRole).toInt();
            out << model.data(model.index(sr), PaperLibraryModel::SlugRole).toString();
        }
        return out;
    };

    sections.setExplicitSourceRows({bookRow, paperRow}, QStringLiteral("Adjacent documents"), QStringLiteral("none"), /*bypassShelfFilter=*/true);
    const QStringList adjacent = slugs();
    QVERIFY2(adjacent.contains(book.slug), "adjacent must keep book neighbours");
    QVERIFY2(adjacent.contains(paper.slug), "adjacent keeps paper neighbours");

    sections.setExplicitSourceRows({bookRow, paperRow}, QStringLiteral("Search"), QStringLiteral("none"), /*bypassShelfFilter=*/false);
    const QStringList search = slugs();
    QVERIFY2(!search.contains(book.slug), "shelf-scoped search drops the book on the Papers shelf");
    QVERIFY2(search.contains(paper.slug), "shelf-scoped search keeps the paper");
}

void PaperLibraryModelTest::testExplicitSearchRowsStayTileFirstAndShelfScoped()
{
    const QString corpusDir = writeCatalog({textbookRecord(), mndRecord(), mndDiagnosisRecord(), gadgetRecord()});

    PaperLibraryModel model;
    QSignalSpy loadedSpy(&model, &PaperLibraryModel::loaded);
    model.load(corpusDir);
    QVERIFY(loadedSpy.wait(10000));

    const int textbookRow = model.rowForLookupSlug(textbookRecord().slug);
    const int mndRow = model.rowForLookupSlug(mndRecord().slug);
    const int diagnosisRow = model.rowForLookupSlug(mndDiagnosisRecord().slug);
    QVERIFY(textbookRow >= 0);
    QVERIFY(mndRow >= 0);
    QVERIFY(diagnosisRow >= 0);

    PaperLibrarySectionedModel sections;
    sections.setSourceModel(&model);
    sections.setSmartFilter(PaperLibrarySectionedModel::Mnd);
    sections.setExplicitSourceRows({textbookRow, mndRow, diagnosisRow}, QStringLiteral("Search"), QStringLiteral("No matching documents"));

    QCOMPARE(sections.rowCount(), 2);
    QVERIFY(!sections.data(sections.index(0), PaperLibrarySectionedModel::SectionHeaderRole).toBool());
    QVERIFY(sections.data(sections.index(0), PaperLibrarySectionedModel::SourceRowRole).isValid());
    QCOMPARE(sections.data(sections.index(0), Qt::DisplayRole).toString(), QStringLiteral("Neurofilament Biomarkers in Amyotrophic Lateral Sclerosis"));
    QCOMPARE(sections.data(sections.index(1), Qt::DisplayRole).toString(), QStringLiteral("Awaji ALS Criteria and Electrodiagnosis in Amyotrophic Lateral Sclerosis"));

    sections.clearExplicitSourceRows();
    QVERIFY(!sections.hasExplicitSourceRows());
    QVERIFY(sections.rowCount() >= 2);
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

void PaperLibraryModelTest::testSectionedModelInfiniteScrollKeepsCorpusBehindFocus()
{
    QList<SyntheticRecord> records;
    static constexpr int RecordCount = 740;
    records.reserve(RecordCount);
    for (int i = 0; i < RecordCount; ++i) {
        SyntheticRecord record = mndDiagnosisRecord();
        record.slug = QStringLiteral("10-9999-synthetic-mnd-scroll-%1").arg(i, 4, 10, QLatin1Char('0'));
        record.doi = QStringLiteral("10.9999/synthetic.mnd.scroll.%1").arg(i);
        record.citeKey = QStringLiteral("scroll%1mnd").arg(i);
        record.title = QStringLiteral("Synthetic ALS Reading Candidate %1").arg(i, 4, 10, QLatin1Char('0'));
        record.authors = QStringLiteral("Scroll Tester %1").arg(i);
        record.addedTs = QStringLiteral("2026-05-%1T00:00:00+00:00").arg((i % 28) + 1, 2, 10, QLatin1Char('0'));
        records.append(record);
    }
    const QString corpusDir = writeCatalog(records);

    QDir(corpusDir).mkpath(QStringLiteral("focus/MND"));
    QJsonArray manifest;
    QJsonObject curated;
    curated.insert(QStringLiteral("id"), records.first().slug);
    curated.insert(QStringLiteral("title"), QStringLiteral("Curated ALS Diagnostic Anchor"));
    curated.insert(QStringLiteral("kind"), QStringLiteral("pdf"));
    curated.insert(QStringLiteral("authors"), records.first().authors);
    curated.insert(QStringLiteral("year"), records.first().year);
    curated.insert(QStringLiteral("journal"), records.first().journal);
    curated.insert(QStringLiteral("source"), records.first().source);
    curated.insert(QStringLiteral("doi"), records.first().doi);
    curated.insert(QStringLiteral("reason"), QStringLiteral("diagnostic anchor; keep first"));
    curated.insert(QStringLiteral("shelf"), QStringLiteral("MND"));
    curated.insert(QStringLiteral("section"), QStringLiteral("00-diagnosis"));
    manifest.append(curated);
    QFile manifestFile(QDir(corpusDir).filePath(QStringLiteral("focus/MND/manifest.json")));
    QVERIFY(manifestFile.open(QIODevice::WriteOnly));
    manifestFile.write(QJsonDocument(manifest).toJson(QJsonDocument::Compact));
    manifestFile.close();

    PaperLibraryModel model;
    QSignalSpy loadedSpy(&model, &PaperLibraryModel::loaded);
    model.load(corpusDir);
    QVERIFY(loadedSpy.wait(10000));

    PaperLibrarySectionedModel sections;
    sections.setSourceModel(&model);
    sections.setSmartFilter(PaperLibrarySectionedModel::Mnd);

    QCOMPARE(sections.data(sections.index(0), Qt::DisplayRole).toString(), QStringLiteral("Curated ALS Diagnostic Anchor"));
    QCOMPARE(sections.rowCount(), 360);
    QVERIFY(sections.canFetchMore());

    int fetches = 0;
    while (sections.canFetchMore() && fetches++ < 10) {
        sections.fetchMore();
    }
    QVERIFY(!sections.canFetchMore());
    QCOMPARE(sections.rowCount(), RecordCount);
    bool foundDeepCorpusRow = false;
    for (int row = 0; row < sections.rowCount(); ++row) {
        if (sections.data(sections.index(row), Qt::DisplayRole).toString() == QLatin1String("Synthetic ALS Reading Candidate 0739")) {
            foundDeepCorpusRow = true;
            break;
        }
    }
    QVERIFY(foundDeepCorpusRow);
}

void PaperLibraryModelTest::testSectionedModelInfersCorpusThumbnailAssetPath()
{
    SyntheticRecord paper = mndRecord();
    paper.slug = QStringLiteral("10-9999-synthetic-mnd-thumbnail");
    paper.title = QStringLiteral("Synthetic Thumbnail Paper in Amyotrophic Lateral Sclerosis");
    const QString corpusDir = writeCatalog({paper});
    const QString thumbnail = touchFile(QStringLiteral("thumbnails/10-9999-synthetic-mnd-thumbnail.png"));

    PaperLibraryModel model;
    QSignalSpy loadedSpy(&model, &PaperLibraryModel::loaded);
    model.load(corpusDir);
    QVERIFY(loadedSpy.wait(10000));

    PaperLibrarySectionedModel sections;
    sections.setSourceModel(&model);
    sections.setSmartFilter(PaperLibrarySectionedModel::Mnd);

    QCOMPARE(sections.rowCount(), 1);
    QCOMPARE(sections.data(sections.index(0), PaperLibrarySectionedModel::ThumbnailPathRole).toString(), thumbnail);
    QCOMPARE(sections.data(sections.index(0), PaperLibrarySectionedModel::ThumbnailSourceRole).toString(),
             QStringLiteral("paperlibrary-corpus-thumbnail"));
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
    QVERIFY(visibleMndPapers >= 6);
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
    curated.insert(QStringLiteral("thumbnail_source"), QStringLiteral("paperlibrary-file-extracted"));
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

void PaperLibraryModelTest::testFocusManifestResolvesRelativePath()
{
    const QString corpusDir = writeCatalog({});
    const QString expectedPath = touchFile(QStringLiteral("loose/relative-review.pdf"));
    QVERIFY(!expectedPath.isEmpty());
    QVERIFY(QDir(corpusDir).mkpath(QStringLiteral("focus/Work")));

    QJsonObject entry;
    entry.insert(QStringLiteral("id"), QStringLiteral("relative-review"));
    entry.insert(QStringLiteral("title"), QStringLiteral("Relative Review Response"));
    entry.insert(QStringLiteral("path"), QStringLiteral("loose/relative-review.pdf"));
    entry.insert(QStringLiteral("kind"), QStringLiteral("pdf"));
    entry.insert(QStringLiteral("reason"), QStringLiteral("review/revision work"));
    entry.insert(QStringLiteral("section"), QStringLiteral("00-current"));
    QFile manifestFile(QDir(corpusDir).filePath(QStringLiteral("focus/Work/manifest.json")));
    QVERIFY(manifestFile.open(QIODevice::WriteOnly));
    manifestFile.write(QJsonDocument(QJsonArray{entry}).toJson(QJsonDocument::Compact));
    manifestFile.close();

    PaperLibraryModel model;
    QSignalSpy loadedSpy(&model, &PaperLibraryModel::loaded);
    model.load(corpusDir);
    QVERIFY(loadedSpy.wait(10000));

    PaperLibrarySectionedModel sections;
    sections.setSourceModel(&model);
    sections.setSmartFilter(PaperLibrarySectionedModel::Work);
    QCOMPARE(sections.rowCount(), 1);
    QCOMPARE(sections.resolvePath(sections.index(0)), expectedPath);
    QCOMPARE(sections.data(sections.index(0), PaperLibrarySectionedModel::PdfPathRole).toString(), expectedPath);
}

void PaperLibraryModelTest::testFocusManifestInfersThumbnailAssetPath()
{
    SyntheticRecord work = widgetRecord();
    work.slug = QStringLiteral("10-9999-synthetic-beyond-bayes-work");
    work.title = QStringLiteral("Beyond Bayes Revision Notes for High-Dimensional Inference");
    work.source = QStringLiteral("unpaywall");
    const QString corpusDir = writeCatalog({work});
    const QString pdf = touchFile(QStringLiteral("pdfs/10-9999-synthetic-beyond-bayes-work.pdf"));
    const QString thumbnail = touchFile(QStringLiteral("focus/Work/thumbnails/10-9999-synthetic-beyond-bayes-work.png"));

    QDir(corpusDir).mkpath(QStringLiteral("focus/Work"));
    QJsonObject curated;
    curated.insert(QStringLiteral("id"), work.slug);
    curated.insert(QStringLiteral("title"), QStringLiteral("Curated Beyond Bayes Draft"));
    curated.insert(QStringLiteral("path"), pdf);
    curated.insert(QStringLiteral("kind"), QStringLiteral("pdf"));
    curated.insert(QStringLiteral("reason"), QStringLiteral("Beyond Bayes manuscript; Bayesian/FEP literature"));
    curated.insert(QStringLiteral("shelf"), QStringLiteral("Work"));
    curated.insert(QStringLiteral("section"), QStringLiteral("00-beyond-bayes-revision"));
    curated.insert(QStringLiteral("thumbnail_source"), QStringLiteral("paperlibrary-file-extracted"));
    QJsonArray manifest;
    manifest.append(curated);
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

    QCOMPARE(sections.rowCount(), 1);
    QCOMPARE(sections.data(sections.index(0), PaperLibrarySectionedModel::ThumbnailPathRole).toString(), thumbnail);
    QCOMPARE(sections.data(sections.index(0), PaperLibrarySectionedModel::ThumbnailSourceRole).toString(),
             QStringLiteral("paperlibrary-file-extracted"));
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

void PaperLibraryModelTest::testAFinishedBookIsExcludedFromTheBookFeed()
{
    // The Books shelf is a curated feed (focus/Reading/manifest.json), a SEPARATE render path from
    // the section grid. A book the reader has finished must drop out of the feed the moment it is
    // marked finished -- without a guard on the feed path, a finished book kept showing there even
    // though the section grid correctly hid it. This is the real "finished books still on Books" bug.
    SyntheticRecord finishedBook = widgetRecord();
    finishedBook.slug = QStringLiteral("feed-finished");
    finishedBook.title = QStringLiteral("A Finished Feed Novel");
    finishedBook.journal = QStringLiteral("(book)");
    finishedBook.genre = QStringLiteral("Fiction");
    finishedBook.recordKind = QStringLiteral("book");
    finishedBook.source = QStringLiteral("book:epub");

    SyntheticRecord unreadBook = gadgetRecord();
    unreadBook.slug = QStringLiteral("feed-unread");
    unreadBook.title = QStringLiteral("An Unread Feed Novel");
    unreadBook.journal = QStringLiteral("(book)");
    unreadBook.genre = QStringLiteral("Fiction");
    unreadBook.recordKind = QStringLiteral("book");
    unreadBook.source = QStringLiteral("book:epub");

    const QString corpusDir = writeCatalog({finishedBook, unreadBook});

    QDir(corpusDir).mkpath(QStringLiteral("focus/Reading"));
    QJsonArray manifest;
    const auto appendEntry = [&manifest](const QString &id, const QString &title) {
        QJsonObject object;
        object.insert(QStringLiteral("id"), id);
        object.insert(QStringLiteral("title"), title);
        object.insert(QStringLiteral("kind"), QStringLiteral("epub"));
        object.insert(QStringLiteral("source"), QStringLiteral("book:epub"));
        object.insert(QStringLiteral("shelf"), QStringLiteral("Reading"));
        object.insert(QStringLiteral("section"), QStringLiteral("00-fiction-current"));
        manifest.append(object);
    };
    appendEntry(QStringLiteral("feed-finished"), QStringLiteral("A Finished Feed Novel"));
    appendEntry(QStringLiteral("feed-unread"), QStringLiteral("An Unread Feed Novel"));

    QFile manifestFile(QDir(corpusDir).filePath(QStringLiteral("focus/Reading/manifest.json")));
    QVERIFY(manifestFile.open(QIODevice::WriteOnly));
    manifestFile.write(QJsonDocument(manifest).toJson(QJsonDocument::Compact));
    manifestFile.close();

    // Mark the one book finished, as the reader would (CorpusFeed/FinishedSlugs).
    KConfigGroup feed = KSharedConfig::openConfig(QString::fromLocal8Bit(qgetenv("PAPERLIBRARY_CONFIG_PATH")),
                                                  KConfig::SimpleConfig)->group(QStringLiteral("CorpusFeed"));
    feed.writeEntry("FinishedSlugs", QStringList{QStringLiteral("feed-finished")});
    feed.sync();

    PaperLibraryModel model;
    QSignalSpy loadedSpy(&model, &PaperLibraryModel::loaded);
    model.load(corpusDir);
    QVERIFY(loadedSpy.wait(10000));

    PaperLibrarySectionedModel sections;
    sections.setSourceModel(&model);
    sections.setSmartFilter(PaperLibrarySectionedModel::Books);

    QStringList feedTitles;
    for (int row = 0; row < sections.rowCount(); ++row) {
        const QModelIndex tile = sections.index(row);
        if (!tile.data(PaperLibrarySectionedModel::SectionHeaderRole).toBool()) {
            feedTitles << tile.data(Qt::DisplayRole).toString();
        }
    }
    QVERIFY2(!feedTitles.contains(QStringLiteral("A Finished Feed Novel")),
             "a finished book is still in the book feed");
    QVERIFY2(feedTitles.contains(QStringLiteral("An Unread Feed Novel")),
             "the unread book vanished from the feed");
}

void PaperLibraryModelTest::testTitleLookupFindsACorpusTwinWithABlurb()
{
    // The detail rail enriches a local-shelf book (no librarian metadata) from its corpus twin,
    // matched by title. The lookup must be case/punctuation-insensitive and must only surface a row
    // that actually has a blurb to lend -- a title-only match is fuzzy, so an empty-blurb row is useless.
    SyntheticRecord withBlurb = widgetRecord();
    withBlurb.slug = QStringLiteral("corpus-twin");
    withBlurb.title = QStringLiteral("The Power Broker");
    withBlurb.description = QStringLiteral("Pulitzer-winning biography of Robert Moses.");

    SyntheticRecord noBlurb = gadgetRecord();
    noBlurb.slug = QStringLiteral("no-blurb");
    noBlurb.title = QStringLiteral("Bleak Untitled Volume");
    noBlurb.description = QString();

    const QString corpusDir = writeCatalog({withBlurb, noBlurb});

    PaperLibraryModel model;
    QSignalSpy loadedSpy(&model, &PaperLibraryModel::loaded);
    model.load(corpusDir);
    QVERIFY(loadedSpy.wait(10000));

    // Case- and punctuation-insensitive match to the row that carries the blurb.
    const int row = model.rowForLookupTitle(QStringLiteral("the POWER broker!"));
    QVERIFY(row >= 0);
    QCOMPARE(model.data(model.index(row), PaperLibraryModel::DescriptionRole).toString(), withBlurb.description);

    // No confident match -> -1; and a title whose only row has no blurb is not indexed.
    QCOMPARE(model.rowForLookupTitle(QStringLiteral("Some Book We Never Had")), -1);
    QCOMPARE(model.rowForLookupTitle(QStringLiteral("Bleak Untitled Volume")), -1);
}

void PaperLibraryModelTest::testAFileBackedFeedBookCanBeMarkedFinishedViaItsCorpusTwin()
{
    // A reading-feed book with no catalog row of its own -- a file-backed import whose manifest id
    // isn't a catalog slug and whose path resolves to nothing, so it renders with sourceRow < 0 --
    // must still be markable finished. It binds to its corpus twin by title, so it lands on the
    // (corpus-backed) Finished shelf and leaves the reading feed. Before the fix, marking it no-op'd.
    SyntheticRecord twin = widgetRecord();
    twin.slug = QStringLiteral("md5-got-corpus");
    twin.title = QStringLiteral("A Game of Thrones");
    twin.journal = QStringLiteral("(book)");
    twin.genre = QStringLiteral("Fiction");
    twin.recordKind = QStringLiteral("book");
    twin.source = QStringLiteral("book:epub");

    const QString corpusDir = writeCatalog({twin});

    // A real reading file that is NOT the corpus copy -- so the entry resolves to sourceRow < 0.
    const QString importedEpub = touchFile(QStringLiteral("reading/A_Game_Of_Thrones_import.epub"));
    QVERIFY(!importedEpub.isEmpty());

    QDir(corpusDir).mkpath(QStringLiteral("focus/Reading"));
    QJsonArray manifest;
    QJsonObject entry;
    entry.insert(QStringLiteral("id"), QStringLiteral("file-deadbeef")); // not a catalog slug
    entry.insert(QStringLiteral("title"), QStringLiteral("A Game of Thrones"));
    entry.insert(QStringLiteral("path"), importedEpub);
    entry.insert(QStringLiteral("kind"), QStringLiteral("epub"));
    entry.insert(QStringLiteral("source"), QStringLiteral("app-recent"));
    entry.insert(QStringLiteral("shelf"), QStringLiteral("Reading"));
    entry.insert(QStringLiteral("section"), QStringLiteral("00-fiction-current"));
    manifest.append(entry);
    QFile manifestFile(QDir(corpusDir).filePath(QStringLiteral("focus/Reading/manifest.json")));
    QVERIFY(manifestFile.open(QIODevice::WriteOnly));
    manifestFile.write(QJsonDocument(manifest).toJson(QJsonDocument::Compact));
    manifestFile.close();

    PaperLibraryModel model;
    QSignalSpy loadedSpy(&model, &PaperLibraryModel::loaded);
    model.load(corpusDir);
    QVERIFY(loadedSpy.wait(10000));

    PaperLibrarySectionedModel books;
    books.setSourceModel(&model);
    books.setSmartFilter(PaperLibrarySectionedModel::Books);

    QModelIndex feedRow;
    for (int r = 0; r < books.rowCount(); ++r) {
        const QModelIndex ix = books.index(r);
        if (ix.data(Qt::DisplayRole).toString() == QStringLiteral("A Game of Thrones")) {
            feedRow = ix;
            break;
        }
    }
    QVERIFY2(feedRow.isValid(), "the file-backed book is missing from the reading feed");
    QCOMPARE(feedRow.data(PaperLibrarySectionedModel::SourceRowRole).toInt(), -1);

    books.setFinished(feedRow, true); // was a silent no-op before the fix (sourceRow < 0)

    // It must have left the reading feed...
    QStringList feedTitles;
    for (int r = 0; r < books.rowCount(); ++r) {
        const QModelIndex ix = books.index(r);
        if (!ix.data(PaperLibrarySectionedModel::SectionHeaderRole).toBool()) {
            feedTitles << ix.data(Qt::DisplayRole).toString();
        }
    }
    QVERIFY2(!feedTitles.contains(QStringLiteral("A Game of Thrones")),
             "the finished file-backed book is still in the reading feed");

    // ...and a Finished shelf, picking the mark up from config, must now show its corpus twin.
    PaperLibrarySectionedModel finishedShelf;
    finishedShelf.setSourceModel(&model);
    finishedShelf.setSmartFilter(PaperLibrarySectionedModel::Finished);
    finishedShelf.reloadFinishedSlugs();
    QStringList finishedTitles;
    for (int r = 0; r < finishedShelf.rowCount(); ++r) {
        const QModelIndex ix = finishedShelf.index(r);
        if (!ix.data(PaperLibrarySectionedModel::SectionHeaderRole).toBool()) {
            finishedTitles << ix.data(Qt::DisplayRole).toString();
        }
    }
    QVERIFY2(finishedTitles.contains(QStringLiteral("A Game of Thrones")),
             "the finished file-backed book did not reach the Finished shelf");
}

void PaperLibraryModelTest::testFictionShelfRejectsNovelPaperFalsePositives()
{
    SyntheticRecord thrones = gadgetRecord();
    thrones.slug = QStringLiteral("md5-synthetic-game-of-thrones");
    thrones.title = QStringLiteral("A Game Of Thrones");
    thrones.authors = QStringLiteral("George R. R. Martin");
    thrones.year = QStringLiteral("1996");
    thrones.journal = QStringLiteral("(book)");
    thrones.source = QStringLiteral("book:epub");

    SyntheticRecord dune = gadgetRecord();
    dune.slug = QStringLiteral("md5-synthetic-dune");
    dune.title = QStringLiteral("Dune");
    dune.authors = QStringLiteral("Herbert, Frank");
    dune.year = QStringLiteral("1965");
    dune.journal = QStringLiteral("(book)");
    dune.source = QStringLiteral("book:epub");

    SyntheticRecord earthsea = gadgetRecord();
    earthsea.slug = QStringLiteral("md5-synthetic-earthsea");
    earthsea.title = QStringLiteral("The Books of Earthsea: The Complete Illustrated Edition");
    earthsea.authors = QStringLiteral("Ursula K. le Guin");
    earthsea.year = QStringLiteral("2018");
    earthsea.source = QStringLiteral("book:epub");

    SyntheticRecord hobbit = gadgetRecord();
    hobbit.slug = QStringLiteral("md5-synthetic-hobbit");
    hobbit.title = QStringLiteral("The Hobbit");
    hobbit.authors = QStringLiteral("Tolkien, J. R. R.");
    hobbit.year = QStringLiteral("1937");
    hobbit.journal = QStringLiteral("(book)");
    hobbit.source = QStringLiteral("aa_book");

    SyntheticRecord carpentaria = gadgetRecord();
    carpentaria.slug = QStringLiteral("md5-synthetic-carpentaria");
    carpentaria.title = QStringLiteral("Carpentaria: A Novel");
    carpentaria.authors = QStringLiteral("Alexis Wright");
    carpentaria.year = QStringLiteral("2006");
    carpentaria.journal = QStringLiteral("(book)");
    carpentaria.source = QStringLiteral("book:epub");

    SyntheticRecord waterKnife = gadgetRecord();
    waterKnife.slug = QStringLiteral("md5-synthetic-water-knife");
    waterKnife.title = QStringLiteral("The Water Knife: A novel");
    waterKnife.authors = QStringLiteral("Bacigalupi, Paolo");
    waterKnife.year = QStringLiteral("2015");
    waterKnife.journal = QStringLiteral("(book)");
    waterKnife.source = QStringLiteral("book:epub");

    SyntheticRecord greenMars = gadgetRecord();
    greenMars.slug = QStringLiteral("md5-synthetic-green-mars");
    greenMars.title = QStringLiteral("Green Mars");
    greenMars.authors = QStringLiteral("Robinson, Kim Stanley");
    greenMars.year = QStringLiteral("1993");
    greenMars.journal = QStringLiteral("(book)");
    greenMars.source = QStringLiteral("book:epub");

    SyntheticRecord novelBiomarker = gadgetRecord();
    novelBiomarker.slug = QStringLiteral("10-9999-synthetic-novel-biomarker-paper");
    novelBiomarker.title = QStringLiteral("Candidate Marker for Diagnosis or Staging: Beta-Band Intermuscular Coherence as a Novel Biomarker");
    novelBiomarker.authors = QStringLiteral("K. M. Fisher");
    novelBiomarker.year = QStringLiteral("2012");
    novelBiomarker.journal = QStringLiteral("Brain");
    novelBiomarker.source = QStringLiteral("synthetic");

    SyntheticRecord novelRepeat = gadgetRecord();
    novelRepeat.slug = QStringLiteral("10-9999-synthetic-novel-repeat-paper");
    novelRepeat.title = QStringLiteral("A Novel Slow Tandem Repeat Located in the Human Genome");
    novelRepeat.authors = QStringLiteral("Hao Pang");
    novelRepeat.year = QStringLiteral("2004");
    novelRepeat.journal = QStringLiteral("Human Biology");
    novelRepeat.source = QStringLiteral("synthetic");

    SyntheticRecord novelParadigm = gadgetRecord();
    novelParadigm.slug = QStringLiteral("10-9999-synthetic-novel-paradigm-paper");
    novelParadigm.title = QStringLiteral("Dynamical Integrity: A Novel Paradigm for Complex Systems");
    novelParadigm.authors = QStringLiteral("Giuseppe Rega");
    novelParadigm.year = QStringLiteral("2018");
    novelParadigm.journal = QStringLiteral("CISM International Centre for Mechanical Sciences");
    novelParadigm.source = QStringLiteral("synthetic");

    const QString corpusDir = writeCatalog({thrones, dune, earthsea, hobbit, carpentaria, waterKnife, greenMars, novelBiomarker, novelRepeat, novelParadigm});
    const QString thronesPath = touchFile(QStringLiteral("reading/A_Game_Of_Thrones.epub"));
    QDir(corpusDir).mkpath(QStringLiteral("focus/Reading"));
    QJsonObject focusObject;
    focusObject.insert(QStringLiteral("id"), QStringLiteral("file-fiction-current"));
    focusObject.insert(QStringLiteral("title"), QStringLiteral("A Game Of Thrones 52314094"));
    focusObject.insert(QStringLiteral("path"), thronesPath);
    focusObject.insert(QStringLiteral("kind"), QStringLiteral("epub"));
    focusObject.insert(QStringLiteral("source"), QStringLiteral("app-recent"));
    focusObject.insert(QStringLiteral("reason"), QStringLiteral("current fiction; recently opened"));
    focusObject.insert(QStringLiteral("section"), QStringLiteral("00-fiction-current"));
    QJsonArray focusManifest;
    focusManifest.append(focusObject);
    QFile manifestFile(QDir(corpusDir).filePath(QStringLiteral("focus/Reading/manifest.json")));
    QVERIFY(manifestFile.open(QIODevice::WriteOnly));
    manifestFile.write(QJsonDocument(focusManifest).toJson(QJsonDocument::Compact));
    manifestFile.close();

    PaperLibraryModel model;
    QSignalSpy loadedSpy(&model, &PaperLibraryModel::loaded);
    model.load(corpusDir);
    QVERIFY(loadedSpy.wait(10000));

    PaperLibrarySectionedModel sections;
    sections.setSourceModel(&model);
    sections.setSmartFilter(PaperLibrarySectionedModel::Fiction);

    QStringList fictionTitles;
    for (int row = 0; row < sections.rowCount(); ++row) {
        fictionTitles.append(sections.data(sections.index(row), Qt::DisplayRole).toString());
    }
    QCOMPARE(fictionTitles.count(QStringLiteral("A Game of Thrones")), 1);
    QVERIFY2(fictionTitles.contains(QStringLiteral("Dune")), qPrintable(fictionTitles.join(QLatin1Char('\n'))));
    QVERIFY2(fictionTitles.contains(QStringLiteral("The Books of Earthsea")), qPrintable(fictionTitles.join(QLatin1Char('\n'))));
    QVERIFY2(fictionTitles.contains(QStringLiteral("The Hobbit")), qPrintable(fictionTitles.join(QLatin1Char('\n'))));
    QVERIFY2(fictionTitles.contains(QStringLiteral("Carpentaria")), qPrintable(fictionTitles.join(QLatin1Char('\n'))));
    QVERIFY2(fictionTitles.contains(QStringLiteral("The Water Knife")), qPrintable(fictionTitles.join(QLatin1Char('\n'))));
    QVERIFY2(fictionTitles.contains(QStringLiteral("Green Mars")), qPrintable(fictionTitles.join(QLatin1Char('\n'))));
    QVERIFY2(!fictionTitles.contains(novelBiomarker.title), qPrintable(fictionTitles.join(QLatin1Char('\n'))));
    QVERIFY2(!fictionTitles.contains(novelRepeat.title), qPrintable(fictionTitles.join(QLatin1Char('\n'))));
    QVERIFY2(!fictionTitles.contains(novelParadigm.title), qPrintable(fictionTitles.join(QLatin1Char('\n'))));
    QCOMPARE(sections.rowCount(), 7);
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
    QCOMPARE(sections.data(sections.index(caroRow), PaperLibrarySectionedModel::RelationHintRole).toString(), QStringLiteral("Political biography"));
    const QStringList caroTags = sections.data(sections.index(caroRow), PaperLibrarySectionedModel::TopicTagsRole).toStringList();
    QVERIFY(caroTags.contains(QStringLiteral("Robert A. Caro")));
    QVERIFY(caroTags.contains(QStringLiteral("Political biography")));
}

void PaperLibraryModelTest::testBookShelfMetadataDoesNotLeakProjectClassifiers()
{
    SyntheticRecord warHistory = gadgetRecord();
    warHistory.slug = QStringLiteral("md5-synthetic-war-history-display");
    warHistory.title = QStringLiteral("1941 The America That Went To War");
    warHistory.authors = QStringLiteral("William M. Christie");
    warHistory.year = QStringLiteral("2015");
    warHistory.journal = QStringLiteral("(book)");
    warHistory.source = QStringLiteral("book:epub");
    warHistory.addedTs = QStringLiteral("2026-05-06T00:00:00+00:00");

    SyntheticRecord senate = caroRecord();
    senate.title = QStringLiteral("Master of the Senate");
    senate.source = QStringLiteral("md-project-review-set");
    senate.journal = QStringLiteral("(book)");
    senate.addedTs = QStringLiteral("2026-05-24T02:47:00+00:00");

    SyntheticRecord anthropology = anthropologyRecord();
    anthropology.title = QStringLiteral("Toward An Anthropological Theory of Value");
    anthropology.authors = QStringLiteral("David Graeber");
    anthropology.year = QStringLiteral("2000");
    anthropology.source = QStringLiteral("md-project-review-set");
    anthropology.journal = QStringLiteral("(book)");

    const QString corpusDir = writeCatalog({warHistory, senate, anthropology});

    PaperLibraryModel model;
    QSignalSpy loadedSpy(&model, &PaperLibraryModel::loaded);
    model.load(corpusDir);
    QVERIFY(loadedSpy.wait(10000));

    PaperLibrarySectionedModel sections;
    sections.setSourceModel(&model);
    sections.setSmartFilter(PaperLibrarySectionedModel::Nonfiction);

    auto tagsForTitle = [&sections](const QString &title) {
        for (int row = 0; row < sections.rowCount(); ++row) {
            const QModelIndex index = sections.index(row);
            if (sections.data(index, Qt::DisplayRole).toString() == title) {
                return sections.data(index, PaperLibrarySectionedModel::TopicTagsRole).toStringList();
            }
        }
        return QStringList();
    };
    auto relationForTitle = [&sections](const QString &title) {
        for (int row = 0; row < sections.rowCount(); ++row) {
            const QModelIndex index = sections.index(row);
            if (sections.data(index, Qt::DisplayRole).toString() == title) {
                return sections.data(index, PaperLibrarySectionedModel::RelationHintRole).toString();
            }
        }
        return QString();
    };
    auto verifyCleanBookTags = [](const QStringList &tags) {
        QVERIFY2(!tags.isEmpty(), "book shelf metadata should not be empty");
        const QString joined = tags.join(QStringLiteral(" | "));
        QVERIFY2(!joined.contains(QStringLiteral("MND")), qPrintable(joined));
        QVERIFY2(!joined.contains(QStringLiteral("ALS")), qPrintable(joined));
        QVERIFY2(!joined.contains(QStringLiteral("Psychiatry")), qPrintable(joined));
        QVERIFY2(!joined.contains(QStringLiteral("2026-")), qPrintable(joined));
        QVERIFY2(!tags.contains(QStringLiteral("Book")), qPrintable(joined));
    };

    const QStringList warTags = tagsForTitle(QStringLiteral("1941: The America That Went to War"));
    verifyCleanBookTags(warTags);
    QVERIFY(warTags.contains(QStringLiteral("William M. Christie")));
    QVERIFY(warTags.contains(QStringLiteral("Military history")));

    const QStringList senateTags = tagsForTitle(QStringLiteral("Master of the Senate"));
    verifyCleanBookTags(senateTags);
    QVERIFY(senateTags.contains(QStringLiteral("Robert A. Caro")));
    QVERIFY(senateTags.contains(QStringLiteral("Political biography")));
    QCOMPARE(relationForTitle(QStringLiteral("Master of the Senate")), QStringLiteral("Political biography"));

    const QStringList anthropologyTags = tagsForTitle(QStringLiteral("Toward An Anthropological Theory of Value"));
    verifyCleanBookTags(anthropologyTags);
    QVERIFY(anthropologyTags.contains(QStringLiteral("David Graeber")));
    QVERIFY(anthropologyTags.contains(QStringLiteral("Anthropology / social theory")));
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
    QVERIFY(nonfictionTitles.contains(QStringLiteral("The Path to Power")));
    QVERIFY(nonfictionTitles.contains(QStringLiteral("The Power Broker")));

    // dawnCommentary is a journal paper (source "imported", journal "Synthetic Cliodynamics"),
    // so it belongs on Papers, not on the Non-fiction BOOK shelf. What this test exists to prove
    // is that the commentary survives duplicate-suppression as a work distinct from the book it
    // discusses -- so assert that on the shelf it actually belongs to.
    const QString commentary =
        QStringLiteral("Introduction to Special Issue: Leading Scholars Comment on Dawn of Everything");
    QVERIFY2(!nonfictionTitles.contains(commentary), qPrintable(nonfictionTitles.join(QLatin1Char('\n'))));

    sections.setSmartFilter(PaperLibrarySectionedModel::Papers);
    QStringList paperTitles;
    for (int row = 0; row < sections.rowCount(); ++row) {
        paperTitles.append(sections.data(sections.index(row), Qt::DisplayRole).toString());
    }
    QVERIFY2(paperTitles.contains(commentary), qPrintable(paperTitles.join(QLatin1Char('\n'))));
    QCOMPARE(paperTitles.count(commentary), 1);
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
    // A journal commentary is a paper; the Non-fiction shelf holds books.
    QVERIFY(!nonfictionTitles.contains(QStringLiteral("Introduction to Special Issue: Leading Scholars Comment on Dawn of Everything")));
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

void PaperLibraryModelTest::testStaleLoadGenerationIsIgnored()
{
    PaperLibraryModel model;
    QSignalSpy loadedSpy(&model, &PaperLibraryModel::loaded);
    const QList<PaperLibraryModel::Record> stale = PaperLibraryModel::parseCatalog(recordLine(widgetRecord()));
    const QList<PaperLibraryModel::Record> current = PaperLibraryModel::parseCatalog(recordLine(widgetRecord()) + recordLine(gadgetRecord()));

    model.m_loading = true;
    model.m_loadGeneration = 2;
    model.finishLoad(stale, {}, 1);
    QCOMPARE(model.rowCount(), 0);
    QCOMPARE(loadedSpy.count(), 0);
    QVERIFY(model.m_loading); // the current generation is still in flight

    model.finishLoad(current, {}, 2);
    QCOMPARE(model.rowCount(), 2);
    QCOMPARE(loadedSpy.count(), 1);
    QCOMPARE(loadedSpy.constFirst().constFirst().toInt(), 2);
    QVERIFY(!model.m_loading);
}

void PaperLibraryModelTest::testReloadFromLoadedKeepsNewestWorker()
{
    const QString corpusDir = writeCatalog({widgetRecord()});
    QVERIFY(!corpusDir.isEmpty());

    PaperLibraryModel model;
    QSignalSpy loadedSpy(&model, &PaperLibraryModel::loaded);
    bool secondLoadStarted = false;
    bool secondCatalogWritten = false;
    QThread *secondWorker = nullptr;
    connect(&model, &PaperLibraryModel::loaded, &model, [&](int) {
        if (secondLoadStarted) {
            return;
        }
        secondLoadStarted = true;
        secondCatalogWritten = !writeCatalog({widgetRecord(), gadgetRecord()}).isEmpty();
        model.load(corpusDir); // starts before generation one's queued finished handler runs
        secondWorker = model.m_worker;
    });

    model.load(corpusDir);
    QTRY_COMPARE_WITH_TIMEOUT(loadedSpy.count(), 2, 10000);
    QVERIFY(secondLoadStarted);
    QVERIFY(secondCatalogWritten);
    QVERIFY(secondWorker);
    QCOMPARE(model.rowCount(), 2);
    QTRY_VERIFY_WITH_TIMEOUT(model.m_workers.isEmpty(), 10000);
    QCOMPARE(model.m_worker, nullptr);
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

void PaperLibraryModelTest::testDownrankStateSynchronizesAcrossModels()
{
    const QString corpusDir = writeCatalog({widgetRecord(), gadgetRecord()});
    PaperLibraryModel source;
    QSignalSpy loadedSpy(&source, &PaperLibraryModel::loaded);
    source.load(corpusDir);
    QVERIFY(loadedSpy.wait(10000));

    PaperLibrarySectionedModel first;
    PaperLibrarySectionedModel second;
    first.setSourceModel(&source);
    second.setSourceModel(&source);

    const auto indexForSlug = [&source](PaperLibrarySectionedModel &sections, const QString &slug) {
        for (int row = 0; row < sections.rowCount(); ++row) {
            const QModelIndex index = sections.index(row);
            const int sourceRow = sections.data(index, PaperLibrarySectionedModel::SourceRowRole).toInt();
            if (sourceRow >= 0 && source.data(source.index(sourceRow), PaperLibraryModel::SlugRole).toString() == slug) {
                return index;
            }
        }
        return QModelIndex();
    };
    const auto isDownranked = [&indexForSlug](PaperLibrarySectionedModel &sections, const QString &slug) {
        const QModelIndex index = indexForSlug(sections, slug);
        return index.isValid() && sections.data(index, PaperLibrarySectionedModel::DownrankedRole).toBool();
    };

    QModelIndex widgetIndex = indexForSlug(first, widgetRecord().slug);
    QVERIFY(widgetIndex.isValid());
    first.setDownranked(widgetIndex, true);
    QVERIFY(isDownranked(first, widgetRecord().slug));
    QVERIFY(isDownranked(second, widgetRecord().slug));

    // `second` was constructed before the first mutation. Its next write must merge that first
    // slug instead of replacing the config with its previously stale private set.
    QModelIndex gadgetIndex = indexForSlug(second, gadgetRecord().slug);
    QVERIFY(gadgetIndex.isValid());
    second.setDownranked(gadgetIndex, true);
    for (PaperLibrarySectionedModel *sections : {&first, &second}) {
        QVERIFY(isDownranked(*sections, widgetRecord().slug));
        QVERIFY(isDownranked(*sections, gadgetRecord().slug));
    }

    KConfigGroup feed = KSharedConfig::openConfig(m_dir->filePath(QStringLiteral("paperlibraryrc")), KConfig::SimpleConfig)->group(QStringLiteral("CorpusFeed"));
    QStringList persisted = feed.readEntry(QStringLiteral("DownrankedSlugs"), QStringList());
    persisted.sort();
    QStringList expected{widgetRecord().slug, gadgetRecord().slug};
    expected.sort();
    QCOMPARE(persisted, expected);

    widgetIndex = indexForSlug(second, widgetRecord().slug);
    QVERIFY(widgetIndex.isValid());
    second.setDownranked(widgetIndex, false);
    QVERIFY(!isDownranked(first, widgetRecord().slug));
    QVERIFY(!isDownranked(second, widgetRecord().slug));
    QVERIFY(isDownranked(first, gadgetRecord().slug));
    const KSharedConfigPtr refreshedConfig = KSharedConfig::openConfig(m_dir->filePath(QStringLiteral("paperlibraryrc")), KConfig::SimpleConfig);
    refreshedConfig->reparseConfiguration();
    const KConfigGroup refreshedFeed(refreshedConfig, QStringLiteral("CorpusFeed"));
    QCOMPARE(refreshedFeed.readEntry(QStringLiteral("DownrankedSlugs"), QStringList()), QStringList{gadgetRecord().slug});
}

void PaperLibraryModelTest::testSettingTheSameLocalBooksDoesNotResetTheModel()
{
    // LibraryView::refresh() runs on every library-tab show and calls setLocalBooks(). A model
    // reset there invalidates every sectioned proxy and forces a full 21k-row rebuild -- so
    // switching to a library tab was paying it every time. Imports rarely change; an unchanged
    // set must not reset the model.
    const QString corpusDir = writeCatalog({widgetRecord(), gadgetRecord()});
    PaperLibraryModel model;
    QSignalSpy loadedSpy(&model, &PaperLibraryModel::loaded);
    model.load(corpusDir);
    QVERIFY(loadedSpy.wait(10000));

    PaperLibraryModel::Record book;
    book.slug = QStringLiteral("local-1");
    book.title = QStringLiteral("An Imported Book");
    book.recordKind = QStringLiteral("book");
    book.pdfPath = QStringLiteral("/tmp/An Imported Book.epub");

    QSignalSpy resetSpy(&model, &QAbstractItemModel::modelReset);
    model.setLocalBooks({book});
    const int afterFirst = resetSpy.count();
    QVERIFY2(afterFirst >= 1, "the first setLocalBooks with new content must reset");

    model.setLocalBooks({book}); // identical content
    QCOMPARE(resetSpy.count(), afterFirst); // no second reset

    model.setLocalBooks({}); // genuinely different -> must reset again
    QVERIFY2(resetSpy.count() > afterFirst, "clearing the local books must reset");
}

void PaperLibraryModelTest::testFinishedBooksAreOffTheReadingShelves()
{
    // A finished book clutters the "what to read" shelves; it lives on Finished. Books, Fiction,
    // Non-fiction and Textbooks must not show it.
    SyntheticRecord finishedBook = widgetRecord();
    finishedBook.slug = QStringLiteral("done-book");
    finishedBook.title = QStringLiteral("A Finished Novel");
    finishedBook.journal = QStringLiteral("(book)");
    finishedBook.genre = QStringLiteral("Fiction");
    finishedBook.recordKind = QStringLiteral("book");
    finishedBook.source = QStringLiteral("book:epub");

    // The SAME book, a second catalog row with a DIFFERENT slug (an EPUB vs a PDF acquisition).
    // Only "done-book" is marked finished, but this duplicate must be retired with it -- that was
    // the bug: finished books kept showing on Books through their unmarked duplicate.
    SyntheticRecord finishedDuplicate = widgetRecord();
    finishedDuplicate.slug = QStringLiteral("done-book-pdf");
    finishedDuplicate.title = QStringLiteral("A Finished Novel"); // same title, different slug
    finishedDuplicate.journal = QStringLiteral("(book)");
    finishedDuplicate.genre = QStringLiteral("Fiction");
    finishedDuplicate.recordKind = QStringLiteral("book");
    finishedDuplicate.source = QStringLiteral("book:pdf");

    // A THIRD copy whose title is series-prefixed -- the real case: "Means of Ascent" is finished
    // but the shown copy is "The Years of Lyndon Johnson: Means of Ascent". Different title, same
    // book; the contains-match must still retire it.
    SyntheticRecord seriesEdition = widgetRecord();
    seriesEdition.slug = QStringLiteral("done-book-series");
    seriesEdition.title = QStringLiteral("The Collected Works: A Finished Novel");
    seriesEdition.journal = QStringLiteral("(book)");
    seriesEdition.genre = QStringLiteral("Fiction");
    seriesEdition.recordKind = QStringLiteral("book");
    seriesEdition.source = QStringLiteral("book:epub");

    SyntheticRecord unreadBook = gadgetRecord();
    unreadBook.slug = QStringLiteral("unread-book");
    unreadBook.title = QStringLiteral("An Unread Novel");
    unreadBook.journal = QStringLiteral("(book)");
    unreadBook.genre = QStringLiteral("Fiction");
    unreadBook.recordKind = QStringLiteral("book");
    unreadBook.source = QStringLiteral("book:epub");

    const QString corpusDir = writeCatalog({finishedBook, finishedDuplicate, seriesEdition, unreadBook});

    // Mark only ONE slug finished, the way the reader would (CorpusFeed/FinishedSlugs).
    KConfigGroup feed = KSharedConfig::openConfig(QString::fromLocal8Bit(qgetenv("PAPERLIBRARY_CONFIG_PATH")),
                                                  KConfig::SimpleConfig)->group(QStringLiteral("CorpusFeed"));
    feed.writeEntry("FinishedSlugs", QStringList{QStringLiteral("done-book")});
    feed.sync();

    PaperLibraryModel model;
    QSignalSpy loadedSpy(&model, &PaperLibraryModel::loaded);
    model.load(corpusDir);
    QVERIFY(loadedSpy.wait(10000));

    const auto slugsOnShelf = [&model](PaperLibrarySectionedModel::SmartFilter filter) {
        PaperLibrarySectionedModel sections;
        sections.setSourceModel(&model);
        sections.setSmartFilter(filter);
        QStringList slugs;
        for (int row = 0; row < sections.rowCount(); ++row) {
            const QModelIndex tile = sections.index(row);
            if (!tile.data(PaperLibrarySectionedModel::SectionHeaderRole).toBool()) {
                slugs << tile.data(PaperLibraryModel::SlugRole).toString();
            }
        }
        return slugs;
    };

    for (auto shelf : {PaperLibrarySectionedModel::Books, PaperLibrarySectionedModel::Fiction}) {
        const QStringList slugs = slugsOnShelf(shelf);
        QVERIFY2(!slugs.contains(QStringLiteral("done-book")), "a finished book is still on a reading shelf");
        QVERIFY2(!slugs.contains(QStringLiteral("done-book-pdf")),
                 "the finished book's unmarked duplicate is still on a reading shelf");
        QVERIFY2(!slugs.contains(QStringLiteral("done-book-series")),
                 "the finished book's series-prefixed edition is still on a reading shelf");
        QVERIFY2(slugs.contains(QStringLiteral("unread-book")), "the unread book vanished");
    }
    // ...but the finished book IS on Finished.
    QVERIFY(slugsOnShelf(PaperLibrarySectionedModel::Finished).contains(QStringLiteral("done-book")));

    feed.deleteEntry("FinishedSlugs"); // don't leak into sibling tests
    feed.sync();
}

void PaperLibraryModelTest::testAClinicalAbstractFromAPaperFeedNeverReachesBooks()
{
    // The ALS/MND leak: europepmc/harvest serve conference abstracts and papers, which the
    // librarian sometimes tags genre="Reference". A book genre must NOT rescue those into Books.
    // A genuine book from a book-serving source with the same genre still must.
    SyntheticRecord abstract = widgetRecord();
    abstract.slug = QStringLiteral("als-abstract");
    abstract.title = QStringLiteral("Platform Communications: Abstract Book — 30th Symposium on ALS/MND");
    abstract.journal = QStringLiteral("Amyotrophic Lateral Sclerosis");
    abstract.genre = QStringLiteral("Reference"); // librarian mislabel
    abstract.recordKind = QStringLiteral("paper");
    abstract.source = QStringLiteral("europepmc");

    SyntheticRecord realBook = gadgetRecord();
    realBook.slug = QStringLiteral("real-reference-book");
    realBook.title = QStringLiteral("The Handbook of Cognition and Emotion");
    realBook.journal = QStringLiteral("(book)");
    realBook.genre = QStringLiteral("Reference");
    realBook.recordKind = QStringLiteral("paper"); // also mislabelled, but from a book source
    realBook.source = QStringLiteral("libgen");

    const QString corpusDir = writeCatalog({abstract, realBook});
    PaperLibraryModel model;
    QSignalSpy loadedSpy(&model, &PaperLibraryModel::loaded);
    model.load(corpusDir);
    QVERIFY(loadedSpy.wait(10000));

    const auto slugsOnShelf = [&model](PaperLibrarySectionedModel::SmartFilter filter) {
        PaperLibrarySectionedModel sections;
        sections.setSourceModel(&model);
        sections.setSmartFilter(filter);
        QStringList slugs;
        for (int row = 0; row < sections.rowCount(); ++row) {
            const QModelIndex tile = sections.index(row);
            if (!tile.data(PaperLibrarySectionedModel::SectionHeaderRole).toBool()) {
                slugs << tile.data(PaperLibraryModel::SlugRole).toString();
            }
        }
        return slugs;
    };

    const QStringList books = slugsOnShelf(PaperLibrarySectionedModel::Books);
    QVERIFY2(!books.contains(QStringLiteral("als-abstract")), "an ALS abstract from europepmc leaked into Books");
    QVERIFY2(books.contains(QStringLiteral("real-reference-book")), "a genuine book from libgen was wrongly excluded");
    // The abstract belongs on Papers.
    QVERIFY(slugsOnShelf(PaperLibrarySectionedModel::Papers).contains(QStringLiteral("als-abstract")));
}

void PaperLibraryModelTest::testBookGenreRescuesARowMislabelledAsAPaper()
{
    // The librarian assigns record_kind and genre in one pass and they disagree on 447 live
    // rows: record_kind="paper" over a book genre. isBook believed record_kind, and it gates
    // Books AND Non-fiction, so 137 real books were on no book shelf at all.
    SyntheticRecord textbook = widgetRecord();
    textbook.slug = QStringLiteral("mislabelled-textbook");
    textbook.title = QStringLiteral("Elements of Information Theory");
    textbook.journal = QStringLiteral("(book)");
    textbook.genre = QStringLiteral("Textbook");
    textbook.recordKind = QStringLiteral("paper"); // the librarian contradicts itself
    textbook.source = QStringLiteral("libgen");

    // "Manual" stays out: 310 rows carry it and they are practice guidelines, not books.
    SyntheticRecord guideline = gadgetRecord();
    guideline.slug = QStringLiteral("practice-guideline");
    guideline.title = QStringLiteral("EANM practice guideline/SNMMI procedure standard");
    guideline.journal = QStringLiteral("European Journal of Nuclear Medicine");
    guideline.genre = QStringLiteral("Manual");
    guideline.recordKind = QStringLiteral("paper");
    guideline.source = QStringLiteral("unpaywall");

    const QString corpusDir = writeCatalog({textbook, guideline});

    PaperLibraryModel model;
    QSignalSpy loadedSpy(&model, &PaperLibraryModel::loaded);
    model.load(corpusDir);
    QVERIFY(loadedSpy.wait(10000));
    QCOMPARE(model.rowCount(), 2);

    const auto slugsOnShelf = [&model](PaperLibrarySectionedModel::SmartFilter filter) {
        PaperLibrarySectionedModel sections;
        sections.setSourceModel(&model);
        sections.setSmartFilter(filter);
        QStringList slugs;
        for (int row = 0; row < sections.rowCount(); ++row) {
            const QModelIndex tile = sections.index(row);
            if (tile.data(PaperLibrarySectionedModel::SectionHeaderRole).toBool()) {
                continue;
            }
            slugs << tile.data(PaperLibraryModel::SlugRole).toString();
        }
        slugs.sort();
        return slugs;
    };

    // The rescue puts it back on a book shelf -- specifically Textbooks. Books is the general
    // reading shelf (fiction + trade non-fiction) and deliberately excludes textbooks/medicine/
    // psychiatry, which have their own shelves, so the textbook does NOT appear on Books.
    QVERIFY2(!slugsOnShelf(PaperLibrarySectionedModel::Books).contains(QStringLiteral("mislabelled-textbook")),
             "a rescued textbook belongs on Textbooks, not the general Books shelf");
    QCOMPARE(slugsOnShelf(PaperLibrarySectionedModel::Textbooks), QStringList{QStringLiteral("mislabelled-textbook")});
    // The guideline is a paper: it belongs on Papers, and on no book shelf.
    QCOMPARE(slugsOnShelf(PaperLibrarySectionedModel::Papers), QStringList{QStringLiteral("practice-guideline")});
    QVERIFY(!slugsOnShelf(PaperLibrarySectionedModel::Nonfiction).contains(QStringLiteral("practice-guideline")));
}

void PaperLibraryModelTest::testFictionAndNonfictionShelvesNeverContainPapers()
{
    // Fiction and Non-fiction are BOOK shelves. Their genre-driven branch says so
    // (isBook && ...), but the fallback branch -- taken whenever the librarian genre is
    // not one of Fiction/Nonfiction/Reference/Textbook/Manual, i.e. for every "Academic"
    // row, ~95% of the corpus -- dropped the isBook conjunct. A paper in a journal whose
    // NAME contains "Anthropology" then matched recordMatchesNonfiction()'s ungated needle
    // and landed on Non-fiction. 730 papers did, in the live corpus; 165 reached Fiction.
    SyntheticRecord anthroPaper = widgetRecord();
    anthroPaper.slug = QStringLiteral("anthro-paper");
    anthroPaper.title = QStringLiteral("Postmarital residence and biological variation");
    anthroPaper.journal = QStringLiteral("American Journal of Physical Anthropology");
    anthroPaper.genre = QStringLiteral("Academic");
    anthroPaper.recordKind = QStringLiteral("paper");
    anthroPaper.source = QStringLiteral("unpaywall");

    SyntheticRecord policyPaper = gadgetRecord();
    policyPaper.slug = QStringLiteral("policy-paper");
    policyPaper.title = QStringLiteral("Government regulation of private health insurance");
    policyPaper.journal = QStringLiteral("Cochrane Database of Systematic Reviews");
    policyPaper.genre = QStringLiteral("Academic");
    policyPaper.recordKind = QStringLiteral("paper");
    policyPaper.source = QStringLiteral("cochrane");

    SyntheticRecord fictionPaper = widgetRecord();
    fictionPaper.slug = QStringLiteral("fiction-paper");
    fictionPaper.title = QStringLiteral("Science fiction and the public understanding of science");
    fictionPaper.journal = QStringLiteral("Public Understanding of Science");
    fictionPaper.genre = QStringLiteral("Academic");
    fictionPaper.recordKind = QStringLiteral("paper");
    fictionPaper.source = QStringLiteral("unpaywall");

    SyntheticRecord nonfictionBook = widgetRecord();
    nonfictionBook.slug = QStringLiteral("nonfiction-book");
    nonfictionBook.title = QStringLiteral("Debt: The First 5000 Years");
    nonfictionBook.journal = QStringLiteral("(book)");
    nonfictionBook.genre = QStringLiteral("Nonfiction");
    nonfictionBook.recordKind = QStringLiteral("book");
    nonfictionBook.source = QStringLiteral("book:epub");

    SyntheticRecord fictionBook = gadgetRecord();
    fictionBook.slug = QStringLiteral("fiction-book");
    fictionBook.title = QStringLiteral("Frankenstein");
    fictionBook.journal = QStringLiteral("(book)");
    fictionBook.genre = QStringLiteral("Fiction");
    fictionBook.recordKind = QStringLiteral("book");
    fictionBook.source = QStringLiteral("book:standardebooks");

    const QString corpusDir =
        writeCatalog({anthroPaper, policyPaper, fictionPaper, nonfictionBook, fictionBook});

    PaperLibraryModel model;
    QSignalSpy loadedSpy(&model, &PaperLibraryModel::loaded);
    model.load(corpusDir);
    QVERIFY(loadedSpy.wait(10000));
    QCOMPARE(model.rowCount(), 5);

    const auto slugsOnShelf = [&model](PaperLibrarySectionedModel::SmartFilter filter) {
        PaperLibrarySectionedModel sections;
        sections.setSourceModel(&model);
        sections.setSmartFilter(filter);
        QStringList slugs;
        for (int row = 0; row < sections.rowCount(); ++row) {
            const QModelIndex tile = sections.index(row);
            if (tile.data(PaperLibrarySectionedModel::SectionHeaderRole).toBool()) {
                continue;
            }
            slugs << tile.data(PaperLibraryModel::SlugRole).toString();
        }
        slugs.sort();
        return slugs;
    };

    QCOMPARE(slugsOnShelf(PaperLibrarySectionedModel::Nonfiction),
             QStringList{QStringLiteral("nonfiction-book")});
    QCOMPARE(slugsOnShelf(PaperLibrarySectionedModel::Fiction),
             QStringList{QStringLiteral("fiction-book")});
}

QTEST_GUILESS_MAIN(PaperLibraryModelTest)
#include "paperlibrarymodeltest.moc"
