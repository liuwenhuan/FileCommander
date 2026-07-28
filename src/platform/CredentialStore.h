#pragma once

#include "PlatformResult.h"

#include <QString>

class CredentialStore {
public:
    static PlatformResult save(const QString &id, const QString &secret);
    static PlatformResult load(const QString &id, QString *secret);
    static PlatformResult remove(const QString &id);
};
