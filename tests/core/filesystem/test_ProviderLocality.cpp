#include <gtest/gtest.h>

#include "FileProvider.h"
#include "LocalFileProvider.h"
#if FILECOMMANDER_HAS_NETWORK
#include "CurlFtpProvider.h"
#include "CurlWebDavProvider.h"
#include "SftpProvider.h"
#endif
#if FILECOMMANDER_HAS_LINUX_INTEGRATION
#include "SmbProvider.h"
#endif

// FileProvider::isLocalFilesystem() decides whether a path may be handed
// straight to QFile/QDir. Secure wipe relies on it: on a backend that answers
// false, the wipe dialog's QFile-based overwrite would shred whatever LOCAL
// file happened to share the remote path's name. Getting this answer wrong is
// silent data loss, so pin it down per backend.
//
// ArchiveProvider is covered in tests/archive (it lives behind libarchive and
// isn't linked into core_tests).

TEST(ProviderLocality, LocalBackendIsLocal) {
    EXPECT_TRUE(LocalFileProvider::instance()->isLocalFilesystem());
}

TEST(ProviderLocality, NetworkBackendsAreNotLocal) {
    // Constructed but never connected: locality is a property of the backend's
    // path namespace, not of the current session, so it must hold either way.
#if FILECOMMANDER_HAS_NETWORK
    SftpProvider sftp;
    EXPECT_FALSE(sftp.isLocalFilesystem());

    CurlFtpProvider ftp;
    EXPECT_FALSE(ftp.isLocalFilesystem());

    CurlWebDavProvider dav;
    EXPECT_FALSE(dav.isLocalFilesystem());
#endif
#if FILECOMMANDER_HAS_LINUX_INTEGRATION
    SmbProvider smb;
    EXPECT_FALSE(smb.isLocalFilesystem());
#endif
}

namespace {

// Stands in for a backend added later that hasn't thought about this question.
class MinimalProvider : public FileProvider {
public:
    QVector<FileInfo> list(const QString &, bool) const override { return {}; }
    bool isDir(const QString &) const override { return false; }
    QString cleanPath(const QString &p) const override { return p; }
    QString parentPath(const QString &) const override { return {}; }
    bool exists(const QString &) const override { return false; }
    RenameResult rename(const QString &, const QString &, QString *) override {
        return RenameResult::Failed;
    }
};

} // namespace

TEST(ProviderLocality, DefaultIsNotLocal) {
    // The default must be the conservative answer: a new backend is refused by
    // the callers that need real local paths until it deliberately opts in.
    MinimalProvider p;
    EXPECT_FALSE(p.isLocalFilesystem());
}
