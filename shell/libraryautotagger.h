/*
    SPDX-FileCopyrightText: 2026 Ian Todd <todd.ian@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef PAPERLIBRARY_LIBRARYAUTOTAGGER_H
#define PAPERLIBRARY_LIBRARYAUTOTAGGER_H

#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>

class LibraryStore;
class QProcess;

/**
 * Fills in library metadata (title, tags, description, keywords) for
 * documents that have none by asking the local `claude` CLI to read the
 * first page. Documents queue up and are processed one at a time in the
 * background: the first page's text is extracted with pdftotext
 * (documents without extractable text — e.g. scans — are skipped
 * silently), truncated, and sent to `claude -p --output-format json`; a
 * valid reply is written back through the LibraryStore setters and
 * announced via tagged(). Any failure is remembered for the session and
 * never retried, never blocks the UI and never surfaces document content
 * anywhere.
 *
 * Gated by the LibraryAutoTag setting (paperlibraryrc [General], default
 * off) and by the CLI being installed. Read-only towards the documents:
 * only the first page's text ever leaves the process, and only to the
 * local CLI; nothing but the library store is written.
 */
class LibraryAutoTagger : public QObject
{
    Q_OBJECT

public:
    explicit LibraryAutoTagger(LibraryStore *store, QObject *parent = nullptr);
    ~LibraryAutoTagger() override;

    /**
     * Queue @p url for tagging. A no-op when auto-tagging is disabled,
     * the entry already has a title, the claude CLI is not installed, or
     * the url was already queued or failed this session.
     */
    void enqueue(const QUrl &url);

    /** The metadata suggestion parsed out of a CLI reply. */
    struct Suggestion {
        QString title;
        QStringList tags;
        QString description;
        QStringList keywords;
        bool valid = false; /**< false on any parse trouble or a missing title */
    };

    /**
     * Parses a `claude -p --output-format json` reply: the CLI wraps the
     * model's text in an envelope ({"type":"result","is_error":false,…,
     * "result":"<text>"}), and the text is expected to be the JSON object
     * the prompt asked for. Defensive on every level: anything unexpected
     * yields an invalid Suggestion.
     */
    static Suggestion parseReply(const QByteArray &reply);

    /** Test seams: override the executables resolved at construction. */
    void setClaudeExecutable(const QString &path);
    void setPdfToTextExecutable(const QString &path);

Q_SIGNALS:
    /** New metadata for @p url has been written to the store. */
    void tagged(const QUrl &url);

private:
    void startNext();
    void extractFirstPageText(const QUrl &url);
    void askClaude(const QUrl &url, const QString &firstPageText);
    void finishCurrent();
    QProcess *makeProcess(int timeoutMs);

    friend class LibraryAutoTaggerTest;

    LibraryStore *m_store;
    QString m_claude;
    QString m_pdfToText;
    QList<QUrl> m_queue;
    QSet<QString> m_seen; /**< everything queued this session; failures are never retried */
    bool m_busy = false;
};

#endif
