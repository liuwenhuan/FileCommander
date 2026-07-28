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

    // Persists the body of an existing note. No-op for an unknown id.
    void save(const QString &id, const QString &content);

    // Reads a note's body; empty string for an unknown id or unreadable file.
    QString load(const QString &id) const;

    // Deletes a note's index entry and its body file.
    void remove(const QString &id);

    // Renames a note (title only; the id/file are unchanged).
    void rename(const QString &id, const QString &title);

    // Reorders the index to match the given id list. Ids not present are kept in
    // their prior relative order at the end, so a stale list can't drop notes.
    void setOrder(const QStringList &idsInOrder);

    // Full-text search (title + body) across every note, case-insensitive.
    // Returns the ids of matching notes in saved order.
    QVector<QString> search(const QString &query) const;

private:
    QString m_dir;                  // <configDir>/notepad
    QVector<NotepadNote> m_notes;   // in-memory index, kept in sync with index.json

    QString indexPath() const;      // <dir>/index.json
    QString filePathFor(const QString &id) const; // <dir>/<id>.txt
    void loadIndex();               // read index.json into m_notes (rebuild if broken)
    void saveIndex() const;         // write m_notes back to index.json
    void rebuildFromDisk();         // reconstruct the index from stray .txt files
    int indexOf(const QString &id) const; // position in m_notes, or -1
};
