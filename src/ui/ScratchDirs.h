#pragma once

#include <QString>

#include <memory>

class QTemporaryDir;

// The three temporary directories the window works out of, created on first use.
//
// They were three near-identical lazy-create functions and three raw
// QTemporaryDir* members that nothing ever deleted -- the process exiting was
// the cleanup. The three differed in exactly two ways: the name in the path,
// and whether the directory outlives the program. Everything else was copied.
//
// The reasoning behind that one real difference is the part worth keeping, so
// it is recorded on each accessor rather than left in whichever copy the reader
// happens to open.
class ScratchDirs {
public:
    ScratchDirs();
    ~ScratchDirs();
    ScratchDirs(const ScratchDirs &) = delete;
    ScratchDirs &operator=(const ScratchDirs &) = delete;

    // Rendered previews. Nothing outside this process reads them and they are
    // small, so they go when the program does.
    QString preview();

    // Copies of remote files handed to other applications.
    //
    // Deliberately kept past shutdown: an application we launched may still be
    // reading (or about to read) a copy, and pulling the file out from under a
    // document the user is looking at is worse than leaving bytes in /tmp,
    // which the system clears anyway. This is also why it is never deleted
    // while the window lives -- we cannot know when a launched application is
    // done with its file.
    QString openWith();

    // Copies of remote archives being browsed.
    //
    // Unlike the open-with copies these are NOT kept past shutdown. Nothing
    // outside this process ever sees them (the archive is browsed in-app), each
    // one is already deleted when its browse ends, and an archive is exactly
    // the kind of file that can be several gigabytes -- so the auto-remove
    // default stands as the backstop for a copy whose browse a crash cut short.
    QString archive();

    // Sweeps up the archive copies now rather than at destruction, which is
    // what closeEvent does: any browse the user never stepped out of leaves a
    // copy behind, and unlike the open-with ones these are ours to remove.
    // A later archive() simply makes a fresh directory.
    void discardArchive();

private:
    // Empty when the directory could not be created, which every caller already
    // handles: a scratch path is a convenience, never something to fail over.
    QString ensure(std::unique_ptr<QTemporaryDir> &slot, const char *nameFragment,
                   bool removeOnExit);

    std::unique_ptr<QTemporaryDir> m_preview;
    std::unique_ptr<QTemporaryDir> m_openWith;
    std::unique_ptr<QTemporaryDir> m_archive;
};
