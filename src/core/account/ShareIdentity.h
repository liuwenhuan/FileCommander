#pragma once

#include <QByteArray>
#include <QString>

// This device's TLS identity for device-to-device transfer: a self-signed
// EC (prime256v1) certificate plus the pin that identifies it.
//
// There is no CA anywhere in this design and there does not need to be. The
// accessing side gets the pin out of band (through the account server, next to
// the peer's address and port) and hands it to curl as CURLOPT_PINNEDPUBLICKEY,
// which is enforced even with peer verification off -- a substituted
// certificate fails with CURLE_SSL_PINNEDPUBKEYNOTMATCH. So the pin *is* the
// verification, and a name that would never match a certificate anyway (a LAN
// address, or 127.0.0.1 at the end of the relay tunnel) costs nothing.
//
// Qt5 cannot generate keys, hence the OpenSSL C API here; the PEM it produces
// goes straight into QSslCertificate/QSslKey on the serving side.
//
// ponytail: the account server mediates the pin, so it could actively MITM a
// transfer by substituting its own -- confidentiality against a passive relay
// operator, not against the account server itself. The upgrade is a persistent
// device keypair in libsecret, a client-side TOFU cache of each peer's pin, and
// comparable safety numbers the two users can read to each other; do it when
// the account server stops being run by the same people as the client.
namespace ShareIdentity {

struct Identity {
    QByteArray certPem;
    QByteArray keyPem;
    // "sha256//<base64 of SHA-256 over the DER SubjectPublicKeyInfo>" -- exactly
    // curl's --pinnedpubkey format, so it travels as-is to the peer.
    QString pin;

    bool isValid() const { return !certPem.isEmpty() && !keyPem.isEmpty() && !pin.isEmpty(); }
};

// This install's identity, generated on first use and then read back from
// <configDir>/share-identity.{pem,key} so the pin is stable across restarts.
// Cached after the first call. Returns an invalid Identity if it can neither
// be generated nor stored.
Identity local();

// A fresh identity, not written anywhere. Used by local() and by tests.
Identity generate();

// The pin for a certificate someone else issued.
QString pinForCertificate(const QByteArray &certPem);

} // namespace ShareIdentity
