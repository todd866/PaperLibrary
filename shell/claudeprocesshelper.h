/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef PAPERLIBRARY_CLAUDEPROCESSHELPER_H
#define PAPERLIBRARY_CLAUDEPROCESSHELPER_H

#include <QByteArray>
#include <QJsonDocument>
#include <QString>

namespace ClaudeProcessHelper
{
QString findClaudeExecutable();
QJsonDocument parseClaudeJsonResult(const QByteArray &reply);
}

#endif
