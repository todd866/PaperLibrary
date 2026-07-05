/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "paperlibrarymodel.h"

#include <KConfigGroup>
#include <KSharedConfig>

#include <QAtomicInt>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>

#include <algorithm>

static QString catalogPath(const QString &corpusDir)
{
    return corpusDir + QStringLiteral("/catalog.jsonl");
}

static KConfigGroup paperLibraryConfigGroup(const QString &name)
{
    const QString overridePath = qEnvironmentVariable("PAPERLIBRARY_CONFIG_PATH");
    const QString configDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    const QString configFilePath =
        !overridePath.isEmpty() ? overridePath : (configDir.isEmpty() ? QStringLiteral("paperlibraryrc") : configDir + QLatin1String("/paperlibraryrc"));
    return KSharedConfig::openConfig(configFilePath, KConfig::SimpleConfig)->group(name);
}

static const char DOWNRANKED_SLUGS_KEY[] = "DownrankedSlugs";
static constexpr int MaxCorpusFeedRows = 360;
static constexpr int MaxCorpusSearchRows = 720;
static constexpr int MaxCorpusRowsPerSection = 90;

static QString derivedPdfPath(const QString &corpusDir, const QString &slug)
{
    return corpusDir + QStringLiteral("/pdfs/") + slug + QStringLiteral(".pdf");
}

// A read-only, immutable sqlite file: URI for @p dbPath. Percent-encoding the
// path keeps a corpus directory whose name contains characters significant in
// a URI (spaces, %, ?, #) from producing a malformed URI — which would open
// the wrong file or fail silently.
static QString readOnlyImmutableUri(const QString &dbPath)
{
    return QUrl::fromLocalFile(dbPath).toString(QUrl::FullyEncoded) + QStringLiteral("?mode=ro&immutable=1");
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

PaperLibraryModel::PaperLibraryModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

PaperLibraryModel::~PaperLibraryModel()
{
    if (!m_worker) {
        return;
    }
    // The worker only touches this object through queued calls, which die with
    // the object; waiting here keeps `this` valid for any call it is posting
    // right now. Drop the finished() handler first — it would deleteLater() the
    // thread on the event loop, but that never spins during teardown and its
    // receiver (this) is going away, so reclaim the thread directly instead.
    m_worker->disconnect(this);
    m_worker->wait();
    delete m_worker;
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
    if (m_loading) {
        return;
    }
    m_loading = true;
    m_corpusDir = corpusDir;
    m_catalogMtime = QFileInfo(catalogPath(corpusDir)).lastModified();

    // Parse and enrich on a worker thread — an ~18k-line catalog must never
    // stall the UI — then land the rows in one queued model reset
    m_worker = QThread::create([this, corpusDir]() {
        QList<Record> records;
        QFile catalog(catalogPath(corpusDir));
        if (catalog.open(QIODevice::ReadOnly)) {
            records = parseCatalog(catalog.readAll());
        }
        sortRecords(records);
        enrichRecords(records, corpusDir);
        QMetaObject::invokeMethod(this, [this, records]() { finishLoad(records); }, Qt::QueuedConnection);
    });
    connect(m_worker, &QThread::finished, this, [this]() {
        m_worker->deleteLater();
        m_worker = nullptr;
    });
    m_worker->start();
}

void PaperLibraryModel::reloadIfChanged()
{
    if (!m_loaded || m_loading) {
        return;
    }
    if (QFileInfo(catalogPath(m_corpusDir)).lastModified() != m_catalogMtime) {
        load(m_corpusDir);
    }
}

bool PaperLibraryModel::isLoaded() const
{
    return m_loaded;
}

void PaperLibraryModel::finishLoad(const QList<Record> &records)
{
    beginResetModel();
    m_records = records;
    rebuildLookupRows();
    endResetModel();
    m_loading = false;
    m_loaded = true;
    Q_EMIT loaded(m_records.count());
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

static bool recordMatchesMnd(const QString &text)
{
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
}

static bool recordMatchesPsychiatry(const QString &text)
{
    if (containsAnyNeedle(text,
                          {QStringLiteral("great depression"),
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
}

static bool recordMatchesPaediatrics(const QString &text)
{
    return containsAnyNeedle(text,
                             {QStringLiteral("paediatric"),
                              QStringLiteral("pediatric"),
                              QStringLiteral("neonat"),
                              QStringLiteral("infant"),
                              QStringLiteral("childhood"),
                              QStringLiteral("children"),
                              QStringLiteral("adolescent"),
                              QStringLiteral("developmental")});
}

static bool recordMatchesObgyn(const QString &text)
{
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
}

static bool recordMatchesBeyondBayes(const QString &text, const QString &source, const QString &journal)
{
    Q_UNUSED(journal);
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
}

static bool recordMatchesPeerReview(const QString &text, const QString &source)
{
    return source.contains(QStringLiteral("peer-review")) || source.contains(QStringLiteral("peerreview")) || source.contains(QStringLiteral("review-assignment"))
        || containsAnyNeedle(text,
                             {QStringLiteral("peer review"),
                              QStringLiteral("reviewer comments"),
                              QStringLiteral("major revisions"),
                              QStringLiteral("minor revisions"),
                              QStringLiteral("manuscript review"),
                              QStringLiteral("referee report")});
}

static bool recordMatchesGameOfThrones(const QString &text)
{
    return containsAnyNeedle(text, {QStringLiteral("game of thrones"), QStringLiteral("song of ice and fire"), QStringLiteral("george r. r. martin"), QStringLiteral("george rr martin")});
}

static bool recordMatchesFiction(const QString &text)
{
    if (recordMatchesGameOfThrones(text)) {
        return true;
    }
    if (containsAnyNeedle(text, {QStringLiteral("nonfiction"), QStringLiteral("non-fiction"), QStringLiteral("non fiction")})) {
        return false;
    }
    return containsAnyWholeWord(text, {QStringLiteral("novel"), QStringLiteral("fiction"), QStringLiteral("fantasy")});
}

static bool recordMatchesCaroLbj(const QString &text)
{
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
}

static bool recordMatchesPolitics(const QString &text)
{
    return recordMatchesCaroLbj(text)
        || containsAnyNeedle(text,
                             {QStringLiteral("politics"),
                              QStringLiteral("political"),
                              QStringLiteral("congress"),
                              QStringLiteral("democracy"),
                              QStringLiteral("government"),
                              QStringLiteral("public policy"),
                              QStringLiteral("presidential biography")});
}

static bool recordMatchesGraeber(const QString &text)
{
    return containsAnyNeedle(text, {QStringLiteral("david graeber"), QStringLiteral("graeber"), QStringLiteral("bullshit jobs"), QStringLiteral("debt:"), QStringLiteral("dawn of everything")});
}

static bool recordMatchesAnthropology(const QString &text)
{
    return recordMatchesGraeber(text) || containsAnyNeedle(text, {QStringLiteral("anthropolog"), QStringLiteral("ethnograph"), QStringLiteral("archaeolog"), QStringLiteral("kinship"), QStringLiteral("debt and exchange")});
}

static bool recordMatchesMedicine(const QString &text)
{
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
    if (recordMatchesFiction(text)) {
        return false;
    }
    const bool bookLike = source.startsWith(QLatin1String("book:")) || source == QLatin1String("aa_book") || journal == QLatin1String("(book)") || text.contains(QStringLiteral("annas archive"));
    return recordMatchesCaroLbj(text) || recordMatchesAnthropology(text) || recordMatchesPolitics(text) || (bookLike && containsAnyNeedle(text,
                                                                                                                   {QStringLiteral("nonfiction"),
                                                                                                                    QStringLiteral("biography"),
                                                                                                                    QStringLiteral("history"),
                                                                                                                    QStringLiteral("memoir"),
                                                                                                                    QStringLiteral("essay"),
                                                                                                                    QStringLiteral("politics"),
                                                                                                                    QStringLiteral("anthropology"),
                                                                                                                    QStringLiteral("science")}));
}

static bool recordMatchesTextbook(const QString &text, const QString &source)
{
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
}

static bool recordMatchesBook(const QString &text, const QString &source, const QString &journal)
{
    return source.startsWith(QLatin1String("book:")) || source == QLatin1String("aa_book") || journal == QLatin1String("(book)") || text.contains(QStringLiteral("annas archive"));
}

static QString publicationKindFor(const QString &text, const QString &source, const QString &journal)
{
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

static int sectionRank(const QString &section)
{
    const QStringList order = {
        QStringLiteral("Pinned"),
        QStringLiteral("Continue Reading"),
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

void PaperLibraryModel::rebuildLookupRows()
{
    m_rowsByLookupSlug.clear();
    m_rowsByLookupDoi.clear();
    m_rowsByLookupPath.clear();

    for (int row = 0; row < m_records.count(); ++row) {
        const Record &record = m_records.at(row);
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
        record.title = object.value(QLatin1String("title")).toString();
        record.authors = object.value(QLatin1String("authors")).toString();
        record.year = object.value(QLatin1String("year")).toString();
        record.journal = object.value(QLatin1String("journal")).toString();
        record.source = object.value(QLatin1String("source")).toString();
        record.addedTs = object.value(QLatin1String("added_ts")).toString();
        record.bytes = static_cast<qint64>(object.value(QLatin1String("bytes")).toDouble());

        // Precomputed so the filter does one contains() per row; the query
        // side case-folds the same way
        record.haystack = QStringList({record.title, record.authors, record.journal, record.year, record.citeKey, record.doi, record.slug, record.source}).join(QLatin1Char('\n')).toCaseFolded();
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
 * The pdf_path/pdf_evicted columns of catalog.db, keyed by slug. Strictly
 * read-only AND immutable: the URI's immutable=1 keeps sqlite from even
 * taking shared locks, so a harvester writing the database is never
 * disturbed — and a WAL-locked or missing database simply reports failure.
 */
static QHash<QString, CatalogDbRow> readCatalogDb(const QString &dbPath, bool *ok)
{
    *ok = false;
    QHash<QString, CatalogDbRow> rows;
    if (!QFileInfo::exists(dbPath) || !QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
        return rows;
    }

    static QAtomicInt connectionCounter;
    const QString connectionName = QStringLiteral("paperlibrary_%1").arg(connectionCounter.fetchAndAddRelaxed(1));
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        db.setDatabaseName(readOnlyImmutableUri(dbPath));
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_OPEN_URI"));
        if (db.open()) {
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

void PaperLibraryModel::enrichRecords(QList<Record> &records, const QString &corpusDir)
{
    bool dbOk = false;
    const QHash<QString, CatalogDbRow> dbRows = readCatalogDb(corpusDir + QStringLiteral("/catalog.db"), &dbOk);

    for (Record &record : records) {
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
                          {QStringLiteral("diagnos"),
                           QStringLiteral("criteria"),
                           QStringLiteral("awaji"),
                           QStringLiteral("el escorial"),
                           QStringLiteral("gold coast"),
                           QStringLiteral("threshold tracking"),
                           QStringLiteral("electrodiagnos"),
                           QStringLiteral("electromyography"),
                           QStringLiteral(" emg "),
                           QStringLiteral("mimic")})) {
        return QStringLiteral("Diagnosis & Criteria");
    }
    if (containsAnyNeedle(text, {QStringLiteral("biomarker"), QStringLiteral("neurofilament"), QStringLiteral(" nfl "), QStringLiteral("csf"), QStringLiteral("serum")})) {
        return QStringLiteral("Biomarkers & Neurofilament");
    }
    if (containsAnyNeedle(text,
                          {QStringLiteral("cortical hyperexcitability"),
                           QStringLiteral("hyperexcitability"),
                           QStringLiteral("transcranial magnetic stimulation"),
                           QStringLiteral(" tms "),
                           QStringLiteral("motor cortex"),
                           QStringLiteral("beta-band"),
                           QStringLiteral("intermuscular"),
                           QStringLiteral("excitability")})) {
        return QStringLiteral("Neurophysiology / Hyperexcitability");
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
    if (recordMatchesFiction(text)) {
        return QStringLiteral("Fiction");
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
        if (!focus.isEmpty() && focus != QLatin1String("General Research")) {
            return focus + QStringLiteral(" paper");
        }
        if (topic != QLatin1String("General Research")) {
            return topic + QStringLiteral(" paper");
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
                return QStringLiteral("Cortical excitability paper");
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
    }
    return QStringLiteral("Reading candidate");
}

static QString corpusRelationHintFor(PaperLibrarySectionedModel::SmartFilter filter, const QModelIndex &index, const QString &text, const QString &source, const QString &journal)
{
    if (index.data(PaperLibraryModel::MissingRole).toBool()) {
        return QStringLiteral("Needs local PDF");
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
            return QStringLiteral("Use for excitability mechanism");
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
    if ((filter == PaperLibrarySectionedModel::Nonfiction || filter == PaperLibrarySectionedModel::Books) && recordMatchesCaroLbj(text)) {
        return QStringLiteral("Linked to LBJ / US power");
    }
    if ((filter == PaperLibrarySectionedModel::Nonfiction || filter == PaperLibrarySectionedModel::Books) && recordMatchesAnthropology(text)) {
        return QStringLiteral("Adjacent: anthropology");
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
        if (recordMatchesMnd(text)) {
            score -= 320;
        }
        if (recordMatchesBeyondBayes(text, sourceName, journal) || recordMatchesPeerReview(text, sourceName)) {
            score -= 280;
        }
        if (recordMatchesPsychiatry(text) || recordMatchesPaediatrics(text) || recordMatchesObgyn(text)) {
            score -= 200;
        }
        if (topicBucketFor(text, sourceName, journal) == QLatin1String("Methods & Statistics")) {
            score -= 160;
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

static bool sourceRowLikelyBefore(const PaperLibraryModel *source, int leftRow, int rightRow);

static bool sourceRowLikelyBeforeForShelf(const PaperLibraryModel *source, int leftRow, int rightRow, PaperLibrarySectionedModel::SmartFilter filter)
{
    const int leftScore = sourceRowShelfPriorityScore(source, leftRow, filter);
    const int rightScore = sourceRowShelfPriorityScore(source, rightRow, filter);
    if (leftScore != rightScore) {
        return leftScore < rightScore;
    }
    return sourceRowLikelyBefore(source, leftRow, rightRow);
}

static bool sourceRowLikelyBefore(const PaperLibraryModel *source, int leftRow, int rightRow)
{
    const QModelIndex left = source->index(leftRow);
    const QModelIndex right = source->index(rightRow);

    const bool leftPinned = left.data(PaperLibraryModel::PinnedRole).toBool();
    const bool rightPinned = right.data(PaperLibraryModel::PinnedRole).toBool();
    if (leftPinned != rightPinned) {
        return leftPinned;
    }

    const int leftAccessCount = left.data(PaperLibraryModel::AccessCountRole).toInt();
    const int rightAccessCount = right.data(PaperLibraryModel::AccessCountRole).toInt();
    if (leftAccessCount != rightAccessCount) {
        return leftAccessCount > rightAccessCount;
    }

    const QString leftLastAccessed = left.data(PaperLibraryModel::LastAccessedRole).toString();
    const QString rightLastAccessed = right.data(PaperLibraryModel::LastAccessedRole).toString();
    if (leftLastAccessed != rightLastAccessed) {
        if (leftLastAccessed.isEmpty() != rightLastAccessed.isEmpty()) {
            return !leftLastAccessed.isEmpty();
        }
        return leftLastAccessed > rightLastAccessed;
    }

    const bool leftMissing = left.data(PaperLibraryModel::MissingRole).toBool();
    const bool rightMissing = right.data(PaperLibraryModel::MissingRole).toBool();
    if (leftMissing != rightMissing) {
        return !leftMissing;
    }

    const int leftCitedBy = left.data(PaperLibraryModel::CitedByCountRole).toInt();
    const int rightCitedBy = right.data(PaperLibraryModel::CitedByCountRole).toInt();
    if (leftCitedBy != rightCitedBy) {
        return leftCitedBy > rightCitedBy;
    }

    const QString leftAdded = left.data(PaperLibraryModel::AddedRole).toString();
    const QString rightAdded = right.data(PaperLibraryModel::AddedRole).toString();
    if (leftAdded != rightAdded) {
        return leftAdded > rightAdded;
    }

    return left.data(Qt::DisplayRole).toString().localeAwareCompare(right.data(Qt::DisplayRole).toString()) < 0;
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
    return base.isEmpty() ? id : base;
}

static QString focusDetail(const QString &authors, const QString &year, const QString &journal)
{
    return joinNonEmpty({authors, year, journal});
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
        entry.path = object.value(QLatin1String("path")).toString().trimmed();
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
        entry.score = object.value(QLatin1String("score")).toDouble();
        entry.order = order++;
        if (entry.title.isEmpty()) {
            entry.title = focusFallbackTitle(entry.path, entry.id);
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

PaperLibrarySectionedModel::PaperLibrarySectionedModel(QObject *parent)
    : QAbstractListModel(parent)
{
    const QStringList slugs = paperLibraryConfigGroup(QStringLiteral("CorpusFeed")).readEntry(DOWNRANKED_SLUGS_KEY, QStringList());
    for (const QString &slug : slugs) {
        if (!slug.isEmpty()) {
            m_downrankedSlugs.insert(slug);
        }
    }
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
    m_source = model;
    if (m_source) {
        connect(m_source, &QAbstractItemModel::modelReset, this, [this]() {
            clearRowCache();
            rebuild();
        });
        connect(m_source, &QAbstractItemModel::dataChanged, this, [this]() {
            clearRowCache();
            rebuild();
        });
    }
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

void PaperLibrarySectionedModel::setQuery(const QString &query)
{
    const QString folded = query.trimmed().toCaseFolded();
    if (m_query == folded) {
        return;
    }
    m_query = folded;
    rebuild();
}

int PaperLibrarySectionedModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.count();
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
    if (role == CoverPixmapRole) {
        return QVariant::fromValue(m_coverPixmaps.value(focusPath));
    }
    if (role == GeneratedCoverRole) {
        return m_generatedCoverPaths.contains(focusPath);
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
    const QString source = sourceIndex.data(PaperLibraryModel::SourceRole).toString().toCaseFolded();
    const QString journal = sourceIndex.data(PaperLibraryModel::JournalRole).toString().toCaseFolded();
    const QString text = sourceIndex.data(PaperLibraryModel::HaystackRole).toString() + QLatin1Char('\n') + source;
    if (role == Qt::DisplayRole && focusRow && !row.title.isEmpty()) {
        return row.title;
    }
    if (role == PaperLibraryModel::DetailRole && focusRow && !focusDetailText.isEmpty()) {
        return focusDetailText;
    }
    if (role == ShelfIntentRole) {
        if (focusRow && !row.focusReason.isEmpty()) {
            return focusReasonPrimary(row.focusReason);
        }
        return corpusShelfIntentFor(m_smartFilter, sourceIndex, text, source, journal);
    }
    if (role == RelationHintRole) {
        if (focusRow && !row.focusReason.isEmpty()) {
            const QString secondary = focusReasonSecondary(row.focusReason);
            return secondary.isEmpty() ? row.focusSection : secondary;
        }
        return corpusRelationHintFor(m_smartFilter, sourceIndex, text, source, journal);
    }
    if (role == PriorityHintRole) {
        if (focusRow && !row.focusSection.isEmpty()) {
            return row.focusSection;
        }
        return corpusPriorityHintFor(sourceIndex, text, source, journal);
    }
    if (role == Qt::ToolTipRole) {
        const QString title = focusRow && !row.title.isEmpty() ? row.title : sourceIndex.data(Qt::DisplayRole).toString();
        const QString detail = focusRow && !focusDetailText.isEmpty() ? focusDetailText : sourceIndex.data(PaperLibraryModel::DetailRole).toString();
        const QString intent = focusRow && !row.focusReason.isEmpty() ? focusReasonPrimary(row.focusReason) : corpusShelfIntentFor(m_smartFilter, sourceIndex, text, source, journal);
        const QString relation =
            focusRow && !row.focusReason.isEmpty() ? focusReasonSecondary(row.focusReason) : corpusRelationHintFor(m_smartFilter, sourceIndex, text, source, journal);
        const QString priority = focusRow && !row.focusSection.isEmpty() ? row.focusSection : corpusPriorityHintFor(sourceIndex, text, source, journal);
        const QString tags = QStringList(data(index, TopicTagsRole).toStringList().mid(0, 3)).join(QStringLiteral(" · "));
        return corpusTileTooltip(title, {detail, priority, intent, relation, tags});
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
        if (focusRow) {
            QStringList tags;
            if (!row.focusSection.isEmpty()) {
                tags.append(row.focusSection);
            }
            const QString primary = focusReasonPrimary(row.focusReason);
            if (!primary.isEmpty()) {
                tags.append(primary);
            }
            return tags;
        }
        const QString year = sourceIndex.data(PaperLibraryModel::YearRole).toString();
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

bool PaperLibrarySectionedModel::sourceRowDownranked(int sourceRow) const
{
    if (!m_source || sourceRow < 0 || sourceRow >= m_source->rowCount()) {
        return false;
    }
    const QString slug = m_source->index(sourceRow).data(PaperLibraryModel::SlugRole).toString();
    return !slug.isEmpty() && m_downrankedSlugs.contains(slug);
}

void PaperLibrarySectionedModel::saveDownrankedSlugs() const
{
    QStringList slugs;
    slugs.reserve(m_downrankedSlugs.size());
    for (const QString &slug : m_downrankedSlugs) {
        slugs.append(slug);
    }
    slugs.sort();
    KConfigGroup group = paperLibraryConfigGroup(QStringLiteral("CorpusFeed"));
    group.writeEntry(DOWNRANKED_SLUGS_KEY, slugs);
    group.sync();
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
    const bool changed = downranked ? !m_downrankedSlugs.contains(slug) : m_downrankedSlugs.contains(slug);
    if (!changed) {
        return;
    }
    if (downranked) {
        m_downrankedSlugs.insert(slug);
    } else {
        m_downrankedSlugs.remove(slug);
    }
    saveDownrankedSlugs();
    clearRowCache();
    rebuild();
}

QString PaperLibrarySectionedModel::cacheKey() const
{
    return QStringLiteral("%1|%2|%3").arg(static_cast<int>(m_smartFilter)).arg(static_cast<int>(m_sectionMode)).arg(m_query);
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

void PaperLibrarySectionedModel::rebuild()
{
    const QString key = cacheKey();
    beginResetModel();
    const auto cached = m_rowCache.constFind(key);
    if (cached != m_rowCache.cend()) {
        m_rows = cached.value();
        rebuildPathIndex();
        endResetModel();
        return;
    }

    m_rows.clear();
    if (!m_source) {
        m_rowsByPath.clear();
        endResetModel();
        return;
    }

    const FocusManifest focusManifest = loadFocusManifest(m_smartFilter, m_source->corpusDir());
    if (focusManifest.found) {
        QSet<int> emittedSourceRows;
        QSet<QString> emittedManifestPaths;
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
            row.focusPath = entry.path;
            row.focusOrder = entry.order;
            row.focusScore = entry.score;
            m_rows.append(row);
        }

        if (m_rows.isEmpty()) {
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

        m_rowCache.insert(key, m_rows);
        rebuildPathIndex();
        endResetModel();
        return;
    }

    QHash<QString, QList<int>> rowsBySection;
    QStringList sectionOrder;
    const int sourceRows = m_source->rowCount();
    for (int row = 0; row < sourceRows; ++row) {
        const QModelIndex index = m_source->index(row);
        const QString haystack = index.data(PaperLibraryModel::HaystackRole).toString();
        if (!m_query.isEmpty() && !haystack.contains(m_query)) {
            continue;
        }

        const QString source = index.data(PaperLibraryModel::SourceRole).toString().toCaseFolded();
        const QString journal = index.data(PaperLibraryModel::JournalRole).toString().toCaseFolded();
        const QString text = haystack + QLatin1Char('\n') + source;
        const bool isBook = recordMatchesBook(text, source, journal);
        const bool isTextbook = recordMatchesTextbook(text, source);
        const bool isMedicine = recordMatchesMedicine(text);
        const bool isPsychiatry = recordMatchesPsychiatry(text);
        const bool isWork = recordMatchesBeyondBayes(text, source, journal) || recordMatchesPeerReview(text, source);
        const bool isAnthropology = recordMatchesAnthropology(text);
        const bool isPolitics = recordMatchesPolitics(text);
        const bool isFiction = recordMatchesFiction(text);
        const bool isNonfiction = recordMatchesNonfiction(text, source, journal);
        const bool isDownranked = sourceRowDownranked(row);
        switch (m_smartFilter) {
        case Papers:
            if (isBook) {
                continue;
            }
            break;
        case Books:
            if (!isBook) {
                continue;
            }
            break;
        case Textbooks:
            if (!isTextbook) {
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

    const int maxRows = m_query.isEmpty() ? MaxCorpusFeedRows : MaxCorpusSearchRows;
    const int maxRowsPerSection = m_query.isEmpty() ? MaxCorpusRowsPerSection : MaxCorpusSearchRows;
    for (const QString &section : std::as_const(sectionOrder)) {
        if (m_rows.size() >= maxRows) {
            break;
        }
        QList<int> sourceRowsForSection = rowsBySection.value(section);
        std::stable_sort(sourceRowsForSection.begin(), sourceRowsForSection.end(), [this](int leftRow, int rightRow) {
            const bool leftDownranked = sourceRowDownranked(leftRow);
            const bool rightDownranked = sourceRowDownranked(rightRow);
            if (leftDownranked != rightDownranked) {
                return !leftDownranked;
            }
            return sourceRowLikelyBeforeForShelf(m_source, leftRow, rightRow, m_smartFilter);
        });
        int rowsFromSection = 0;
        for (const int sourceRow : sourceRowsForSection) {
            if (m_rows.size() >= maxRows || rowsFromSection >= maxRowsPerSection) {
                break;
            }
            Row row;
            row.sourceRow = sourceRow;
            m_rows.append(row);
            ++rowsFromSection;
        }
    }
    m_rowCache.insert(key, m_rows);
    rebuildPathIndex();
    endResetModel();
}
