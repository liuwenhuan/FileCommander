#include "SearchEngine.h"

#include <QDirIterator>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QtConcurrent/QtConcurrent>

SearchEngine::SearchEngine(QObject *parent) : QObject(parent) {}

void SearchEngine::cancel() {
    m_cancelled = true;
}

void SearchEngine::start(const QString &rootPath, const QString &namePattern, bool caseSensitive,
                          bool includeSubdirs) {
    m_cancelled = false;
    m_running = true;
    m_truncated = false;
    emit started();

    QRegularExpression::PatternOptions options = caseSensitive
                                                      ? QRegularExpression::NoPatternOption
                                                      : QRegularExpression::CaseInsensitiveOption;
    QRegularExpression regex(QRegularExpression::wildcardToRegularExpression(namePattern),
                              options);

    // `this` is only touched from lambda invocations that run while this
    // SearchEngine is still alive -- SearchDialog defers its own
    // destruction until the `finished` signal below has fired (see
    // SearchDialog::closeEvent), so there is no dangling-this risk despite
    // running on a background thread.
    QtConcurrent::run([this, rootPath, regex, includeSubdirs]() {
        const QDir::Filters filters = QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden;
        const QDirIterator::IteratorFlags flags =
            includeSubdirs ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags;
        QDirIterator it(rootPath, filters, flags);

        // Deliver matches in throttled batches (by count or elapsed time) rather
        // than one queued signal per file. A wildcard search of a large tree
        // matches hundreds of thousands of files; a per-file emit + addItem
        // would swamp the GUI thread's event loop so it never handles the Stop
        // click or repaints, and the window appears frozen.
        constexpr int kBatchSize = 256;
        constexpr qint64 kFlushMs = 80;
        QStringList batch;
        QElapsedTimer sinceFlush;
        sinceFlush.start();
        auto flush = [&]() {
            if (batch.isEmpty())
                return;
            emit resultsFound(batch);
            batch.clear();
            sinceFlush.restart();
        };

        int total = 0;
        while (it.hasNext()) {
            if (m_cancelled.load())
                break;
            const QString path = it.next();
            if (regex.match(it.fileName()).hasMatch()) {
                batch.append(path);
                if (batch.size() >= kBatchSize || sinceFlush.elapsed() >= kFlushMs)
                    flush();
                if (++total >= kMaxResults) {
                    // Stop at the cap: keeps the GUI list bounded and, more
                    // importantly, ends the traversal so the worker isn't still
                    // churning while the user reads truncated results.
                    m_truncated = true;
                    break;
                }
            }
        }
        flush();

        m_running = false;
        emit finished();
    });
}
