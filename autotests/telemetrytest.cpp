/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <QtTest>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "../shell/telemetry.h"

class TelemetryTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init();
    void testAPreviousSessionWithoutAnEndIsReportedAsACrash();
    void testACleanlyEndedSessionIsNotReportedAsACrash();
    void testTheCrashReportCarriesWhatTheDeadSessionWasDoing();
    void testABlockedMainThreadIsLoggedAsAStall();
    void testABriefTickIsNotLoggedAsAStall();
    void testAnOperationScopeAttributesEventsAndRestoresOnExit();
    void testSessionStartCarriesTheBuildId();
    void testAnInProgressFreezeIsLoggedFromOffThread();

private:
    QList<QJsonObject> readLog() const;
    QTemporaryDir m_dir;
    QString m_logPath;
};

QList<QJsonObject> TelemetryTest::readLog() const
{
    QList<QJsonObject> events;
    QFile file(m_logPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return events;
    }
    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty()) {
            continue;
        }
        events.append(QJsonDocument::fromJson(line).object());
    }
    return events;
}

void TelemetryTest::init()
{
    // A fresh log per test, pointed at by the env override the class honours.
    m_logPath = m_dir.filePath(QStringLiteral("telemetry-%1.jsonl").arg(QRandomGenerator::global()->generate()));
    qputenv("PAPERLIBRARY_TELEMETRY_PATH", QFile::encodeName(m_logPath));
}

void TelemetryTest::testAPreviousSessionWithoutAnEndIsReportedAsACrash()
{
    const QList<QJsonObject> log = {
        QJsonObject{{QStringLiteral("type"), QStringLiteral("session_start")}, {QStringLiteral("session"), QStringLiteral("A")}},
        QJsonObject{{QStringLiteral("type"), QStringLiteral("tab_open")}, {QStringLiteral("session"), QStringLiteral("A")}},
        // no session_end for A -> A crashed
    };
    const auto incident = Telemetry::reconcilePreviousSession(log);
    QVERIFY(incident.has_value());
    QCOMPARE(incident->value(QStringLiteral("crashed_session")).toString(), QStringLiteral("A"));
}

void TelemetryTest::testACleanlyEndedSessionIsNotReportedAsACrash()
{
    const QList<QJsonObject> log = {
        QJsonObject{{QStringLiteral("type"), QStringLiteral("session_start")}, {QStringLiteral("session"), QStringLiteral("A")}},
        QJsonObject{{QStringLiteral("type"), QStringLiteral("session_end")}, {QStringLiteral("session"), QStringLiteral("A")}},
    };
    QVERIFY(!Telemetry::reconcilePreviousSession(log).has_value());
}

void TelemetryTest::testTheCrashReportCarriesWhatTheDeadSessionWasDoing()
{
    const QList<QJsonObject> log = {
        QJsonObject{{QStringLiteral("type"), QStringLiteral("session_start")}, {QStringLiteral("session"), QStringLiteral("A")}},
        QJsonObject{{QStringLiteral("type"), QStringLiteral("tab_switch")}, {QStringLiteral("session"), QStringLiteral("A")},
                    {QStringLiteral("tab_kind"), QStringLiteral("pdf")}, {QStringLiteral("tab_count"), 7}},
    };
    const auto incident = Telemetry::reconcilePreviousSession(log);
    QVERIFY(incident.has_value());
    QCOMPARE(incident->value(QStringLiteral("last_event")).toString(), QStringLiteral("tab_switch"));
    QCOMPARE(incident->value(QStringLiteral("last_tab_kind")).toString(), QStringLiteral("pdf"));
    QCOMPARE(incident->value(QStringLiteral("last_tab_count")).toInt(), 7);
}

void TelemetryTest::testABlockedMainThreadIsLoggedAsAStall()
{
    Telemetry &t = Telemetry::instance();
    t.setStallThresholdMs(200);
    t.start();

    {
        // Block inside a named operation. The scope closes before the heartbeat fires (after the
        // block recovers), so the stall must still attribute to it via the remembered last op.
        TelemetryScope op(QStringLiteral("heavy_thing"));
        QElapsedTimer block;
        block.start();
        while (block.elapsed() < 500) {
            // busy-wait: the event loop cannot run, so the heartbeat cannot tick on time
        }
    }
    // Now let the heartbeat fire and notice its own lateness.
    QCoreApplication::processEvents();
    QTest::qWait(150);
    t.stop();

    bool sawStall = false;
    qint64 worst = 0;
    QString stallOp;
    for (const QJsonObject &event : readLog()) {
        if (event.value(QStringLiteral("type")).toString() == QStringLiteral("ui_stall")) {
            sawStall = true;
            worst = qMax(worst, qint64(event.value(QStringLiteral("stall_ms")).toDouble()));
            stallOp = event.value(QStringLiteral("op")).toString();
        }
    }
    QVERIFY2(sawStall, "a 500ms main-thread block was not logged as a ui_stall");
    QVERIFY2(worst >= 200, "the logged stall was shorter than the block");
    QCOMPARE(stallOp, QStringLiteral("heavy_thing")); // recovered stall attributes to the op that ran
}

void TelemetryTest::testABriefTickIsNotLoggedAsAStall()
{
    Telemetry &t = Telemetry::instance();
    t.setStallThresholdMs(400);
    t.start();
    QTest::qWait(350); // several normal heartbeats, none blocked past the threshold
    t.stop();

    for (const QJsonObject &event : readLog()) {
        QVERIFY2(event.value(QStringLiteral("type")).toString() != QStringLiteral("ui_stall"),
                 "an idle run logged a phantom stall");
    }
}

void TelemetryTest::testAnOperationScopeAttributesEventsAndRestoresOnExit()
{
    Telemetry &t = Telemetry::instance();
    t.start();
    {
        TelemetryScope outer(QStringLiteral("outer_op"));
        t.logEvent(QStringLiteral("probe"));
        {
            TelemetryScope inner(QStringLiteral("inner_op"));
            t.logEvent(QStringLiteral("probe"));
        }
        t.logEvent(QStringLiteral("probe")); // back to outer after the inner scope closes
    }
    t.logEvent(QStringLiteral("probe")); // no scope: op cleared
    t.stop();

    QStringList ops;
    for (const QJsonObject &e : readLog()) {
        if (e.value(QStringLiteral("type")).toString() == QStringLiteral("probe")) {
            ops << e.value(QStringLiteral("op")).toString();
        }
    }
    QCOMPARE(ops, (QStringList{QStringLiteral("outer_op"), QStringLiteral("inner_op"),
                               QStringLiteral("outer_op"), QString()}));
}

void TelemetryTest::testSessionStartCarriesTheBuildId()
{
    Telemetry &t = Telemetry::instance();
    t.start();
    t.stop();
    bool sawBuild = false;
    for (const QJsonObject &e : readLog()) {
        if (e.value(QStringLiteral("type")).toString() == QStringLiteral("session_start")) {
            sawBuild = e.contains(QStringLiteral("build")); // "unknown" in the test build, but present
        }
    }
    QVERIFY2(sawBuild, "session_start must carry the build id so a regression can be attributed");
}

void TelemetryTest::testAnInProgressFreezeIsLoggedFromOffThread()
{
    Telemetry &t = Telemetry::instance();
    t.setFreezeThresholdMs(300); // a freeze the watchdog thread will notice quickly
    t.start();
    {
        TelemetryScope op(QStringLiteral("frozen_op"));
        // Block the MAIN thread outright -- no event loop, so the recovered-stall heartbeat can't
        // even fire. Only the separate watchdog thread can notice and record this.
        QElapsedTimer block;
        block.start();
        while (block.elapsed() < 900) {
            // hard busy-wait: the freeze thread runs concurrently and must log the hang
        }
    }
    QTest::qWait(150); // let the watchdog's write settle
    t.stop();

    QJsonObject hang;
    for (const QJsonObject &e : readLog()) {
        if (e.value(QStringLiteral("type")).toString() == QStringLiteral("hang_in_progress")) {
            hang = e;
        }
    }
    QVERIFY2(!hang.isEmpty(), "a 900ms main-thread freeze was not caught by the watchdog thread");
    QVERIFY2(hang.value(QStringLiteral("stuck_ms")).toDouble() >= 300, "the logged freeze was too short");
    QCOMPARE(hang.value(QStringLiteral("op")).toString(), QStringLiteral("frozen_op"));
}

QTEST_MAIN(TelemetryTest)
#include "telemetrytest.moc"
