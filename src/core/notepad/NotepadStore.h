#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

// One quick note: an id (a braceless UUID), a user-facing title, and the path of
// the .txt file holding its body.
struct NotepadNote {
    QString id;
    QString title;
    QString filePath;
};

// File + JSON index store for the quick-notepad panel. Bodies live one-per-file
// at <configDir>/notepad/<id>.txt; the ordered index (id/title/order) is a JSON
// array at <configDir>/notepad/index.json. Kept deliberately small and
// self-healing: a missing directory is created and a corrupt index is rebuilt
// from whatever .txt files are on disk rather than throwing.
class NotepadStore {
public:
    NotepadStore();
    explicit NotepadStore(const QString &directory);

    // All notes in their saved order.
    QVector<NotepadNote> notes() const;

    // Creates a note with the given title (an empty body file + an index entry),
    // returns the fully-populated record.
    NotepadNote create(const QString &title);

    // Persists the body of an existing note. expectedContent is the body the
    // caller originally loaded; save fails rather than overwriting a newer edit
    // committed by another process.
    bool save(const QString &id, const QString &content, const QString &expectedContent);

    // Reads a note's body; empty string for an unknown id or unreadable file.
    QString load(const QString &id) const;

    // Deletes a note's body and index entry. Returns false without changing the
    // in-memory index if the body or updated index cannot be persisted.
    bool remove(const QString &id);

    // Renames a note (title only; the id/file are unchanged).
    bool rename(const QString &id, const QString &title);

    // Reorders the index to match the given id list. Ids not present are kept in
    // their prior relative order at the end, so a stale list can't drop notes.
    bool setOrder(const QStringList &idsInOrder);

    // Whether construction loaded or initialized a usable storage directory.
    bool isAvailable() const { return m_available; }

    // Full-text search (title + body) across every note, case-insensitive.
    // Returns the ids of matching notes in saved order.
    QVector<QString> search(const QString &query) const;

private:
    QString m_dir;                  // <configDir>/notepad
    QVector<NotepadNote> m_notes;   // in-memory index, kept in sync with index.json
    bool m_available = false;

    QString indexPath() const;      // <dir>/index.json
    QString lockPath() const;       // <dir>/.index.lock
    QString filePathFor(const QString &id) const; // <dir>/<id>.txt
    QString tombstonePathFor(const QString &id) const;
    bool isSafeBodyPath(const QString &path, bool allowMissing) const;
    bool recoverPendingDeletes();
    bool recoverOrphanedBodies();
    bool synchronizeFromDisk();
    void loadIndex();               // read index.json into m_notes (rebuild if broken)
    bool saveIndex(const QVector<NotepadNote> &notes) const;
    bool saveIndex() const;         // write m_notes back to index.json
    void rebuildFromDisk();         // reconstruct the index from stray .txt files
    int indexOf(const QString &id) const; // position in m_notes, or -1
};
