#include "ShareIdentity.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include "Settings.h"

namespace {

// Ten years. The pin, not the expiry, is what decides whether a peer is
// trusted, and a certificate that expired mid-transfer would only ever be a
// self-inflicted outage.
constexpr long kValiditySeconds = 10L * 365 * 24 * 3600;

QByteArray bioToByteArray(BIO *bio) {
    char *data = nullptr;
    const long size = BIO_get_mem_data(bio, &data);
    return size > 0 ? QByteArray(data, int(size)) : QByteArray();
}

// EVP_EC_gen() would be one line, but it is OpenSSL 3.0 only and this builds
// against whatever the distribution ships.
EVP_PKEY *generateEcKey() {
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    if (!ctx)
        return nullptr;
    EVP_PKEY *key = nullptr;
    if (EVP_PKEY_keygen_init(ctx) == 1 &&
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx, NID_X9_62_prime256v1) == 1)
        EVP_PKEY_keygen(ctx, &key);
    EVP_PKEY_CTX_free(ctx);
    return key;
}

QString pinForX509(X509 *cert) {
    X509_PUBKEY *pubkey = X509_get_X509_PUBKEY(cert);
    if (!pubkey)
        return QString();
    unsigned char *der = nullptr;
    const int length = i2d_X509_PUBKEY(pubkey, &der);
    if (length <= 0 || !der)
        return QString();
    const QByteArray spki(reinterpret_cast<const char *>(der), length);
    OPENSSL_free(der);
    const QByteArray digest = QCryptographicHash::hash(spki, QCryptographicHash::Sha256);
    return QStringLiteral("sha256//") + QString::fromLatin1(digest.toBase64());
}

QByteArray readFile(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
    return file.readAll();
}

bool writeFile(const QString &path, const QByteArray &data, QFile::Permissions permissions) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    if (file.write(data) != data.size())
        return false;
    file.close();
    return file.setPermissions(permissions);
}

} // namespace

namespace ShareIdentity {

Identity generate() {
    Identity identity;
    EVP_PKEY *key = generateEcKey();
    if (!key)
        return identity;

    X509 *cert = X509_new();
    if (!cert) {
        EVP_PKEY_free(key);
        return identity;
    }

    X509_set_version(cert, 2); // v3
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
    X509_gmtime_adj(X509_getm_notBefore(cert), -3600); // tolerate a skewed clock
    X509_gmtime_adj(X509_getm_notAfter(cert), kValiditySeconds);
    X509_set_pubkey(cert, key);

    // Self-signed and never checked by name, so the subject is a label for
    // whoever inspects it and nothing more.
    X509_NAME *name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char *>("FileCommander device"), -1,
                               -1, 0);
    X509_set_issuer_name(cert, name);

    if (X509_sign(cert, key, EVP_sha256()) > 0) {
        BIO *certBio = BIO_new(BIO_s_mem());
        BIO *keyBio = BIO_new(BIO_s_mem());
        if (certBio && keyBio && PEM_write_bio_X509(certBio, cert) == 1 &&
            PEM_write_bio_PrivateKey(keyBio, key, nullptr, nullptr, 0, nullptr, nullptr) == 1) {
            identity.certPem = bioToByteArray(certBio);
            identity.keyPem = bioToByteArray(keyBio);
            identity.pin = pinForX509(cert);
        }
        BIO_free(certBio);
        BIO_free(keyBio);
    }

    X509_free(cert);
    EVP_PKEY_free(key);
    if (!identity.isValid())
        return Identity();
    return identity;
}

QString pinForCertificate(const QByteArray &certPem) {
    BIO *bio = BIO_new_mem_buf(certPem.constData(), certPem.size());
    if (!bio)
        return QString();
    X509 *cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!cert)
        return QString();
    const QString pin = pinForX509(cert);
    X509_free(cert);
    return pin;
}

Identity local() {
    static Identity cached = [] {
        const QString dir = Settings::configDir();
        if (dir.isEmpty())
            return generate(); // no config dir: still encrypt, just not stably
        const QString certPath = QDir(dir).filePath(QStringLiteral("share-identity.pem"));
        const QString keyPath = QDir(dir).filePath(QStringLiteral("share-identity.key"));

        Identity identity;
        identity.certPem = readFile(certPath);
        identity.keyPem = readFile(keyPath);
        identity.pin = pinForCertificate(identity.certPem);
        if (identity.isValid())
            return identity;

        identity = generate();
        if (!identity.isValid())
            return Identity();
        // The key is the whole secret: nobody but this user gets to read it.
        if (!writeFile(keyPath, identity.keyPem, QFile::ReadOwner | QFile::WriteOwner) ||
            !writeFile(certPath, identity.certPem,
                       QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup |
                           QFile::ReadOther)) {
            // Unwritable config dir. The identity still works for this run; the
            // pin simply changes on the next one.
            QFile::remove(keyPath);
            QFile::remove(certPath);
        }
        return identity;
    }();
    return cached;
}

} // namespace ShareIdentity
