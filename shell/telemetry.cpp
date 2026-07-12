/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "telemetry.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTimer>

#include <chrono>

#include <fcntl.h>
#include <unistd.h>

// Baked in at compile time by CMake so a regression can be blamed on a build. Fall back to
// "unknown" when built outside the CMake path (e.g. a test that compiles this file directly).
#ifndef PAPERLIBRARY_GIT_COMMIT
#define PAPERLIBRARY_GIT_COMMIT "unknown"
#endif
#ifndef PAPERLIBRARY_BUILD_TS
#define PAPERLIBRARY_BUILD_TS "unknown"
#endif

namespace
{
qint64 steadyNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}

Telemetry &Telemetry::instance()
{
    static Telemetry telemetry;
    return telemetry;
}

Telemetry::Telemetry(QObject *parent)
    : QObject(parent)
{
}

QString Telemetry::logPath()
{
    const QByteArray override = qgetenv("PAPERLIBRARY_TELEMETRY_PATH");
    if (!override.isEmpty()) {
        return QString::fromLocal8Bit(override);
    }
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/telemetry.jsonl");
}

QString Telemetry::buildId()
{
    return QStringLiteral(PAPERLIBRARY_GIT_COMMIT);
}

void Telemetry::setStallThresholdMs(int ms)
{
    m_stallThresholdMs = qMax(1, ms);
}

int Telemetry::stallThresholdMs() const
{
    return m_stallThresholdMs;
}

void Telemetry::setFreezeThresholdMs(int ms)
{
    m_freezeThresholdMs = qMax(1, ms);
}

void Telemetry::setContext(const QString &key, const QString &value)
{
    m_context.insert(key, value);
}

void Telemetry::setContext(const QString &key, int value)
{
    m_context.insert(key, value);
}

void Telemetry::setOperation(const QString &op)
{
    m_context.insert(QStringLiteral("op"), op); // for regular events on the main thread
    if (!op.isEmpty()) {
        // Remember the last real operation. A recovered ui_stall is logged only after the operation
        // returns and its scope has cleared op, so the live context is empty by then; attribute the
        // stall to whatever last ran instead of losing it. (The in-progress freeze reads the live op
        // and does not need this.)
        m_lastOperation = op;
    }
    QMutexLocker lock(&m_opMutex);              // for the watchdog, which reads off-thread
    m_currentOp = op;
}

QString Telemetry::currentOperation() const
{
    QMutexLocker lock(&m_opMutex);
    return m_currentOp;
}

void Telemetry::writeLine(const QJsonObject &object)
{
    // Thread-safe: a single write() to an O_APPEND fd is atomic per line, and this opens the file
    // fresh each call and touches no shared member. The main thread and the freeze-watch thread
    // both use it. fsync means the breadcrumb is on disk before whatever it describes can crash.
    const QString path = logPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    const int fd = ::open(QFile::encodeName(path).constData(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) {
        return; // telemetry must never be the thing that breaks the app
    }
    const QByteArray line = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    const ssize_t written = ::write(fd, line.constData(), line.size());
    Q_UNUSED(written);
    ::fsync(fd);
    ::close(fd);
}

void Telemetry::logEvent(const QString &type, const QJsonObject &fields)
{
    QJsonObject event;
    event.insert(QStringLiteral("ts"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    event.insert(QStringLiteral("session"), m_sessionId);
    event.insert(QStringLiteral("seq"), double(m_seq++));
    event.insert(QStringLiteral("type"), type);
    for (auto it = m_context.constBegin(); it != m_context.constEnd(); ++it) {
        event.insert(it.key(), it.value());
    }
    for (auto it = fields.constBegin(); it != fields.constEnd(); ++it) {
        event.insert(it.key(), it.value());
    }
    writeLine(event);
}

std::optional<QJsonObject> Telemetry::reconcilePreviousSession(const QList<QJsonObject> &events)
{
    // Find the most recent session that opened, and whether it also closed. A session_start with
    // no later session_end for the same id is a session that died without shutting down.
    QString lastSession;
    for (const QJsonObject &event : events) {
        const QString type = event.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("session_start")) {
            lastSession = event.value(QStringLiteral("session")).toString();
        }
    }
    if (lastSession.isEmpty()) {
        return std::nullopt;
    }
    QJsonObject lastEventOfSession;
    bool endedCleanly = false;
    for (const QJsonObject &event : events) {
        if (event.value(QStringLiteral("session")).toString() != lastSession) {
            continue;
        }
        lastEventOfSession = event;
        if (event.value(QStringLiteral("type")).toString() == QStringLiteral("session_end")) {
            endedCleanly = true;
        }
    }
    if (endedCleanly) {
        return std::nullopt;
    }
    QJsonObject incident;
    incident.insert(QStringLiteral("crashed_session"), lastSession);
    incident.insert(QStringLiteral("last_event"), lastEventOfSession.value(QStringLiteral("type")));
    // Carry forward what the dead session was last doing -- the context on its final breadcrumb.
    for (const QString &key : {QStringLiteral("op"), QStringLiteral("tab_kind"),
                               QStringLiteral("tab_count"), QStringLiteral("last_action"),
                               QStringLiteral("document")}) {
        if (lastEventOfSession.contains(key)) {
            incident.insert(QStringLiteral("last_") + key, lastEventOfSession.value(key));
        }
    }
    return incident;
}

void Telemetry::start()
{
    if (m_started) {
        return;
    }
    m_started = true;

    // A session id that does not need a clock the tests can't control: time + a random tail.
    m_sessionId = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddHHmmss"))
        + QStringLiteral("-") + QString::number(QRandomGenerator::global()->generate(), 16);

    // Reconcile a previous crash BEFORE writing this session's start, reading the log as it stands.
    QFile existing(logPath());
    if (existing.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QList<QJsonObject> events;
        while (!existing.atEnd()) {
            const QByteArray line = existing.readLine().trimmed();
            if (line.isEmpty()) {
                continue;
            }
            const QJsonDocument doc = QJsonDocument::fromJson(line);
            if (doc.isObject()) {
                events.append(doc.object());
            }
        }
        existing.close();
        if (const std::optional<QJsonObject> incident = reconcilePreviousSession(events)) {
            logEvent(QStringLiteral("session_incomplete"), *incident);
        }
    }

    logEvent(QStringLiteral("session_start"),
             {{QStringLiteral("build"), QStringLiteral(PAPERLIBRARY_GIT_COMMIT)},
              {QStringLiteral("build_ts"), QStringLiteral(PAPERLIBRARY_BUILD_TS)}});

    // The recovered-stall watchdog: a timer that should fire every m_heartbeatIntervalMs measures
    // how late it actually fired; lateness beyond the threshold is main-thread block time it lived
    // through. It also stamps m_lastAliveMs, which the freeze thread watches.
    m_lastAliveMs.store(steadyNowMs());
    m_sinceLastTick.start();
    m_heartbeat = new QTimer(this);
    m_heartbeat->setTimerType(Qt::PreciseTimer);
    m_heartbeat->setInterval(m_heartbeatIntervalMs);
    connect(m_heartbeat, &QTimer::timeout, this, &Telemetry::onHeartbeat);
    m_heartbeat->start();

    // The in-progress-freeze watchdog: a plain thread that notices when the main thread stops
    // updating m_lastAliveMs and records the freeze from off-thread, so it survives a force-quit.
    m_freezeWatchRunning.store(true);
    m_freezeThread = std::thread([this]() { freezeWatchLoop(); });
}

void Telemetry::onHeartbeat()
{
    m_lastAliveMs.store(steadyNowMs()); // tell the freeze thread the main loop is alive
    const qint64 elapsed = m_sinceLastTick.restart();
    const qint64 lateness = elapsed - m_heartbeatIntervalMs;
    if (lateness >= m_stallThresholdMs) {
        QJsonObject fields{{QStringLiteral("stall_ms"), double(lateness)}};
        if (m_context.value(QStringLiteral("op")).toString().isEmpty() && !m_lastOperation.isEmpty()) {
            fields.insert(QStringLiteral("op"), m_lastOperation); // the op that just ran and blocked
        }
        logEvent(QStringLiteral("ui_stall"), fields);
    }
}

void Telemetry::freezeWatchLoop()
{
    bool reportedThisEpisode = false;
    while (m_freezeWatchRunning.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (!m_freezeWatchRunning.load()) {
            break;
        }
        const qint64 stuckMs = steadyNowMs() - m_lastAliveMs.load();
        if (stuckMs >= m_freezeThresholdMs) {
            if (!reportedThisEpisode) {
                // Build the event ourselves (off the main thread): only m_sessionId (const after
                // start) and the mutex-guarded op are read, and writeLine opens the file per call.
                QJsonObject event;
                event.insert(QStringLiteral("ts"),
                             QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
                event.insert(QStringLiteral("session"), m_sessionId);
                event.insert(QStringLiteral("seq"),
                             QString(QStringLiteral("f") + QString::number(m_freezeSeq.fetch_add(1))));
                event.insert(QStringLiteral("type"), QStringLiteral("hang_in_progress"));
                event.insert(QStringLiteral("stuck_ms"), double(stuckMs));
                event.insert(QStringLiteral("op"), currentOperation());
                writeLine(event);
                reportedThisEpisode = true;
            }
        } else {
            reportedThisEpisode = false; // main thread recovered; arm for the next freeze
        }
    }
}

void Telemetry::stop()
{
    if (!m_started) {
        return;
    }
    if (m_heartbeat) {
        m_heartbeat->stop();
    }
    m_freezeWatchRunning.store(false);
    if (m_freezeThread.joinable()) {
        m_freezeThread.join();
    }
    logEvent(QStringLiteral("session_end"));
    m_started = false;
}

TelemetryScope::TelemetryScope(const QString &op)
    : m_previous(Telemetry::instance().currentOperation())
{
    Telemetry::instance().setOperation(op);
}

TelemetryScope::~TelemetryScope()
{
    Telemetry::instance().setOperation(m_previous);
}
