#include "CredentialStore.h"

#undef signals
#undef slots
#include <libsecret/secret.h>

namespace {
const SecretSchema *passwordSchema() {
    static const SecretSchema schema = {
        "org.FileCommander.Connection",
        SECRET_SCHEMA_NONE,
        {{"id", SECRET_SCHEMA_ATTRIBUTE_STRING},
         {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}},
        0, 0, 0, 0, 0, 0, 0, 0,
    };
    return &schema;
}

PlatformResult failure(GError *error, const QString &action) {
    const QString message =
        error ? QString::fromUtf8(error->message)
              : QStringLiteral("Unknown libsecret failure.");
    const qint64 code = error ? error->code : 0;
    if (error)
        g_error_free(error);
    return PlatformResult::failure(
        PlatformError::NativeFailure,
        QStringLiteral("%1: %2").arg(action, message), code);
}
}

PlatformResult CredentialStore::save(const QString &id, const QString &secret) {
    if (id.isEmpty())
        return PlatformResult::failure(PlatformError::InvalidPath,
                                       QStringLiteral("Credential id is empty."));
    GError *error = nullptr;
    const QString label = QStringLiteral("FileCommander connection %1").arg(id);
    const bool ok = secret_password_store_sync(
        passwordSchema(), SECRET_COLLECTION_DEFAULT, label.toUtf8().constData(),
        secret.toUtf8().constData(), nullptr, &error, "id",
        id.toUtf8().constData(), nullptr);
    return ok && !error ? PlatformResult::success()
                        : failure(error, QStringLiteral("Secret store"));
}

PlatformResult CredentialStore::load(const QString &id, QString *secret) {
    if (secret)
        secret->clear();
    if (id.isEmpty())
        return PlatformResult::failure(PlatformError::InvalidPath,
                                       QStringLiteral("Credential id is empty."));
    GError *error = nullptr;
    gchar *value = secret_password_lookup_sync(passwordSchema(), nullptr, &error, "id",
                                               id.toUtf8().constData(), nullptr);
    if (error)
        return failure(error, QStringLiteral("Secret lookup"));
    if (!value)
        return PlatformResult::failure(PlatformError::NotFound,
                                       QStringLiteral("Credential was not found."));
    if (secret)
        *secret = QString::fromUtf8(value);
    secret_password_free(value);
    return PlatformResult::success();
}

PlatformResult CredentialStore::remove(const QString &id) {
    if (id.isEmpty())
        return PlatformResult::failure(PlatformError::InvalidPath,
                                       QStringLiteral("Credential id is empty."));
    GError *error = nullptr;
    secret_password_clear_sync(passwordSchema(), nullptr, &error, "id",
                               id.toUtf8().constData(), nullptr);
    return error ? failure(error, QStringLiteral("Secret removal"))
                 : PlatformResult::success();
}
