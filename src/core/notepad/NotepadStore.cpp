#include "NotepadStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>
#include <QUuid>

#include "config/Settings.h"

namespace {

// Notes live under the shared config directory so every store agrees on where
// configuration lives (Settings owns that path).
QString notepadDir() {
    const QString configDir = Settings::configDir();
    if (configDir.isEmpty())
        return QString();
    const QString dir = configDir + QStringLiteral("/notepad");
    if (!QDir().mkpath(dir))
        return QString();
    return dir;
}

bool isValidNoteId(const QString &id) {
    if (id.size() != 36)
        return false;
    const QUuid uuid(id);
    return !uuid.isNull() && uuid.toString(QUuid::WithoutBraces) == id;
}

} // namespace

NotepadStore::NotepadStore() : NotepadStore(notepadDir()) {}

NotepadStore::NotepadStore(const QString &directory)
    : m_dir(directory.isEmpty() ? notepadDir() : directory) {
    if (m_dir.isEmpty() || !QDir().mkpath(m_dir))
        return;
    const QFileInfo dirInfo(m_dir);
    if (!dirInfo.isDir() || dirInfo.isSymbolicLink())
        return;
    m_dir = dirInfo.canonicalFilePath();
    if (m_dir.isEmpty() || !QFile::setPermissions(
                               m_dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                          QFileDevice::ExeOwner))
        return;

    QLockFile lock(lockPath());
    if (!lock.tryLock(5000))
        return;
    if (!synchronizeFromDisk())
        m_available = false;
}

QString NotepadStore::indexPath() const {
    return m_dir.isEmpty() ? QString() : m_dir + QStringLiteral("/index.json");
}

QString NotepadStore::lockPath() const {
    return m_dir.isEmpty() ? QString() : m_dir + QStringLiteral("/.index.lock");
}

QString NotepadStore::filePathFor(const QString &id) const {
    if (m_dir.isEmpty() || !isValidNoteId(id))
        return QString();
    return m_dir + QStringLiteral("/") + id + QStringLiteral(".txt");
}

QString NotepadStore::tombstonePathFor(const QString &id) const {
    if (m_dir.isEmpty() || !isValidNoteId(id))
        return QString();
    return m_dir + QStringLiteral("/") + id + QStringLiteral(".txt.deleting");
}

bool NotepadStore::isSafeBodyPath(const QString &path, bool allowMissing) const {
    if (path.isEmpty())
        return false;
    const QFileInfo info(path);
    if (!info.exists())
        return allowMissing && !info.isSymbolicLink();
    return info.isFile() && !info.isSymbolicLink();
}

bool NotepadStore::recoverPendingDeletes() {
    QDir dir(m_dir);
    const QStringList tombstones = dir.entryList(
        QStringList{QStringLiteral("*.txt.deleting")}, QDir::Files | QDir::NoSymLinks, QDir::Name);
    for (const QString &fileName : tombstones) {
        const QString suffix = QStringLiteral(".txt.deleting");
        const QString id = fileName.left(fileName.size() - suffix.size());
        if (!isValidNoteId(id))
            continue;

        const QString tombstone = dir.filePath(fileName);
        const QString body = filePathFor(id);
        if (indexOf(id) >= 0) {
            if (QFileInfo::exists(body) || !QFile::rename(tombstone, body))
                return false;
            if (!QFile::setPermissions(body, QFileDevice::ReadOwner | QFileDevice::WriteOwner))
                return false;
        } else if (!QFile::remove(tombstone)) {
            return false;
        }
    }
    return true;
}

bool NotepadStore::recoverOrphanedBodies() {
    QDir dir(m_dir);
    const QStringList txtFiles = dir.entryList(QStringList{QStringLiteral("*.txt")},
                                               QDir::Files | QDir::NoSymLinks, QDir::Name);
    QVector<NotepadNote> recovered = m_notes;
    for (const QString &fileName : txtFiles) {
        const QString id = QFileInfo(fileName).completeBaseName();
        if (!isValidNoteId(id) || indexOf(id) >= 0)
            continue;
        NotepadNote note;
        note.id = id;
        note.title = id;
        note.filePath = filePathFor(id);
        recovered.append(note);
    }
    if (recovered.size() == m_notes.size())
        return true;
    if (!saveIndex(recovered))
        return false;
    m_notes = recovered;
    return true;
}

bool NotepadStore::synchronizeFromDisk() {
    const QVector<NotepadNote> previous = m_notes;
    const bool wasAvailable = m_available;
    loadIndex();
    if (!m_available) {
        m_notes = previous;
        m_available = wasAvailable;
        return false;
    }

    // An indexed body replaced by a directory or symlink is an I/O failure, not
    // a concurrent deletion. Do not let a refresh silently erase that note from
    // this instance's state; the caller must report failure and preserve the UI.
    for (const NotepadNote &note : previous) {
        if (indexOf(note.id) >= 0)
            continue;
        const QFileInfo body(note.filePath);
        if (body.exists() || body.isSymbolicLink()) {
            m_notes = previous;
            m_available = wasAvailable;
            return false;
        }
    }

    if (!recoverPendingDeletes() || !recoverOrphanedBodies()) {
        m_notes = previous;
        m_available = wasAvailable;
        return false;
    }
    return true;
}

int NotepadStore::indexOf(const QString &id) const {
    for (int i = 0; i < m_notes.size(); ++i) {
        if (m_notes.at(i).id == id)
            return i;
    }
    return -1;
}

void NotepadStore::loadIndex() {
    m_notes.clear();
    m_available = false;

    QFile file(indexPath());
    if (!file.exists()) {
        // Nothing saved yet: pick up any stray .txt files (e.g. after an index
        // was lost) so no note is orphaned, then persist a fresh index.
        rebuildFromDisk();
        return;
    }
    const QFileInfo indexInfo(file);
    if (!indexInfo.isFile() || indexInfo.isSymbolicLink() || !file.open(QIODevice::ReadOnly))
        return;

    const QByteArray raw = file.readAll();
    file.close();

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isArray()) {
        // Corrupt index: rebuild from the .txt files on disk.
        rebuildFromDisk();
        return;
    }

    // Read entries, honouring the stored "order" field.
    QVector<QPair<int, NotepadNote>> ordered;
    const QJsonArray array = doc.array();
    for (const QJsonValue &value : array) {
        if (!value.isObject())
            continue;
        const QJsonObject obj = value.toObject();
        const QString id = obj.value(QStringLiteral("id")).toString();
        if (!isValidNoteId(id))
            continue;
        NotepadNote note;
        note.id = id;
        note.title = obj.value(QStringLiteral("title")).toString();
        note.filePath = filePathFor(id);
        if (!isSafeBodyPath(note.filePath, true))
            continue;
        const int order = obj.value(QStringLiteral("order")).toInt(ordered.size());
        ordered.append({order, note});
    }

    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const QPair<int, NotepadNote> &a, const QPair<int, NotepadNote> &b) {
                         return a.first < b.first;
                     });
    for (const auto &entry : ordered)
        m_notes.append(entry.second);
    m_available = true;
}

void NotepadStore::rebuildFromDisk() {
    m_notes.clear();

    QDir dir(m_dir);
    const QStringList txtFiles = dir.entryList(QStringList{QStringLiteral("*.txt")},
                                               QDir::Files | QDir::NoSymLinks, QDir::Name);
    for (const QString &fileName : txtFiles) {
        NotepadNote note;
        note.id = QFileInfo(fileName).completeBaseName();
        if (!isValidNoteId(note.id))
            continue;
        note.title = note.id; // no title survived; fall back to the id
        note.filePath = filePathFor(note.id);
        m_notes.append(note);
    }

    m_available = saveIndex();
}

bool NotepadStore::saveIndex(const QVector<NotepadNote> &notes) const {
    const QString path = indexPath();
    if (path.isEmpty() || QFileInfo(path).isSymbolicLink())
        return false;

    QJsonArray array;
    for (int i = 0; i < notes.size(); ++i) {
        const NotepadNote &note = notes.at(i);
        QJsonObject obj;
        obj.insert(QStringLiteral("id"), note.id);
        obj.insert(QStringLiteral("title"), note.title);
        obj.insert(QStringLiteral("order"), i);
        array.append(obj);
    }

    // QSaveFile writes atomically, so a crash mid-write can't leave a truncated
    // index behind (loadIndex would otherwise rebuild it).
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        file.cancelWriting();
        return false;
    }
    const QByteArray data = QJsonDocument(array).toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size()) {
        file.cancelWriting();
        return false;
    }
    const bool committed = file.commit();
    if (committed)
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return committed;
}

bool NotepadStore::saveIndex() const {
    return saveIndex(m_notes);
}

QVector<NotepadNote> NotepadStore::notes() const {
    return m_notes;
}

NotepadNote NotepadStore::create(const QString &title) {
    NotepadNote note;
    if (!m_available || m_dir.isEmpty())
        return note;

    QLockFile lock(lockPath());
    if (!lock.tryLock(5000) || !synchronizeFromDisk())
        return note;

    note.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    note.title = title;
    note.filePath = filePathFor(note.id);
    if (!isSafeBodyPath(note.filePath, true))
        return {};

    QSaveFile file(note.filePath);
    if (!file.open(QIODevice::WriteOnly) || !file.commit() ||
        !QFile::setPermissions(note.filePath,
                               QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        QFile::remove(note.filePath);
        return {};
    }

    QVector<NotepadNote> updated = m_notes;
    updated.append(note);
    if (!saveIndex(updated)) {
        QFile::remove(note.filePath);
        return {};
    }
    m_notes = updated;
    return note;
}

bool NotepadStore::save(const QString &id, const QString &content,
                        const QString &expectedContent) {
    if (!m_available || !isValidNoteId(id))
        return false;

    QLockFile lock(lockPath());
    if (!lock.tryLock(5000) || !synchronizeFromDisk() || indexOf(id) < 0)
        return false;
    const QString path = filePathFor(id);
    if (!isSafeBodyPath(path, false))
        return false;

    QFile current(path);
    if (!current.open(QIODevice::ReadOnly) ||
        QString::fromUtf8(current.readAll()) != expectedContent) {
        return false;
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        file.cancelWriting();
        return false;
    }
    const QByteArray data = content.toUtf8();
    if (file.write(data) != data.size()) {
        file.cancelWriting();
        return false;
    }
    if (!file.commit())
        return false;
    return true;
}

QString NotepadStore::load(const QString &id) const {
    if (indexOf(id) < 0)
        return QString();
    const QString path = filePathFor(id);
    if (!isSafeBodyPath(path, false))
        return QString();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    const QString content = QString::fromUtf8(file.readAll());
    file.close();
    return content;
}

bool NotepadStore::remove(const QString &id) {
    if (!m_available || !isValidNoteId(id))
        return false;

    QLockFile lock(lockPath());
    if (!lock.tryLock(5000) || !synchronizeFromDisk())
        return false;
    const int pos = indexOf(id);
    if (pos < 0)
        return false;

    const QString path = filePathFor(id);
    const QString tombstone = tombstonePathFor(id);
    if (!isSafeBodyPath(path, false) || !isSafeBodyPath(tombstone, true) ||
        !QFile::rename(path, tombstone)) {
        return false;
    }

    QVector<NotepadNote> updated = m_notes;
    updated.remove(pos);
    if (!saveIndex(updated)) {
        QFile::rename(tombstone, path);
        return false;
    }

    m_notes = updated;
    // The index is now authoritative. If cleanup fails, startup recovery removes
    // the unreferenced tombstone without making the deleted note reappear.
    QFile::remove(tombstone);
    return true;
}

bool NotepadStore::rename(const QString &id, const QString &title) {
    if (!m_available || !isValidNoteId(id))
        return false;

    QLockFile lock(lockPath());
    if (!lock.tryLock(5000) || !synchronizeFromDisk())
        return false;
    const int pos = indexOf(id);
    if (pos < 0)
        return false;

    QVector<NotepadNote> updated = m_notes;
    updated[pos].title = title;
    if (!saveIndex(updated))
        return false;
    m_notes = updated;
    return true;
}

bool NotepadStore::setOrder(const QStringList &idsInOrder) {
    if (!m_available)
        return false;

    QLockFile lock(lockPath());
    if (!lock.tryLock(5000) || !synchronizeFromDisk())
        return false;

    QVector<NotepadNote> reordered;
    reordered.reserve(m_notes.size());
    QSet<QString> appended;

    // First, the ids the caller listed, in the requested order. Ignore duplicate
    // or stale ids so an invalid request cannot duplicate an index entry.
    for (const QString &id : idsInOrder) {
        const int pos = indexOf(id);
        if (pos >= 0 && !appended.contains(id)) {
            reordered.append(m_notes.at(pos));
            appended.insert(id);
        }
    }
    // Then any notes the list omitted, keeping their prior relative order, so a
    // stale id list can never silently drop a note.
    for (const NotepadNote &note : m_notes) {
        if (!appended.contains(note.id))
            reordered.append(note);
    }

    if (!saveIndex(reordered))
        return false;
    m_notes = reordered;
    return true;
}

QVector<QString> NotepadStore::search(const QString &query) const {
    QVector<QString> hits;
    if (query.isEmpty())
        return hits;

    for (const NotepadNote &note : m_notes) {
        if (note.title.contains(query, Qt::CaseInsensitive) ||
            load(note.id).contains(query, Qt::CaseInsensitive)) {
            hits.append(note.id);
        }
    }
    return hits;
}
