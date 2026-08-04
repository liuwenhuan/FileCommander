#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QSignalSpy>

#include "FileOperations.h"
#include "privilege/PrivilegeBroker.h"

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <cerrno>
#endif

namespace {

QString writeFile(const QString &dir, const QString &name, const QByteArray &content = "data") {
    const QString path = QDir(dir).filePath(name);
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.write(content);
    file.close();
    return path;
}

} // namespace

TEST(FileOperationsTest, CopyPathsCopiesFileWithoutRemovingSource) {
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    const QString source = writeFile(srcDir.path(), "a.txt");

    FileOperations ops;
    QString err;
    ASSERT_TRUE(ops.copyPaths({source}, dstDir.path(), nullptr, &err)) << err.toStdString();

    EXPECT_TRUE(QFile::exists(source));
    EXPECT_TRUE(QFile::exists(QDir(dstDir.path()).filePath("a.txt")));
}

TEST(FileOperationsTest, MovePathsRemovesSource) {
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    const QString source = writeFile(srcDir.path(), "b.txt");

    FileOperations ops;
    QString err;
    ASSERT_TRUE(ops.movePaths({source}, dstDir.path(), nullptr, &err)) << err.toStdString();

    EXPECT_FALSE(QFile::exists(source));
    EXPECT_TRUE(QFile::exists(QDir(dstDir.path()).filePath("b.txt")));
}

TEST(FileOperationsTest, FailedEmptyDirectoryMoveKeepsSource) {
    QTemporaryDir sourceParent;
    QTemporaryDir destinationParent;
    ASSERT_TRUE(sourceParent.isValid() && destinationParent.isValid());
    const QString source = sourceParent.filePath("empty");
    ASSERT_TRUE(QDir().mkpath(source));
    const QString destinationFile = writeFile(destinationParent.path(), "not-a-directory");

    FileOperations ops;
    QString error;
    EXPECT_FALSE(ops.movePaths({source}, destinationFile, nullptr, &error));
    EXPECT_TRUE(QFileInfo(source).isDir());
    EXPECT_FALSE(QFileInfo(QDir(destinationFile).filePath("empty")).exists());
}

TEST(FileOperationsTest, ConflictResolverSkipLeavesDestinationUntouched) {
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    const QString source = writeFile(srcDir.path(), "c.txt", "new");
    writeFile(dstDir.path(), "c.txt", "original");

    FileOperations ops;
    ConflictResolver resolver = [](const FileConflict &) {
        return ErrorAction::Skip;
    };
    QString err;
    ASSERT_FALSE(ops.copyPaths({source}, dstDir.path(), resolver, &err));

    QFile dest(QDir(dstDir.path()).filePath("c.txt"));
    dest.open(QIODevice::ReadOnly);
    EXPECT_EQ(dest.readAll(), QByteArray("original"));
}

TEST(FileOperationsTest, ConflictResolverOverwriteReplacesDestination) {
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    const QString source = writeFile(srcDir.path(), "d.txt", "new");
    writeFile(dstDir.path(), "d.txt", "original");

    FileOperations ops;
    ConflictResolver resolver = [](const FileConflict &) {
        return ErrorAction::Overwrite;
    };
    QString err;
    ASSERT_TRUE(ops.copyPaths({source}, dstDir.path(), resolver, &err));

    QFile dest(QDir(dstDir.path()).filePath("d.txt"));
    dest.open(QIODevice::ReadOnly);
    EXPECT_EQ(dest.readAll(), QByteArray("new"));
}

#ifdef Q_OS_WIN
TEST(FileOperationsTest, FailedOverwriteKeepsExistingDestination) {
    QTemporaryDir sourceDir;
    QTemporaryDir destinationDir;
    ASSERT_TRUE(sourceDir.isValid() && destinationDir.isValid());
    const QString source = writeFile(sourceDir.path(), "locked.txt", "new");
    const QString destination = writeFile(destinationDir.path(), "locked.txt", "original");

    HANDLE lock = CreateFileW(reinterpret_cast<LPCWSTR>(source.utf16()), GENERIC_READ, 0,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(lock, INVALID_HANDLE_VALUE);

    FileOperations ops;
    QString error;
    const bool copied = ops.copyPaths(
        {source}, destinationDir.path(),
        [](const FileConflict &) { return ErrorAction::Overwrite; }, &error);
    CloseHandle(lock);

    EXPECT_FALSE(copied);
    QFile existing(destination);
    ASSERT_TRUE(existing.open(QIODevice::ReadOnly));
    EXPECT_EQ(existing.readAll(), QByteArray("original"));
}

TEST(FileOperationsTest, FailedSourceRemovalReportsMoveFailureAndKeepsBothCopies) {
    QTemporaryDir sourceDir;
    QTemporaryDir destinationDir;
    ASSERT_TRUE(sourceDir.isValid() && destinationDir.isValid());
    const QString source = writeFile(sourceDir.path(), "locked-move.txt", "payload");
    const QString destination = destinationDir.filePath("locked-move.txt");

    HANDLE lock = CreateFileW(reinterpret_cast<LPCWSTR>(source.utf16()), GENERIC_READ,
                              FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(lock, INVALID_HANDLE_VALUE);

    FileOperations ops;
    OperationError observed;
    ops.setErrorResolver([&](const OperationError &error) {
        observed = error;
        return ErrorAction::Skip;
    });
    QString error;
    const bool moved = ops.movePaths({source}, destinationDir.path(), nullptr, &error);
    CloseHandle(lock);

    EXPECT_FALSE(moved);
    EXPECT_TRUE(QFile::exists(source));
    EXPECT_TRUE(QFile::exists(destination));
    EXPECT_EQ(observed.operation, OperationType::Delete);
}
#endif

TEST(FileOperationsTest, FailedElevatedAttemptIsReclassifiedAndCannotElevateAgain) {
    QTemporaryDir sourceDir;
    QTemporaryDir destinationDir;
    ASSERT_TRUE(sourceDir.isValid() && destinationDir.isValid());
    const QString source = sourceDir.filePath("missing.txt");

    FileOperations ops;
    ops.setNativeErrorOverrideForTesting(
        [](OperationType, const QString &, const QString &, qint64) {
#ifdef Q_OS_WIN
            return qint64(ERROR_ACCESS_DENIED);
#else
            return qint64(EACCES);
#endif
        });
    int prompts = 0;
    ops.setErrorResolver([&](const OperationError &error) {
        ++prompts;
        if (prompts == 1) {
            EXPECT_TRUE(error.elevatable);
            return ErrorAction::Elevate;
        }
        EXPECT_EQ(error.category, OperationErrorCategory::DiskFull);
        EXPECT_FALSE(error.elevatable);
        return ErrorAction::Skip;
    });
    ops.setPrivilegeExecutor([](const PrivilegedOperationRequest &) {
#ifdef Q_OS_WIN
        return PrivilegeResult{PrivilegeStatus::Failed, ERROR_DISK_FULL, "disk full"};
#else
        return PrivilegeResult{PrivilegeStatus::Failed, ENOSPC, "disk full"};
#endif
    });

    QString error;
    EXPECT_FALSE(ops.copyPaths({source}, destinationDir.path(), nullptr, &error));
    EXPECT_EQ(prompts, 2);
}

TEST(FileOperationsTest, CopyPathsOntoSelfKeepsOriginalAndMakesRenamedDuplicate) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString source = writeFile(dir.path(), "self.txt", "payload");

    FileOperations ops;
    QString err;
    // Copying into the directory the file already lives in must not destroy it.
    ASSERT_TRUE(ops.copyPaths({source}, dir.path(), nullptr, &err)) << err.toStdString();

    QFile original(source);
    ASSERT_TRUE(original.open(QIODevice::ReadOnly));
    EXPECT_EQ(original.readAll(), QByteArray("payload"));

    QFile duplicate(QDir(dir.path()).filePath("self (1).txt"));
    ASSERT_TRUE(duplicate.open(QIODevice::ReadOnly));
    EXPECT_EQ(duplicate.readAll(), QByteArray("payload"));
}

TEST(FileOperationsTest, MovePathsOntoSelfIsNoOp) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString source = writeFile(dir.path(), "stay.txt", "payload");

    FileOperations ops;
    QString err;
    ASSERT_TRUE(ops.movePaths({source}, dir.path(), nullptr, &err)) << err.toStdString();

    QFile original(source);
    ASSERT_TRUE(original.open(QIODevice::ReadOnly));
    EXPECT_EQ(original.readAll(), QByteArray("payload"));
    // No spurious renamed duplicate left behind.
    EXPECT_FALSE(QFile::exists(QDir(dir.path()).filePath("stay (1).txt")));
}

TEST(FileOperationsTest, CopyAsWritesToExplicitTargetName) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString source = writeFile(dir.path(), "orig.txt", "payload");

    FileOperations ops;
    QString err;
    const QString target = QDir(dir.path()).filePath("copy.txt");
    ASSERT_TRUE(ops.copyAs(source, target, nullptr, &err)) << err.toStdString();

    EXPECT_TRUE(QFile::exists(source));
    QFile out(target);
    ASSERT_TRUE(out.open(QIODevice::ReadOnly));
    EXPECT_EQ(out.readAll(), QByteArray("payload"));
}

TEST(FileOperationsTest, CopyReportsByteProgressToCompletion) {
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    const QString source = writeFile(srcDir.path(), "big.bin", QByteArray(4096, 'z'));

    FileOperations ops;
    QSignalSpy spy(&ops, &FileOperations::progress);
    QString err;
    ASSERT_TRUE(ops.copyPaths({source}, dstDir.path(), nullptr, &err)) << err.toStdString();

    ASSERT_FALSE(spy.isEmpty());
    const QList<QVariant> last = spy.takeLast();
    EXPECT_EQ(last.at(0).toLongLong(), 1);    // doneItems
    EXPECT_EQ(last.at(1).toLongLong(), 1);    // totalItems
    EXPECT_EQ(last.at(2).toLongLong(), 4096); // doneBytes
    EXPECT_EQ(last.at(3).toLongLong(), 4096); // totalBytes
}

TEST(FileOperationsTest, RequestCancelStopsRemainingEntries) {
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    const QString a = writeFile(srcDir.path(), "a.txt");
    const QString b = writeFile(srcDir.path(), "b.txt");
    // Pre-create a conflict so the resolver runs for the first entry, where
    // we trigger cancellation mid-batch.
    writeFile(dstDir.path(), "a.txt", "existing");

    FileOperations ops;
    ConflictResolver resolver = [&ops](const FileConflict &) {
        ops.requestCancel();
        return ErrorAction::Skip;
    };
    QString err;
    EXPECT_FALSE(ops.copyPaths({a, b}, dstDir.path(), resolver, &err));
    EXPECT_TRUE(ops.wasCancelled());
    // The second entry must never have been processed.
    EXPECT_FALSE(QFile::exists(QDir(dstDir.path()).filePath("b.txt")));
}

TEST(FileOperationsTest, ErrorResolverSkipContinuesButBatchFails) {
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    const QString missing = QDir(srcDir.path()).filePath("nope.txt"); // never created
    const QString real = writeFile(srcDir.path(), "real.txt");

    FileOperations ops;
    int calls = 0;
    ops.setErrorResolver([&calls](const QString &, const QString &) {
        ++calls;
        return ErrorAction::Skip;
    });
    QString err;
    EXPECT_FALSE(ops.copyPaths({missing, real}, dstDir.path(), nullptr, &err));
    EXPECT_GE(calls, 1); // the missing file triggered the resolver
    // The batch carried on and still copied the good file.
    EXPECT_TRUE(QFile::exists(QDir(dstDir.path()).filePath("real.txt")));
}

TEST(FileOperationsTest, ErrorResolverRetryReattemptsThenSkips) {
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    const QString missing = QDir(srcDir.path()).filePath("gone.txt");

    FileOperations ops;
    int calls = 0;
    ops.setErrorResolver([&calls](const QString &, const QString &) {
        ++calls;
        return calls < 3 ? ErrorAction::Retry : ErrorAction::Skip;
    });
    QString err;
    EXPECT_FALSE(ops.copyPaths({missing}, dstDir.path(), nullptr, &err));
    EXPECT_EQ(calls, 3); // two retries, then skip
}

TEST(FileOperationsTest, ElevationCompletesOnlyFailedItemAndContinuesBatch) {
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    const QString protectedSource = QDir(srcDir.path()).filePath("needs-admin.txt");
    const QString regularSource = writeFile(srcDir.path(), "regular.txt", "regular");

    FileOperations ops;
    int brokerCalls = 0;
    int resolverCalls = 0;
    ops.setNativeErrorOverrideForTesting(
        [protectedSource](OperationType, const QString &source, const QString &, qint64 code) {
#ifdef Q_OS_WIN
            return source == protectedSource ? qint64(ERROR_ACCESS_DENIED) : code;
#else
            return source == protectedSource ? qint64(EACCES) : code;
#endif
        });
    ops.setPrivilegeExecutor([&](const PrivilegedOperationRequest &request) {
        ++brokerCalls;
        EXPECT_EQ(request.kind, PrivilegedOperationKind::Copy);
        EXPECT_EQ(request.sourcePath, protectedSource);
        QFile elevatedTarget(request.targetPath);
        EXPECT_TRUE(elevatedTarget.open(QIODevice::WriteOnly));
        elevatedTarget.write("elevated");
        return PrivilegeResult{PrivilegeStatus::Succeeded, 0, {}};
    });
    ops.setErrorResolver([&](const OperationError &error) {
        ++resolverCalls;
        EXPECT_EQ(error.category, OperationErrorCategory::PermissionDenied);
        EXPECT_TRUE(error.elevatable);
        return ErrorAction::Elevate;
    });

    QString error;
    EXPECT_TRUE(ops.copyPaths({protectedSource, regularSource}, dstDir.path(), nullptr, &error));
    EXPECT_EQ(brokerCalls, 1);
    EXPECT_EQ(resolverCalls, 1);
    EXPECT_TRUE(QFile::exists(QDir(dstDir.path()).filePath("needs-admin.txt")));
    EXPECT_TRUE(QFile::exists(QDir(dstDir.path()).filePath("regular.txt")));
}

TEST(FileOperationsTest, CancelledElevationReturnsToDecisionAndBatchFailsWhenSkipped) {
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    const QString protectedSource = QDir(srcDir.path()).filePath("denied.txt");

    FileOperations ops;
    int resolverCalls = 0;
    ops.setNativeErrorOverrideForTesting(
        [](OperationType, const QString &, const QString &, qint64) {
#ifdef Q_OS_WIN
            return qint64(ERROR_ACCESS_DENIED);
#else
            return qint64(EACCES);
#endif
        });
    ops.setPrivilegeExecutor([](const PrivilegedOperationRequest &) {
        return PrivilegeResult{PrivilegeStatus::Cancelled, 0, QStringLiteral("UAC cancelled")};
    });
    ops.setErrorResolver([&](const OperationError &error) {
        ++resolverCalls;
        return resolverCalls == 1 ? ErrorAction::Elevate : ErrorAction::Skip;
    });

    QString error;
    EXPECT_FALSE(ops.copyPaths({protectedSource}, dstDir.path(), nullptr, &error));
    EXPECT_EQ(resolverCalls, 2);
    EXPECT_FALSE(QFile::exists(QDir(dstDir.path()).filePath("denied.txt")));
}

TEST(FileOperationsTest, PermanentDeleteCanCompleteThroughPrivilegeExecutor) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString protectedPath = dir.filePath("protected-delete.txt");

    FileOperations ops;
    int brokerCalls = 0;
    ops.setNativeErrorOverrideForTesting(
        [](OperationType, const QString &, const QString &, qint64) {
#ifdef Q_OS_WIN
            return qint64(ERROR_ACCESS_DENIED);
#else
            return qint64(EACCES);
#endif
        });
    ops.setPrivilegeExecutor([&](const PrivilegedOperationRequest &request) {
        ++brokerCalls;
        EXPECT_EQ(request.kind, PrivilegedOperationKind::DeletePermanent);
        EXPECT_EQ(request.sourcePath, protectedPath);
        EXPECT_TRUE(request.targetPath.isEmpty());
        return PrivilegeResult{PrivilegeStatus::Succeeded, 0, {}};
    });
    ops.setErrorResolver([](const OperationError &error) {
        EXPECT_EQ(error.operation, OperationType::Delete);
        EXPECT_TRUE(error.elevatable);
        return ErrorAction::Elevate;
    });

    QString error;
    EXPECT_TRUE(ops.deletePaths({protectedPath}, false, &error));
    EXPECT_EQ(brokerCalls, 1);
}

TEST(FileOperationsTest, RenameCanCompleteThroughPrivilegeExecutor) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString source = dir.filePath("protected-old.txt");
    const QString target = dir.filePath("protected-new.txt");

    FileOperations ops;
    int brokerCalls = 0;
    ops.setNativeErrorOverrideForTesting(
        [](OperationType, const QString &, const QString &, qint64) {
#ifdef Q_OS_WIN
            return qint64(ERROR_ACCESS_DENIED);
#else
            return qint64(EACCES);
#endif
        });
    ops.setPrivilegeExecutor([&](const PrivilegedOperationRequest &request) {
        ++brokerCalls;
        EXPECT_EQ(request.kind, PrivilegedOperationKind::Rename);
        EXPECT_EQ(request.sourcePath, source);
        EXPECT_EQ(request.targetPath, target);
        QFile targetFile(target);
        EXPECT_TRUE(targetFile.open(QIODevice::WriteOnly));
        return PrivilegeResult{PrivilegeStatus::Succeeded, 0, {}};
    });
    ops.setErrorResolver([](const OperationError &error) {
        EXPECT_EQ(error.operation, OperationType::Rename);
        EXPECT_TRUE(error.elevatable);
        return ErrorAction::Elevate;
    });

    QString error;
    EXPECT_TRUE(ops.renamePath(source, QStringLiteral("protected-new.txt"), &error));
    EXPECT_EQ(brokerCalls, 1);
    EXPECT_TRUE(QFile::exists(target));
}

TEST(FileOperationsTest, MakeDirectoryCanCompleteThroughPrivilegeExecutor) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString parentFile = writeFile(dir.path(), "not-a-directory", "data");
    const QString target = QDir(parentFile).filePath("child");

    FileOperations ops;
    int brokerCalls = 0;
    ops.setNativeErrorOverrideForTesting(
        [](OperationType, const QString &, const QString &, qint64) {
#ifdef Q_OS_WIN
            return qint64(ERROR_ACCESS_DENIED);
#else
            return qint64(EACCES);
#endif
        });
    ops.setPrivilegeExecutor([&](const PrivilegedOperationRequest &request) {
        ++brokerCalls;
        EXPECT_EQ(request.kind, PrivilegedOperationKind::Mkdir);
        EXPECT_TRUE(request.sourcePath.isEmpty());
        EXPECT_EQ(request.targetPath, target);
        return PrivilegeResult{PrivilegeStatus::Succeeded, 0, {}};
    });
    ops.setErrorResolver([](const OperationError &error) {
        EXPECT_EQ(error.operation, OperationType::Mkdir);
        return ErrorAction::Elevate;
    });

    QString error;
    EXPECT_TRUE(ops.makeDirectory(parentFile, QStringLiteral("child"), &error));
    EXPECT_EQ(brokerCalls, 1);
}

TEST(FileOperationsTest, SymlinkCanCompleteThroughPrivilegeExecutor) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString source = writeFile(dir.path(), "link-source.txt", "data");
    const QString invalidDestinationParent = writeFile(dir.path(), "not-a-link-directory", "data");
    const QString target = QDir(invalidDestinationParent).filePath("link-source.txt");

    FileOperations ops;
    int brokerCalls = 0;
    ops.setNativeErrorOverrideForTesting(
        [](OperationType, const QString &, const QString &, qint64) {
#ifdef Q_OS_WIN
            return qint64(ERROR_ACCESS_DENIED);
#else
            return qint64(EACCES);
#endif
        });
    ops.setPrivilegeExecutor([&](const PrivilegedOperationRequest &request) {
        ++brokerCalls;
        EXPECT_EQ(request.kind, PrivilegedOperationKind::Symlink);
        EXPECT_EQ(request.sourcePath, source);
        EXPECT_EQ(request.targetPath, target);
        return PrivilegeResult{PrivilegeStatus::Succeeded, 0, {}};
    });
    ops.setErrorResolver([](const OperationError &error) {
        EXPECT_EQ(error.operation, OperationType::Copy);
        return ErrorAction::Elevate;
    });

    QString error;
    EXPECT_TRUE(ops.createSymlinks({source}, invalidDestinationParent, &error));
    EXPECT_EQ(brokerCalls, 1);
}

TEST(FileOperationsTest, CopyAsCanCompleteThroughPrivilegeExecutor) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString source = dir.filePath("protected-source.txt");
    const QString target = dir.filePath("renamed-copy.txt");

    FileOperations ops;
    int brokerCalls = 0;
    ops.setNativeErrorOverrideForTesting(
        [](OperationType, const QString &, const QString &, qint64) {
#ifdef Q_OS_WIN
            return qint64(ERROR_ACCESS_DENIED);
#else
            return qint64(EACCES);
#endif
        });
    ops.setPrivilegeExecutor([&](const PrivilegedOperationRequest &request) {
        ++brokerCalls;
        EXPECT_EQ(request.kind, PrivilegedOperationKind::Copy);
        EXPECT_EQ(request.sourcePath, source);
        EXPECT_EQ(request.targetPath, target);
        QFile targetFile(target);
        EXPECT_TRUE(targetFile.open(QIODevice::WriteOnly));
        return PrivilegeResult{PrivilegeStatus::Succeeded, 0, {}};
    });
    ops.setErrorResolver([](const OperationError &) { return ErrorAction::Elevate; });

    QString error;
    EXPECT_TRUE(ops.copyAs(source, target, nullptr, &error));
    EXPECT_EQ(brokerCalls, 1);
    EXPECT_TRUE(QFile::exists(target));
}

TEST(FileOperationsTest, DeletePathsPermanentlyRemovesFile) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeFile(dir.path(), "e.txt");

    FileOperations ops;
    QString err;
    ASSERT_TRUE(ops.deletePaths({path}, /*toTrash=*/false, &err));
    EXPECT_FALSE(QFile::exists(path));
}

TEST(FileOperationsTest, MakeDirectoryCreatesNewFolder) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    FileOperations ops;
    QString err;
    ASSERT_TRUE(ops.makeDirectory(dir.path(), "newdir", &err)) << err.toStdString();
    EXPECT_TRUE(QDir(dir.path()).exists("newdir"));
}

TEST(FileOperationsTest, MakeDirectoryFailsIfAlreadyExists) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir(dir.path()).mkdir("existing"));

    FileOperations ops;
    QString err;
    EXPECT_FALSE(ops.makeDirectory(dir.path(), "existing", &err));
}

TEST(FileOperationsTest, RenamePathRenamesFile) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeFile(dir.path(), "old.txt");

    FileOperations ops;
    QString err;
    ASSERT_TRUE(ops.renamePath(path, "new.txt", &err)) << err.toStdString();
    EXPECT_FALSE(QFile::exists(path));
    EXPECT_TRUE(QDir(dir.path()).exists("new.txt"));
}

TEST(FileOperationsTest, CopyPathsCopiesDirectoryRecursively) {
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    QDir(srcDir.path()).mkdir("nested");
    writeFile(QDir(srcDir.path()).filePath("nested"), "inner.txt");

    FileOperations ops;
    QString err;
    ASSERT_TRUE(ops.copyPaths({QDir(srcDir.path()).filePath("nested")}, dstDir.path(), nullptr,
                               &err))
        << err.toStdString();

    EXPECT_TRUE(QFile::exists(QDir(dstDir.path()).filePath("nested/inner.txt")));
}

TEST(FileOperationsTest, CreateSymlinksCreatesWorkingLink) {
#ifdef Q_OS_WIN
    GTEST_SKIP() << "Windows symbolic links require Developer Mode or elevation";
#endif
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    const QString target = writeFile(srcDir.path(), "target.txt", "link content");

    FileOperations ops;
    QString err;
    ASSERT_TRUE(ops.createSymlinks({target}, dstDir.path(), &err)) << err.toStdString();

    const QString linkPath = QDir(dstDir.path()).filePath("target.txt");
    QFileInfo linkInfo(linkPath);
    EXPECT_TRUE(linkInfo.isSymLink());
    QFile linkFile(linkPath);
    ASSERT_TRUE(linkFile.open(QIODevice::ReadOnly));
    EXPECT_EQ(linkFile.readAll(), QByteArray("link content"));
}

TEST(FileOperationsTest, CreateSymlinksRenamesOnNameConflict) {
#ifdef Q_OS_WIN
    GTEST_SKIP() << "Windows symbolic links require Developer Mode or elevation";
#endif
    QTemporaryDir srcDir, dstDir;
    ASSERT_TRUE(srcDir.isValid() && dstDir.isValid());
    const QString target = writeFile(srcDir.path(), "dup.txt", "original");
    writeFile(dstDir.path(), "dup.txt", "unrelated existing file");

    FileOperations ops;
    QString err;
    ASSERT_TRUE(ops.createSymlinks({target}, dstDir.path(), &err)) << err.toStdString();

    // The pre-existing dup.txt must be untouched; the link gets a renamed path instead.
    QFile existing(QDir(dstDir.path()).filePath("dup.txt"));
    ASSERT_TRUE(existing.open(QIODevice::ReadOnly));
    EXPECT_EQ(existing.readAll(), QByteArray("unrelated existing file"));

    QFileInfo linkInfo(QDir(dstDir.path()).filePath("dup (1).txt"));
    EXPECT_TRUE(linkInfo.isSymLink());
}

namespace {

// A directory with a file in it, so a recursive copy has something to chew on.
QString makeTreeWithAFile(const QTemporaryDir &dir, const QString &name) {
    const QString root = dir.filePath(name);
    QDir().mkpath(root);
    QFile file(QDir(root).filePath(QStringLiteral("payload.bin")));
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QByteArray(64 * 1024, 'x'));
        file.close();
    }
    return root;
}

} // namespace

// Moving a directory into a place inside itself ran until the path outgrew the
// filesystem: the copy walked into its own output. Measured in the wild at
// 1.9 GB from a folder that was a fraction of that. Nothing below noticed,
// because every single file copy it performed was legal.
TEST(FileOperationsSelfCopy, ADirectoryCannotBeCopiedIntoItself) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString source = makeTreeWithAFile(dir, QStringLiteral("tools"));
    // Exactly the shape that was reported: a child of the source named after it.
    const QString destination = QDir(source).filePath(QStringLiteral("tools"));
    QDir().mkpath(destination);

    FileOperations ops;
    QString error;
    EXPECT_FALSE(ops.copyPaths({source}, destination, {}, &error));
    EXPECT_FALSE(error.isEmpty()) << "refused without telling the user why";
    EXPECT_TRUE(error.contains(QStringLiteral("itself"), Qt::CaseInsensitive))
        << error.toStdString();

    // ...and it refused before writing anything, rather than part way in.
    EXPECT_FALSE(QFileInfo::exists(QDir(destination).filePath(QStringLiteral("tools"))));
}

TEST(FileOperationsSelfCopy, ADirectoryCannotBeMovedIntoItself) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString source = makeTreeWithAFile(dir, QStringLiteral("tools"));
    const QString destination = QDir(source).filePath(QStringLiteral("nested"));
    QDir().mkpath(destination);

    FileOperations ops;
    QString error;
    EXPECT_FALSE(ops.movePaths({source}, destination, {}, &error));
    // The source must still be there: a refused move that ate the source would
    // be far worse than the recursion it is preventing.
    EXPECT_TRUE(QFileInfo::exists(QDir(source).filePath(QStringLiteral("payload.bin"))));
}

// The guard must not refuse the ordinary neighbours of that case.
TEST(FileOperationsSelfCopy, OrdinaryDestinationsAreStillAllowed) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString source = makeTreeWithAFile(dir, QStringLiteral("tools"));

    // A sibling directory.
    const QString sibling = dir.filePath(QStringLiteral("elsewhere"));
    QDir().mkpath(sibling);
    FileOperations ops;
    QString error;
    EXPECT_TRUE(ops.copyPaths({source}, sibling, {}, &error)) << error.toStdString();
    EXPECT_TRUE(QFileInfo::exists(
        QDir(sibling).filePath(QStringLiteral("tools/payload.bin"))));

    // A directory whose name merely starts the same way.
    const QString lookalike = dir.filePath(QStringLiteral("toolsets"));
    QDir().mkpath(lookalike);
    error.clear();
    EXPECT_TRUE(ops.copyPaths({source}, lookalike, {}, &error)) << error.toStdString();

    // And a plain file into the directory it already lives in still produces a
    // numbered copy rather than being mistaken for a self-copy.
    const QString file = QDir(source).filePath(QStringLiteral("payload.bin"));
    error.clear();
    EXPECT_TRUE(ops.copyPaths({file}, source, {}, &error)) << error.toStdString();
}
