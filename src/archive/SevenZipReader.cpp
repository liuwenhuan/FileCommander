#include "SevenZipReader.h"

#include <QFile>
#include <QVector>

#include <cstdlib>

// Vendored public-domain LZMA SDK (C). Its headers self-guard for C++.
#include "lzma_sdk/7z.h"
#include "lzma_sdk/7zCrc.h"
#include "lzma_sdk/7zFile.h"
#include "lzma_sdk/7zTypes.h"
#include "lzma_sdk/sevenz_aes.h"

namespace {

// Installs the password (UTF-16LE, as 7z expects) for the current thread's
// decode calls, and resets the "AES coder seen" flag so we can classify a
// later failure as NeedPassword / WrongPassword. An empty password means none.
void applyPassword(const QString &password) {
    Sevenz_ResetAesFlag();
    if (password.isEmpty()) {
        Sevenz_ClearPassword();
        return;
    }
    QVector<Byte> le;
    le.reserve(password.size() * 2);
    const ushort *u = password.utf16();
    for (int i = 0; i < password.size(); ++i) {
        le.push_back(Byte(u[i] & 0xFF));
        le.push_back(Byte((u[i] >> 8) & 0xFF));
    }
    Sevenz_SetPassword(le.constData(), size_t(le.size()));
}

// Maps a decode failure to an encryption status when an AES coder was involved
// (else returns Unsupported/Error via the caller). A wrong or missing password
// makes the encrypted stream decode to garbage, so both surface here.
bool mapEncryptedFailure(const QString &password, SevenZipReader::Status *out) {
    if (!Sevenz_AesSeen())
        return false;
    *out = password.isEmpty() ? SevenZipReader::Status::NeedPassword
                              : SevenZipReader::Status::WrongPassword;
    return true;
}

// Our own allocator so we don't depend on the SDK's optional g_Alloc symbol.
void *sz_alloc(ISzAllocPtr, size_t size) { return std::malloc(size); }
void sz_free(ISzAllocPtr, void *p) { std::free(p); }
const ISzAlloc kAlloc = {sz_alloc, sz_free};

// CrcGenerateTable() must run once before any SzArEx_Open.
struct CrcInit {
    CrcInit() { CrcGenerateTable(); }
};
const CrcInit g_crcInit;

QString nameToQString(const UInt16 *utf16) {
    QString s = QString::fromUtf16(reinterpret_cast<const char16_t *>(utf16));
    return s.replace(QLatin1Char('\\'), QLatin1Char('/'));
}

// Windows FILETIME (100 ns ticks since 1601-01-01) -> QDateTime.
QDateTime fileTimeToQDateTime(const CNtfsFileTime &ft) {
    const UInt64 t = (UInt64(ft.High) << 32) | ft.Low;
    constexpr UInt64 kEpochDiff = 116444736000000000ULL; // 1601 -> 1970
    if (t == 0 || t < kEpochDiff)
        return {};
    return QDateTime::fromSecsSinceEpoch(qint64((t - kEpochDiff) / 10000000ULL));
}

// RAII-ish holder for the file stream + look-ahead stream + parsed db.
struct Archive {
    CFileInStream stream;
    CLookToRead2 look;
    CSzArEx db;
    bool fileOpen = false;
    bool dbInit = false;

    ~Archive() {
        if (dbInit)
            SzArEx_Free(&db, &kAlloc);
        if (look.buf)
            ISzAlloc_Free(&kAlloc, look.buf);
        if (fileOpen)
            File_Close(&stream.file);
    }
};

// Opens + parses the archive header. Returns SZ_OK, or an SRes error.
SRes openArchive(const QString &path, Archive &a) {
    FileInStream_CreateVTable(&a.stream);
    LookToRead2_CreateVTable(&a.look, False);
    a.look.buf = nullptr;

    if (InFile_Open(&a.stream.file, path.toUtf8().constData()))
        return SZ_ERROR_NO_ARCHIVE;
    a.fileOpen = true;

    constexpr size_t kBuf = 1 << 18;
    a.look.buf = static_cast<Byte *>(ISzAlloc_Alloc(&kAlloc, kBuf));
    if (!a.look.buf)
        return SZ_ERROR_MEM;
    a.look.bufSize = kBuf;
    a.look.realStream = &a.stream.vt;
    LookToRead2_INIT(&a.look);

    SzArEx_Init(&a.db);
    a.dbInit = true;
    return SzArEx_Open(&a.db, &a.look.vt, &kAlloc, &kAlloc);
}

SevenZipReader::Status statusForOpen(SRes res) {
    // Reached only for non-encryption open failures (encrypted ones are caught
    // by mapEncryptedFailure via the AES-seen flag before this is consulted).
    if (res == SZ_ERROR_UNSUPPORTED)
        return SevenZipReader::Status::Unsupported;
    return SevenZipReader::Status::Error;
}

} // namespace

SevenZipReader::Status SevenZipReader::list(const QString &archivePath, const QString &password,
                                            const std::function<void(const Entry &)> &cb,
                                            std::atomic<bool> *cancel) {
    applyPassword(password);
    Archive a;
    const SRes res = openArchive(archivePath, a);
    if (res != SZ_OK) {
        // A header-encrypted archive (-mhe) fails to open until decrypted.
        Status enc;
        if (mapEncryptedFailure(password, &enc))
            return enc;
        return statusForOpen(res);
    }

    QVector<UInt16> nameBuf;
    for (UInt32 i = 0; i < a.db.NumFiles; ++i) {
        if (cancel && cancel->load())
            return Status::Error;
        Entry e;
        e.isDir = SzArEx_IsDir(&a.db, i);
        const size_t len = SzArEx_GetFileNameUtf16(&a.db, i, nullptr);
        nameBuf.resize(int(len));
        SzArEx_GetFileNameUtf16(&a.db, i, nameBuf.data());
        e.path = nameToQString(nameBuf.constData());
        e.size = e.isDir ? 0 : qint64(SzArEx_GetFileSize(&a.db, i));
        if (SzBitWithVals_Check(&a.db.MTime, i))
            e.modified = fileTimeToQDateTime(a.db.MTime.Vals[i]);
        cb(e);
    }
    return Status::Ok;
}

SevenZipReader::Status SevenZipReader::readEntry(const QString &archivePath,
                                                 const QString &password,
                                                 const QString &entryPath, const QString &destPath,
                                                 std::atomic<bool> *cancel) {
    applyPassword(password);
    Archive a;
    const SRes res = openArchive(archivePath, a);
    if (res != SZ_OK) {
        Status enc;
        if (mapEncryptedFailure(password, &enc))
            return enc;
        return statusForOpen(res);
    }

    // Locate the entry by its full path.
    UInt32 target = a.db.NumFiles;
    QVector<UInt16> nameBuf;
    for (UInt32 i = 0; i < a.db.NumFiles; ++i) {
        if (SzArEx_IsDir(&a.db, i))
            continue;
        const size_t len = SzArEx_GetFileNameUtf16(&a.db, i, nullptr);
        nameBuf.resize(int(len));
        SzArEx_GetFileNameUtf16(&a.db, i, nameBuf.data());
        if (nameToQString(nameBuf.constData()) == entryPath) {
            target = i;
            break;
        }
    }
    if (target == a.db.NumFiles)
        return Status::Error;
    if (cancel && cancel->load())
        return Status::Error;

    // SzArEx_Extract decodes the whole solid block containing the entry into
    // outBuffer (cached across calls; freed here). outBuffer must be null first.
    UInt32 blockIndex = 0xFFFFFFFF;
    Byte *outBuffer = nullptr;
    size_t outBufferSize = 0, offset = 0, outSizeProcessed = 0;
    const SRes ex = SzArEx_Extract(&a.db, &a.look.vt, target, &blockIndex, &outBuffer,
                                   &outBufferSize, &offset, &outSizeProcessed, &kAlloc, &kAlloc);

    Status result = Status::Ok;
    if (ex == SZ_OK) {
        QFile out(destPath);
        if (out.open(QIODevice::WriteOnly)) {
            out.write(reinterpret_cast<const char *>(outBuffer + offset), qint64(outSizeProcessed));
            out.close();
        } else {
            result = Status::Error;
        }
    } else {
        Status enc;
        if (mapEncryptedFailure(password, &enc))
            result = enc;
        else
            result = (ex == SZ_ERROR_UNSUPPORTED) ? Status::Unsupported : Status::Error;
    }
    if (outBuffer)
        ISzAlloc_Free(&kAlloc, outBuffer);
    return result;
}
