/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "paperlibrarymodel.h"

#include <KConfigGroup>
#include <KSharedConfig>

#include <QAtomicInt>
#include <QDir>
#include <QFileInfo>
#include <QHash>
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
    return source.contains(QStringLiteral("highdimensional")) || source.contains(QStringLiteral("biosystems")) || journal == QLatin1String("biosystems")
        || containsAnyNeedle(text,
                             {QStringLiteral("beyond bayes"),
                              QStringLiteral("bayesian"),
                              QStringLiteral("information geometry"),
                              QStringLiteral("monte carlo"),
                              QStringLiteral("prediction model"),
                              QStringLiteral("high dimensional"),
                              QStringLiteral("high-dimensional"),
                              QStringLiteral("bioelectric"),
                              QStringLiteral("morphogenesis"),
                              QStringLiteral("anticipatory systems"),
                              QStringLiteral("complex systems")});
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
    return recordMatchesGameOfThrones(text) || containsAnyNeedle(text, {QStringLiteral("novel"), QStringLiteral("fiction"), QStringLiteral("fantasy")});
}

static bool recordMatchesCaroLbj(const QString &text)
{
    return containsAnyNeedle(text, {QStringLiteral("robert caro"), QStringLiteral("lyndon johnson"), QStringLiteral("lyndon b. johnson"), QStringLiteral("lbj"), QStringLiteral("years of lyndon johnson")});
}

static bool recordMatchesPolitics(const QString &text)
{
    return containsAnyNeedle(text, {QStringLiteral("politics"), QStringLiteral("political"), QStringLiteral("congress"), QStringLiteral("democracy"), QStringLiteral("government"), QStringLiteral("public policy")});
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
                                                   QStringLiteral("anatomy"),
                                                   QStringLiteral("physiology"),
                                                   QStringLiteral("pathology"),
                                                   QStringLiteral("pharmacology"),
                                                   QStringLiteral("epidemiology"),
                                                   QStringLiteral("statistics"),
                                                   QStringLiteral("neuroscience"),
                                                   QStringLiteral("medicine")});
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
        QStringLiteral("MD Project Review Set"),
        QStringLiteral("MND Project"),
        QStringLiteral("MND / ALS"),
        QStringLiteral("Psychiatry"),
        QStringLiteral("Child & Adolescent Psychiatry"),
        QStringLiteral("Mood, Anxiety & Trauma"),
        QStringLiteral("Psychosis & Bipolar"),
        QStringLiteral("Substance Use"),
        QStringLiteral("Paeds Rotation"),
        QStringLiteral("OBGYN Rotation"),
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
        QStringLiteral("Methods & Statistics"),
        QStringLiteral("Neuroscience"),
        QStringLiteral("Medicine & Clinical"),
        QStringLiteral("Medicine"),
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
    if (containsAnyNeedle(text, {QStringLiteral("diagnos"), QStringLiteral("criteria"), QStringLiteral("awaji"), QStringLiteral("el escorial"), QStringLiteral("mimic")})) {
        return QStringLiteral("Diagnosis & Criteria");
    }
    if (containsAnyNeedle(text, {QStringLiteral("biomarker"), QStringLiteral("neurofilament"), QStringLiteral("csf"), QStringLiteral("serum")})) {
        return QStringLiteral("Biomarkers");
    }
    if (containsAnyNeedle(text, {QStringLiteral("c9orf72"), QStringLiteral("sod1"), QStringLiteral("tdp-43"), QStringLiteral("genetic"), QStringLiteral("mutation"), QStringLiteral("pathology"), QStringLiteral("mechanism")})) {
        return QStringLiteral("Genetics & Mechanisms");
    }
    if (containsAnyNeedle(text, {QStringLiteral("treatment"), QStringLiteral("therapy"), QStringLiteral("trial"), QStringLiteral("riluzole"), QStringLiteral("edaravone"), QStringLiteral("ceftriaxone")})) {
        return QStringLiteral("Treatment & Trials");
    }
    if (containsAnyNeedle(text, {QStringLiteral("epidemiology"), QStringLiteral("incidence"), QStringLiteral("prevalence"), QStringLiteral("risk factor")})) {
        return QStringLiteral("Epidemiology & Risk");
    }
    if (containsAnyNeedle(text, {QStringLiteral("care"), QStringLiteral("management"), QStringLiteral("respiratory"), QStringLiteral("feeding"), QStringLiteral("end-of-life")})) {
        return QStringLiteral("Care & Management");
    }
    return QStringLiteral("Other MND / ALS");
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
        QStringLiteral("c9orf72"),
        QStringLiteral("sod1"),
        QStringLiteral("tdp-43"),
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
    m_source = model;
    if (m_source) {
        connect(m_source, &QAbstractItemModel::modelReset, this, &PaperLibrarySectionedModel::rebuild);
        connect(m_source, &QAbstractItemModel::dataChanged, this, &PaperLibrarySectionedModel::rebuild);
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
    const QModelIndex sourceIndex = m_source ? m_source->index(row.sourceRow) : QModelIndex();
    const QString pdfPath = m_source ? m_source->resolvePdfPath(row.sourceRow) : QString();
    if (role == PdfPathRole) {
        return pdfPath;
    }
    if (role == CoverPixmapRole) {
        return QVariant::fromValue(m_coverPixmaps.value(pdfPath));
    }
    if (role == GeneratedCoverRole) {
        return m_generatedCoverPaths.contains(pdfPath);
    }
    if (role == DownrankedRole) {
        return sourceRowDownranked(row.sourceRow);
    }
    const QString source = sourceIndex.data(PaperLibraryModel::SourceRole).toString().toCaseFolded();
    const QString journal = sourceIndex.data(PaperLibraryModel::JournalRole).toString().toCaseFolded();
    const QString text = sourceIndex.data(PaperLibraryModel::HaystackRole).toString() + QLatin1Char('\n') + source;
    if (role == KindRole) {
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
        return relatedQueryFor(text, source, journal);
    }
    if (role == FocusRole) {
        return focusBucketFor(sourceIndex, text, source, journal);
    }
    if (role == ThumbnailSeedRole) {
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
    return row.header ? QString() : m_source->resolvePdfPath(row.sourceRow);
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
    for (int row = 0; row < m_rows.count(); ++row) {
        const Row &sectionRow = m_rows.at(row);
        if (!sectionRow.header && m_source && m_source->resolvePdfPath(sectionRow.sourceRow) == path) {
            const QModelIndex changed = index(row);
            Q_EMIT dataChanged(changed, changed, roles);
        }
    }
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
    rebuild();
}

void PaperLibrarySectionedModel::rebuild()
{
    beginResetModel();
    m_rows.clear();
    if (!m_source) {
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
            if (index.data(PaperLibraryModel::PinnedRole).toBool()) {
                section = QStringLiteral("Pinned");
            } else if (index.data(PaperLibraryModel::AccessCountRole).toInt() > 0) {
                section = QStringLiteral("Continue Reading");
            } else if (recordMatchesMnd(text)) {
                section = QStringLiteral("MND / ALS");
            } else if (recordMatchesPsychiatry(text)) {
                section = psychiatryTopicSectionFor(text);
            } else if (recordMatchesPaediatrics(text)) {
                section = QStringLiteral("Paeds Rotation");
            } else if (recordMatchesObgyn(text)) {
                section = QStringLiteral("OBGYN Rotation");
            } else {
                section = topicBucketFor(text, source, journal);
            }
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

    for (const QString &section : std::as_const(sectionOrder)) {
        QList<int> sourceRowsForSection = rowsBySection.value(section);
        std::stable_sort(sourceRowsForSection.begin(), sourceRowsForSection.end(), [this](int leftRow, int rightRow) {
            const bool leftDownranked = sourceRowDownranked(leftRow);
            const bool rightDownranked = sourceRowDownranked(rightRow);
            if (leftDownranked != rightDownranked) {
                return !leftDownranked;
            }
            return sourceRowLikelyBefore(m_source, leftRow, rightRow);
        });
        for (const int sourceRow : sourceRowsForSection) {
            m_rows.append({false, sourceRow, QString()});
        }
    }
    endResetModel();
}
