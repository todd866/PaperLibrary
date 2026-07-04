/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "claudeprocesshelper.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonObject>
#include <QStandardPaths>
#include <QStringList>

namespace
{
QJsonDocument parseJsonSlice(const QString &text, QLatin1Char openChar, QLatin1Char closeChar)
{
    const int open = text.indexOf(openChar);
    const int close = text.lastIndexOf(closeChar);
    if (open < 0 || close <= open) {
        return {};
    }
    return QJsonDocument::fromJson(text.mid(open, close - open + 1).toUtf8());
}
}

namespace ClaudeProcessHelper
{
QString findClaudeExecutable()
{
    const QString onPath = QStandardPaths::findExecutable(QStringLiteral("claude"));
    if (!onPath.isEmpty()) {
        return onPath;
    }

    const QStringList fallbacks = {QDir::homePath() + QStringLiteral("/.local/bin/claude"), QStringLiteral("/opt/homebrew/bin/claude")};
    for (const QString &fallback : fallbacks) {
        if (QFileInfo(fallback).isExecutable()) {
            return fallback;
        }
    }
    return QString();
}

QJsonDocument parseClaudeJsonResult(const QByteArray &reply)
{
    const QJsonDocument envelope = QJsonDocument::fromJson(reply);
    if (!envelope.isObject()) {
        return {};
    }

    const QJsonObject outer = envelope.object();
    if (outer.value(QLatin1String("is_error")).toBool(true)) {
        return {};
    }

    const QString text = outer.value(QLatin1String("result")).toString();

    const int arrayOpen = text.indexOf(QLatin1Char('['));
    const int objectOpen = text.indexOf(QLatin1Char('{'));
    const bool preferArray = arrayOpen >= 0 && (objectOpen < 0 || arrayOpen < objectOpen);

    QJsonDocument inner = preferArray ? parseJsonSlice(text, QLatin1Char('['), QLatin1Char(']')) : parseJsonSlice(text, QLatin1Char('{'), QLatin1Char('}'));
    if ((preferArray && inner.isArray()) || (!preferArray && inner.isObject())) {
        return inner;
    }

    inner = preferArray ? parseJsonSlice(text, QLatin1Char('{'), QLatin1Char('}')) : parseJsonSlice(text, QLatin1Char('['), QLatin1Char(']'));
    if (inner.isObject() || inner.isArray()) {
        return inner;
    }

    return {};
}
}
