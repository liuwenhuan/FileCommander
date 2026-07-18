#pragma once

#include <QDateTime>
#include <QSharedPointer>
#include <QString>
#include <QVector>

// One entry in an archive's directory tree. Children are held via
// QSharedPointer (not by value in a QList/QVector<ArchiveNode>) -- same
// lesson as TabState: avoid storing objects with heap-owning members
// (QString/QVector) by value in Qt containers.
struct ArchiveNode {
    QString name;     // basename only
    QString fullPath; // path within the archive, e.g. "dir/sub/file.txt"
    bool isDir = false;
    qint64 size = 0;
    QDateTime modified;
    ArchiveNode *parent = nullptr; // non-owning; tree is owned root-down via children
    QVector<QSharedPointer<ArchiveNode>> children;

    QSharedPointer<ArchiveNode> findChild(const QString &childName) const {
        for (const auto &c : children) {
            if (c->name == childName)
                return c;
        }
        return {};
    }
};
