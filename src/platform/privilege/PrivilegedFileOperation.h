#pragma once

#include <QByteArray>
#include <QString>

enum class PrivilegedOperationKind { Copy, Move, DeletePermanent, Mkdir, Rename, Symlink };

struct PrivilegedOperationRequest {
    int version = 1;
    PrivilegedOperationKind kind;
    QString sourcePath;
    QString targetPath;
    bool overwrite = false;
};

enum class PrivilegeStatus { Succeeded, Denied, Cancelled, Failed, InvalidRequest };

struct PrivilegeResult {
    PrivilegeStatus status;
    qint64 nativeCode;
    QString message;
};

QByteArray encodePrivilegedRequest(const PrivilegedOperationRequest &request);
PrivilegeResult decodePrivilegedRequest(const QByteArray &encoded,
                                        PrivilegedOperationRequest *request);
PrivilegeResult validatePrivilegedOperationRequest(const PrivilegedOperationRequest &request);
