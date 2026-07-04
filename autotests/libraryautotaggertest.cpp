/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <QTest>

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>

#include <KConfigGroup>
#include <KSharedConfig>

#include <csignal>
#include <memory>
#include <sys/types.h>

#include "../shell/libraryautotagger.h"
#include "../shell/librarystore.h"

// Is a process still alive? (Reaped or never-existed processes report ESRCH.)
static bool processAlive(qint64 pid)
{
    return pid > 0 && ::kill(static_cast<pid_t>(pid), 0) == 0;
}

static qint64 readPid(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return -1;
    }
    return file.readAll().trimmed().toLongLong();
}

// The envelope `claude -p --output-format json` wraps replies in, with the
// model's text as the "result" string
static QByteArray makeEnvelope(const QString &resultText, bool isError = false)
{
    QJsonObject outer;
    outer.insert(QStringLiteral("type"), QStringLiteral("result"));
    outer.insert(QStringLiteral("subtype"), isError ? QStringLiteral("error_during_execution") : QStringLiteral("success"));
    outer.insert(QStringLiteral("is_error"), isError);
    outer.insert(QStringLiteral("result"), resultText);
    return QJsonDocument(outer).toJson(QJsonDocument::Compact);
}

// The JSON object the prompt asks the model to answer with
static QString fixtureSuggestion()
{
    QJsonObject object;
    object.insert(QStringLiteral("title"), QStringLiteral("Fixture Title"));
    object.insert(QStringLiteral("tags"), QJsonArray({QStringLiteral("Alpha"), QStringLiteral("Beta")}));
    object.insert(QStringLiteral("description"), QStringLiteral("A fixture description."));
    object.insert(QStringLiteral("keywords"), QJsonArray({QStringLiteral("one"), QStringLiteral("two"), QStringLiteral("three")}));
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

class LibraryAutoTaggerTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();

    void testParseReplyValidEnvelope();
    void testParseReplyFencedResult();
    void testParseReplyRejectsBadInput();
    void testEnqueueSkipsWhenDisabled();
    void testEnqueueSkipsExistingTitle();
    void testEnqueueSkipsWithoutCli();
    void testEnqueueSkipsDuplicatesAndFailures();
    void testEnqueueSkipsCorpusDocuments();
    void testPipelineTagsDocument();
    void testPipelineSkipsWithoutText();
    void testTeardownTerminatesChildGracefully();
    void testTeardownForceKillsUncooperativeChild();

private:
    QString storePath() const;
    QUrl touchPdf(const QString &name);
    QString writeScript(const QString &name, const QString &body);
    static KConfigGroup partGeneralGroup();

    std::unique_ptr<QTemporaryDir> m_dir;
};

void LibraryAutoTaggerTest::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
}

void LibraryAutoTaggerTest::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    qputenv("PAPERLIBRARY_CONFIG_PATH", QFile::encodeName(m_dir->filePath(QStringLiteral("paperlibraryrc"))));

    // Most tests exercise the pipeline, so enable it explicitly; no corpus
    // path may leak between tests either. The production default is opt-in.
    KConfigGroup group = partGeneralGroup();
    group.writeEntry("LibraryAutoTag", true);
    group.deleteEntry("PaperLibraryPath");
    group.sync();
}

KConfigGroup LibraryAutoTaggerTest::partGeneralGroup()
{
    // The tagger reads its setting where the part's config schema declares
    // it (paperlibraryrc [General] LibraryAutoTag)
    return KSharedConfig::openConfig(QString::fromLocal8Bit(qgetenv("PAPERLIBRARY_CONFIG_PATH")), KConfig::SimpleConfig)->group(QStringLiteral("General"));
}

QString LibraryAutoTaggerTest::storePath() const
{
    return m_dir->filePath(QStringLiteral("librarystorerc"));
}

QUrl LibraryAutoTaggerTest::touchPdf(const QString &name)
{
    const QString path = m_dir->filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return QUrl();
    }
    file.write("x");
    file.close();
    return QUrl::fromLocalFile(path);
}

QString LibraryAutoTaggerTest::writeScript(const QString &name, const QString &body)
{
    const QString path = m_dir->filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return QString();
    }
    file.write(QStringLiteral("#!/bin/sh\n%1\n").arg(body).toUtf8());
    file.close();
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return path;
}

void LibraryAutoTaggerTest::testParseReplyValidEnvelope()
{
    const LibraryAutoTagger::Suggestion suggestion = LibraryAutoTagger::parseReply(makeEnvelope(fixtureSuggestion()));
    QVERIFY(suggestion.valid);
    QCOMPARE(suggestion.title, QStringLiteral("Fixture Title"));
    QCOMPARE(suggestion.tags, QStringList({QStringLiteral("Alpha"), QStringLiteral("Beta")}));
    QCOMPARE(suggestion.description, QStringLiteral("A fixture description."));
    QCOMPARE(suggestion.keywords, QStringList({QStringLiteral("one"), QStringLiteral("two"), QStringLiteral("three")}));
}

void LibraryAutoTaggerTest::testParseReplyFencedResult()
{
    // A model that wraps its answer in a markdown fence despite the prompt
    const QByteArray fenced = makeEnvelope(QStringLiteral("```json\n%1\n```").arg(fixtureSuggestion()));
    const LibraryAutoTagger::Suggestion suggestion = LibraryAutoTagger::parseReply(fenced);
    QVERIFY(suggestion.valid);
    QCOMPARE(suggestion.title, QStringLiteral("Fixture Title"));
    QCOMPARE(suggestion.tags, QStringList({QStringLiteral("Alpha"), QStringLiteral("Beta")}));
}

void LibraryAutoTaggerTest::testParseReplyRejectsBadInput()
{
    // Not JSON at all
    QVERIFY(!LibraryAutoTagger::parseReply("claude: command not found").valid);
    // The CLI reporting an error
    QVERIFY(!LibraryAutoTagger::parseReply(makeEnvelope(fixtureSuggestion(), true)).valid);
    // A result that contains no JSON object
    QVERIFY(!LibraryAutoTagger::parseReply(makeEnvelope(QStringLiteral("I could not read that document."))).valid);
    // A result object without a usable title
    QVERIFY(!LibraryAutoTagger::parseReply(makeEnvelope(QStringLiteral("{\"tags\": [\"Alpha\"]}"))).valid);
}

void LibraryAutoTaggerTest::testEnqueueSkipsWhenDisabled()
{
    KConfigGroup group = partGeneralGroup();
    group.writeEntry("LibraryAutoTag", false);
    group.sync();

    const QUrl url = touchPdf(QStringLiteral("a.pdf"));
    LibraryStore store(storePath());
    store.recordOpen(url, QDateTime(QDate(2026, 1, 1), QTime(10, 0)));

    LibraryAutoTagger tagger(&store);
    tagger.setPdfToTextExecutable(writeScript(QStringLiteral("pdftotext"), QStringLiteral("echo text")));
    tagger.setClaudeExecutable(writeScript(QStringLiteral("claude"), QStringLiteral("printf '%s\\n' '%1'").arg(QString::fromUtf8(makeEnvelope(fixtureSuggestion())))));
    tagger.enqueue(url);

    QVERIFY(tagger.m_queue.isEmpty());
    QVERIFY(tagger.m_seen.isEmpty());
    QVERIFY(!tagger.m_busy);
}

void LibraryAutoTaggerTest::testEnqueueSkipsExistingTitle()
{
    const QUrl url = touchPdf(QStringLiteral("a.pdf"));
    LibraryStore store(storePath());
    store.recordOpen(url, QDateTime(QDate(2026, 1, 1), QTime(10, 0)));
    store.setTitle(url, QStringLiteral("Already Curated"));

    LibraryAutoTagger tagger(&store);
    tagger.setPdfToTextExecutable(writeScript(QStringLiteral("pdftotext"), QStringLiteral("echo text")));
    tagger.setClaudeExecutable(writeScript(QStringLiteral("claude"), QStringLiteral("printf '%s\\n' '%1'").arg(QString::fromUtf8(makeEnvelope(fixtureSuggestion())))));
    tagger.enqueue(url);

    QVERIFY(tagger.m_queue.isEmpty());
    QVERIFY(!tagger.m_busy);
    QCOMPARE(store.metadata(url).title, QStringLiteral("Already Curated"));
}

void LibraryAutoTaggerTest::testEnqueueSkipsWithoutCli()
{
    const QUrl url = touchPdf(QStringLiteral("a.pdf"));
    LibraryStore store(storePath());
    store.recordOpen(url, QDateTime(QDate(2026, 1, 1), QTime(10, 0)));

    LibraryAutoTagger tagger(&store);
    tagger.setPdfToTextExecutable(writeScript(QStringLiteral("pdftotext"), QStringLiteral("echo text")));
    tagger.setClaudeExecutable(QString()); // the CLI is not installed
    tagger.enqueue(url);

    QVERIFY(tagger.m_queue.isEmpty());
    QVERIFY(tagger.m_seen.isEmpty());
    QVERIFY(!tagger.m_busy);
}

void LibraryAutoTaggerTest::testEnqueueSkipsDuplicatesAndFailures()
{
    const QUrl url = touchPdf(QStringLiteral("a.pdf"));
    LibraryStore store(storePath());
    store.recordOpen(url, QDateTime(QDate(2026, 1, 1), QTime(10, 0)));

    // A claude that logs every invocation, then fails
    const QString log = m_dir->filePath(QStringLiteral("invocations.log"));
    LibraryAutoTagger tagger(&store);
    tagger.setPdfToTextExecutable(writeScript(QStringLiteral("pdftotext"), QStringLiteral("echo text")));
    tagger.setClaudeExecutable(writeScript(QStringLiteral("claude"), QStringLiteral("echo x >> '%1'\nexit 1").arg(log)));

    tagger.enqueue(url);
    tagger.enqueue(url); // duplicate while queued/processing: a no-op
    QTRY_VERIFY(!tagger.m_busy && tagger.m_queue.isEmpty());
    QCOMPARE(store.metadata(url).title, QString());

    // Failed this session: never retried, no second process
    tagger.enqueue(url);
    QVERIFY(tagger.m_queue.isEmpty());
    QVERIFY(!tagger.m_busy);
    QFile logFile(log);
    QVERIFY(logFile.open(QIODevice::ReadOnly));
    QCOMPARE(logFile.readAll().trimmed(), QByteArray("x"));
}

void LibraryAutoTaggerTest::testEnqueueSkipsCorpusDocuments()
{
    // Documents that live inside the configured PaperLibrary corpus are
    // catalogued outside this app; the tagger must never read them or send their
    // text anywhere (owner rule: corpus content stays out of the model)
    KConfigGroup group = partGeneralGroup();
    group.writeEntry("PaperLibraryPath", m_dir->filePath(QStringLiteral("corpus")));
    group.sync();

    QDir().mkpath(m_dir->filePath(QStringLiteral("corpus/pdfs")));
    const QUrl corpusUrl = touchPdf(QStringLiteral("corpus/pdfs/synthetic-widget-dynamics.pdf"));
    LibraryStore store(storePath());
    store.recordOpen(corpusUrl, QDateTime(QDate(2026, 1, 1), QTime(10, 0)));

    LibraryAutoTagger tagger(&store);
    tagger.setPdfToTextExecutable(writeScript(QStringLiteral("pdftotext"), QStringLiteral("echo text")));
    tagger.setClaudeExecutable(writeScript(QStringLiteral("claude"), QStringLiteral("printf '%s\\n' '%1'").arg(QString::fromUtf8(makeEnvelope(fixtureSuggestion())))));
    tagger.enqueue(corpusUrl);

    QVERIFY(tagger.m_queue.isEmpty());
    QVERIFY(tagger.m_seen.isEmpty());
    QVERIFY(!tagger.m_busy);
    QCOMPARE(store.metadata(corpusUrl).title, QString());

    // A document outside the corpus still queues as usual
    const QUrl normalUrl = touchPdf(QStringLiteral("elsewhere.pdf"));
    store.recordOpen(normalUrl, QDateTime(QDate(2026, 1, 1), QTime(10, 0)));
    tagger.enqueue(normalUrl);
    QVERIFY(tagger.m_seen.contains(normalUrl.toLocalFile()));
    QTRY_VERIFY(!tagger.m_busy && tagger.m_queue.isEmpty());
}

void LibraryAutoTaggerTest::testPipelineTagsDocument()
{
    const QUrl url = touchPdf(QStringLiteral("doc.pdf"));
    LibraryStore store(storePath());
    store.recordOpen(url, QDateTime(QDate(2026, 1, 1), QTime(10, 0)));

    LibraryAutoTagger tagger(&store);
    tagger.setPdfToTextExecutable(writeScript(QStringLiteral("pdftotext"), QStringLiteral("echo \"Chapter one of the fixture document.\"")));
    tagger.setClaudeExecutable(writeScript(QStringLiteral("claude"), QStringLiteral("printf '%s\\n' '%1'").arg(QString::fromUtf8(makeEnvelope(fixtureSuggestion())))));

    QSignalSpy taggedSpy(&tagger, &LibraryAutoTagger::tagged);
    tagger.enqueue(url);
    QVERIFY(taggedSpy.wait(10000));
    QCOMPARE(taggedSpy.count(), 1);
    QCOMPARE(taggedSpy.first().first().toUrl(), url);

    const LibraryStore::Entry entry = store.metadata(url);
    QCOMPARE(entry.title, QStringLiteral("Fixture Title"));
    QCOMPARE(entry.tags, QStringList({QStringLiteral("Alpha"), QStringLiteral("Beta")}));
    QCOMPARE(entry.description, QStringLiteral("A fixture description."));
    QCOMPARE(entry.keywords, QStringList({QStringLiteral("one"), QStringLiteral("two"), QStringLiteral("three")}));
}

void LibraryAutoTaggerTest::testPipelineSkipsWithoutText()
{
    const QUrl url = touchPdf(QStringLiteral("scan.pdf"));
    LibraryStore store(storePath());
    store.recordOpen(url, QDateTime(QDate(2026, 1, 1), QTime(10, 0)));

    // pdftotext yields nothing (a scanned document); claude logs if ever run
    const QString log = m_dir->filePath(QStringLiteral("invocations.log"));
    LibraryAutoTagger tagger(&store);
    tagger.setPdfToTextExecutable(writeScript(QStringLiteral("pdftotext"), QStringLiteral("exit 0")));
    tagger.setClaudeExecutable(writeScript(QStringLiteral("claude"), QStringLiteral("echo x >> '%1'\nprintf '%s\\n' '%2'").arg(log, QString::fromUtf8(makeEnvelope(fixtureSuggestion())))));

    QSignalSpy taggedSpy(&tagger, &LibraryAutoTagger::tagged);
    tagger.enqueue(url);
    QTRY_VERIFY(!tagger.m_busy && tagger.m_queue.isEmpty());

    QVERIFY(taggedSpy.isEmpty());
    QVERIFY(!QFile::exists(log)); // no claude call with nothing to read
    QCOMPARE(store.metadata(url).title, QString());
}

// Teardown must escalate: a well-behaved child gets a terminate() (SIGTERM)
// first, so it can shut down cleanly, rather than being SIGKILLed outright. A
// child that traps SIGTERM writes a marker on the way out; the marker proves
// terminate() ran before any kill().
void LibraryAutoTaggerTest::testTeardownTerminatesChildGracefully()
{
    const QUrl url = touchPdf(QStringLiteral("doc.pdf"));
    LibraryStore store(storePath());
    store.recordOpen(url, QDateTime(QDate(2026, 1, 1), QTime(10, 0)));

    const QString pidFile = m_dir->filePath(QStringLiteral("child.pid"));
    const QString marker = m_dir->filePath(QStringLiteral("terminated.marker"));

    auto *tagger = new LibraryAutoTagger(&store);
    // A pdftotext that never finishes on its own, but exits cleanly on SIGTERM
    tagger->setPdfToTextExecutable(writeScript(QStringLiteral("pdftotext"), QStringLiteral("echo $$ > '%1'\ntrap 'echo bye > \"%2\"; exit 0' TERM\nwhile :; do sleep 0.1; done").arg(pidFile, marker)));
    tagger->setClaudeExecutable(writeScript(QStringLiteral("claude"), QStringLiteral("echo x")));

    tagger->enqueue(url);
    QTRY_VERIFY(QFile::exists(pidFile)); // the child is up and running
    const qint64 pid = readPid(pidFile);
    QVERIFY(processAlive(pid));

    QElapsedTimer timer;
    timer.start();
    delete tagger;                   // destructor must terminate() before kill() and wait it out
    QVERIFY(timer.elapsed() < 3500); // bounded, never an indefinite hang

    QVERIFY(QFile::exists(marker));                     // SIGTERM was delivered: a clean shutdown
    QTRY_VERIFY_WITH_TIMEOUT(!processAlive(pid), 3000); // and no lingering child
}

// A child that ignores SIGTERM must still be force-killed and reaped within a
// bounded window — teardown never hangs and never leaves the process running.
void LibraryAutoTaggerTest::testTeardownForceKillsUncooperativeChild()
{
    const QUrl url = touchPdf(QStringLiteral("doc.pdf"));
    LibraryStore store(storePath());
    store.recordOpen(url, QDateTime(QDate(2026, 1, 1), QTime(10, 0)));

    const QString pidFile = m_dir->filePath(QStringLiteral("child.pid"));

    auto *tagger = new LibraryAutoTagger(&store);
    // A pdftotext that ignores SIGTERM and never exits on its own
    tagger->setPdfToTextExecutable(writeScript(QStringLiteral("pdftotext"), QStringLiteral("echo $$ > '%1'\ntrap '' TERM\nwhile :; do sleep 0.1; done").arg(pidFile)));
    tagger->setClaudeExecutable(writeScript(QStringLiteral("claude"), QStringLiteral("echo x")));

    tagger->enqueue(url);
    QTRY_VERIFY(QFile::exists(pidFile));
    const qint64 pid = readPid(pidFile);
    QVERIFY(processAlive(pid));

    QElapsedTimer timer;
    timer.start();
    delete tagger; // terminate() ignored → escalate to kill(), then reap
    QVERIFY(timer.elapsed() < 3500);

    QTRY_VERIFY_WITH_TIMEOUT(!processAlive(pid), 3000); // reaped, not lingering
}

QTEST_GUILESS_MAIN(LibraryAutoTaggerTest)
#include "libraryautotaggertest.moc"
