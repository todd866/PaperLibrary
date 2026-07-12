/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef PAPERLIBRARY_TELEMETRY_H
#define PAPERLIBRARY_TELEMETRY_H

#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QMutex>
#include <QObject>
#include <QString>

#include <atomic>
#include <optional>
#include <thread>

class QTimer;

/**
 * A local, append-only incident log for PaperLibrary.
 *
 * The app should tell us how it actually behaves rather than leave the user to reconstruct a
 * hang or a crash from memory. Every session, tab action, UI-thread stall and suspected crash
 * is written as one JSON object per line to a file the user owns. Nothing leaves the machine;
 * there is no network path. `telemetry_report.py` in the backend reads it back.
 *
 * What it captures:
 *
 *   ui_stall   The event loop was blocked past the threshold and then RECOVERED. A heartbeat
 *              timer measures its own lateness; the current context -- op, tab kind, tab count --
 *              is attached, so the stall names what was running, not just which tab.
 *
 *   hang_in_progress  The event loop is blocked RIGHT NOW and may never recover. A separate
 *              watchdog thread notices the main thread has stopped ticking and writes this from
 *              off-thread, so a freeze that ends in a force-quit still leaves its duration and op
 *              on disk -- the incident that takes down the reporter still files a report.
 *
 *   session_incomplete  The previous session wrote session_start but never session_end -- a crash
 *              or force-quit. On the next start we reconcile it and record what it was last doing.
 *
 * Every session_start carries the build (git commit + timestamp), so a regression can be blamed
 * on a change. The log is fsync'd per event, so a crash cannot lose the breadcrumb before it.
 */
class Telemetry : public QObject
{
    Q_OBJECT

public:
    static Telemetry &instance();

    /** Open the log, reconcile a previous crash, write session_start, start the watchdogs. */
    void start();

    /** Write session_end and stop the watchdogs. A clean shutdown; its absence next time is a crash. */
    void stop();

    /** Append one event. `type` names it; `fields` are merged in after ts/session/seq/context. */
    void logEvent(const QString &type, const QJsonObject &fields = {});

    /** Set a piece of rolling context (e.g. "tab_kind"->"pdf") attached to stalls and events. */
    void setContext(const QString &key, const QString &value);
    void setContext(const QString &key, int value);

    /**
     * Name the operation running right now, so a stall (recovered or in-progress) attributes to
     * it. Prefer the RAII TelemetryScope over calling this directly. Thread-safe to read from the
     * watchdog. Passing an empty string clears the operation.
     */
    void setOperation(const QString &op);
    QString currentOperation() const;

    /** Milliseconds of event-loop lateness above which a RECOVERED tick is logged as a stall. */
    void setStallThresholdMs(int ms);
    int stallThresholdMs() const;

    /** Milliseconds the main thread may be unresponsive before the watchdog logs hang_in_progress. */
    void setFreezeThresholdMs(int ms);

    /** Where the log is written. Honours PAPERLIBRARY_TELEMETRY_PATH; else the app data dir. */
    static QString logPath();

    /** The build this binary was compiled from: git commit + timestamp, from compile defines. */
    static QString buildId();

    /**
     * Given the existing log's events in order, decide whether the most recent *prior* session
     * ended cleanly. Returns the reconciliation event to record if it did not, else nullopt.
     * Pure and static so the crash logic can be tested without a running app.
     */
    static std::optional<QJsonObject> reconcilePreviousSession(const QList<QJsonObject> &events);

private:
    explicit Telemetry(QObject *parent = nullptr);
    void writeLine(const QJsonObject &object); // thread-safe: opens the file per call, no shared state
    void onHeartbeat();
    void freezeWatchLoop();

    QString m_sessionId;
    quint64 m_seq = 0;
    QHash<QString, QJsonValue> m_context;
    QTimer *m_heartbeat = nullptr;
    QElapsedTimer m_sinceLastTick;
    int m_heartbeatIntervalMs = 100;
    int m_stallThresholdMs = 250;
    bool m_started = false;

    // Shared with the freeze-watch thread. m_sessionId is const after start(); the rest are these.
    QString m_lastOperation;                // main-thread only: last non-empty op, for recovered stalls
    mutable QMutex m_opMutex;               // guards m_currentOp (written main thread, read watcher)
    QString m_currentOp;
    std::atomic<qint64> m_lastAliveMs{0};   // steady-clock ms of the last main-thread heartbeat
    std::atomic<bool> m_freezeWatchRunning{false};
    std::atomic<quint64> m_freezeSeq{0};    // seq for off-thread writes; disjoint from m_seq
    std::thread m_freezeThread;
    int m_freezeThresholdMs = 1000;
};

/**
 * RAII operation breadcrumb. While it is alive, telemetry attributes stalls to `op`; on scope exit
 * the previous operation is restored, so nested scopes read correctly.
 *
 *   TelemetryScope op(QStringLiteral("shelf_rebuild"));
 */
class TelemetryScope
{
public:
    explicit TelemetryScope(const QString &op);
    ~TelemetryScope();
    TelemetryScope(const TelemetryScope &) = delete;
    TelemetryScope &operator=(const TelemetryScope &) = delete;

private:
    QString m_previous;
};

#endif // PAPERLIBRARY_TELEMETRY_H
