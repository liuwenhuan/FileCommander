#include "NotepadStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUuid>

#include "config/Settings.h"

namespace {

// Notes live under the shared config directory so every store agrees on where
// configuration lives (Settings owns that path).
QString notepadDir() {
    const QString dir = Settings::configDir() + QStringLiteral("/notepad");
    QDir().mkpath(dir);
    return dir;
}

} // namespace

NotepadStore::NotepadStore() : m_dir(notepadDir()) {
    loadIndex();
}

QString NotepadStore::indexPath() const {
    return m_dir + QStringLiteral("/index.json");
}

QString NotepadStore::filePathFor(const QString &id) const {
    return m_dir + QStringLiteral("/") + id + QStringLiteral(".txt");
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

    QFile file(indexPath());
    if (!file.exists()) {
        // Nothing saved yet: pick up any stray .txt files (e.g. after an index
        // was lost) so no note is orphaned, then persist a fresh index.
        rebuildFromDisk();
        return;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        rebuildFromDisk();
        return;
    }

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
        if (id.isEmpty())
            continue;
        NotepadNote note;
        note.id = id;
        note.title = obj.value(QStringLiteral("title")).toString();
        note.filePath = filePathFor(id);
        const int order = obj.value(QStringLiteral("order")).toInt(ordered.size());
        ordered.append({order, note});
    }

    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const QPair<int, NotepadNote> &a, const QPair<int, NotepadNote> &b) {
                         return a.first < b.first;
                     });
    for (const auto &entry : ordered)
        m_notes.append(entry.second);
}

void NotepadStore::rebuildFromDisk() {
    m_notes.clear();

    QDir dir(m_dir);
    const QStringList txtFiles =
        dir.entryList(QStringList{QStringLiteral("*.txt")}, QDir::Files, QDir::Name);
    for (const QString &fileName : txtFiles) {
        NotepadNote note;
        note.id = QFileInfo(fileName).completeBaseName();
        if (note.id.isEmpty())
            continue;
        note.title = note.id; // no title survived; fall back to the id
        note.filePath = filePathFor(note.id);
        m_notes.append(note);
    }

    saveIndex();
}

void NotepadStore::saveIndex() const {
    QJsonArray array;
    for (int i = 0; i < m_notes.size(); ++i) {
        const NotepadNote &note = m_notes.at(i);
        QJsonObject obj;
        obj.insert(QStringLiteral("id"), note.id);
        obj.insert(QStringLiteral("title"), note.title);
        obj.insert(QStringLiteral("order"), i);
        array.append(obj);
    }

    // QSaveFile writes atomically, so a crash mid-write can't leave a truncated
    // index behind (loadIndex would otherwise rebuild it).
    QSaveFile file(indexPath());
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(QJsonDocument(array).toJson(QJsonDocument::Indented));
    file.commit();
}

QVector<NotepadNote> NotepadStore::notes() const {
    return m_notes;
}

NotepadNote NotepadStore::create(const QString &title) {
    NotepadNote note;
    note.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    note.title = title;
    note.filePath = filePathFor(note.id);

    // Touch an empty body file so the note exists even before the first edit.
    QFile file(note.filePath);
    if (file.open(QIODevice::WriteOnly))
        file.close();

    m_notes.append(note);
    saveIndex();
    return note;
}

void NotepadStore::save(const QString &id, const QString &content) {
    if (indexOf(id) < 0)
        return;
    QSaveFile file(filePathFor(id));
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(content.toUtf8());
    file.commit();
}

QString NotepadStore::load(const QString &id) const {
    QFile file(filePathFor(id));
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    const QString content = QString::fromUtf8(file.readAll());
    file.close();
    return content;
}

void NotepadStore::remove(const QString &id) {
    const int pos = indexOf(id);
    if (pos < 0)
        return;
    QFile::remove(filePathFor(id));
    m_notes.remove(pos);
    saveIndex();
}

void NotepadStore::rename(const QString &id, const QString &title) {
    const int pos = indexOf(id);
    if (pos < 0)
        return;
    m_notes[pos].title = title;
    saveIndex();
}

void NotepadStore::setOrder(const QStringList &idsInOrder) {
    QVector<NotepadNote> reordered;
    reordered.reserve(m_notes.size());

    // First, the ids the caller listed, in the requested order.
    for (const QString &id : idsInOrder) {
        const int pos = indexOf(id);
        if (pos >= 0)
            reordered.append(m_notes.at(pos));
    }
    // Then any notes the list omitted, keeping their prior relative order, so a
    // stale id list can never silently drop a note.
    for (const NotepadNote &note : m_notes) {
        if (!idsInOrder.contains(note.id))
            reordered.append(note);
    }

    m_notes = reordered;
    saveIndex();
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
