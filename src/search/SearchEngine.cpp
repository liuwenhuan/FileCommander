#include "SearchEngine.h"

#include <QDirIterator>
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

        while (it.hasNext()) {
            if (m_cancelled.load())
                break;
            const QString path = it.next();
            if (regex.match(it.fileName()).hasMatch())
                emit resultFound(path);
        }

        m_running = false;
        emit finished();
    });
}
