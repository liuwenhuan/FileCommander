#include "ArchiveNames.h"

#include <archive.h>
#include <archive_entry.h>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace fc {

void applyHeaderCharset(struct archive *a) {
    if (!a)
        return;
#ifdef Q_OS_WIN
    // The system ANSI code page is what a zip made on this kind of machine used
    // -- 936 (GBK) on a Chinese Windows, 932 on a Japanese one. That is also the
    // assumption Explorer and 7-Zip make, so a file that opens for the user
    // elsewhere opens here.
    const QByteArray option = "hdrcharset=CP" + QByteArray::number(int(GetACP()));
    // Ignored on formats that define their own charset; a failure here is not
    // worth refusing the archive over, it just leaves the previous assumption.
    archive_read_set_options(a, option.constData());
#else
    // Linux desktops are UTF-8, and libarchive already assumes the locale
    // charset there. Forcing anything would be the wrong guess more often than
    // the right one.
    Q_UNUSED(a);
#endif
}

QString entryPathname(struct archive_entry *entry) {
    if (!entry)
        return {};
    if (const wchar_t *wide = archive_entry_pathname_w(entry))
        return QString::fromWCharArray(wide);
    // No wide name: libarchive could not convert it. The multi-byte name is
    // defined to be in the local encoding, so decode it as that -- fromUtf8()
    // here is what turned an unreadable name into an unrecoverable one.
    if (const char *narrow = archive_entry_pathname(entry))
        return QString::fromLocal8Bit(narrow);
    return {};
}

void setEntryPathname(struct archive_entry *entry, const QString &path) {
    if (!entry)
        return;
#ifdef Q_OS_WIN
    const std::wstring wide = path.toStdWString();
    archive_entry_copy_pathname_w(entry, wide.c_str());
#else
    archive_entry_set_pathname(entry, path.toLocal8Bit().constData());
#endif
}

} // namespace fc
