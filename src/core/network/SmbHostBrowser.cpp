#include "SmbHostBrowser.h"

#include <QPointer>
#include <QUrl>
#include <QtConcurrent>

#include <libsmbclient.h>

namespace {

// Anonymous auth for browsing: hand libsmbclient empty credentials so it queries
// the network neighbourhood as guest. Browsing the smb:// root and workgroups
// needs no login, so there is nothing to leak here (unlike SmbProvider, which
// carries real credentials).
void anonymousAuthCallback(SMBCCTX * /*ctx*/, const char * /*srv*/, const char * /*shr*/,
                           char *wg, int wglen, char *un, int unlen, char *pw, int pwlen) {
    if (wg && wglen > 0)
        wg[0] = '\0';
    if (un && unlen > 0)
        un[0] = '\0';
    if (pw && pwlen > 0)
        pw[0] = '\0';
}

// Lists the SMB servers directly under an smb:// URL (the root or a workgroup).
// Returns their names; workgroup names encountered under the root are appended
// to `workgroupsOut` so the caller can descend into them.
QVector<SmbHost> serversUnder(SMBCCTX *ctx, const QByteArray &url,
                              QStringList *workgroupsOut) {
    QVector<SmbHost> hosts;

    smbc_opendir_fn opendirFn = smbc_getFunctionOpendir(ctx);
    smbc_readdir_fn readdirFn = smbc_getFunctionReaddir(ctx);
    smbc_closedir_fn closedirFn = smbc_getFunctionClosedir(ctx);

    SMBCFILE *dir = opendirFn(ctx, url.constData());
    if (!dir)
        return hosts;

    struct smbc_dirent *de = nullptr;
    while ((de = readdirFn(ctx, dir)) != nullptr) {
        const QString name = QString::fromUtf8(de->name);
        if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral(".."))
            continue;

        if (de->smbc_type == SMBC_SERVER) {
            hosts.append(SmbHost{name, QString()});
        } else if (de->smbc_type == SMBC_WORKGROUP && workgroupsOut) {
            workgroupsOut->append(name);
        }
    }

    closedirFn(ctx, dir);
    return hosts;
}

} // namespace

SmbHostBrowser::SmbHostBrowser(QObject *parent) : QObject(parent) {}

void SmbHostBrowser::startDiscovery() {
    if (m_running)
        return; // ignore overlapping requests
    m_running = true;

    // A QPointer lets the worker's cross-thread callbacks no-op if this object is
    // destroyed before the scan finishes.
    QPointer<SmbHostBrowser> self(this);

    QtConcurrent::run([self]() {
        // Marshals a batch of hosts back to the object's thread. If the browser
        // has already been destroyed there is nothing to deliver to, so skip.
        auto emitHosts = [self](const QVector<SmbHost> &hosts) {
            if (hosts.isEmpty() || !self)
                return;
            QMetaObject::invokeMethod(
                self.data(),
                [self, hosts]() {
                    if (self)
                        emit self->hostsDiscovered(hosts);
                },
                Qt::QueuedConnection);
        };

        SMBCCTX *ctx = smbc_new_context();
        if (ctx) {
            smbc_setFunctionAuthDataWithContext(ctx, anonymousAuthCallback);
            smbc_setOptionUseKerberos(ctx, 0);
            smbc_setOptionFallbackAfterKerberos(ctx, 1);
            smbc_setOptionNoAutoAnonymousLogin(ctx, 0);
            smbc_setDebug(ctx, 0);

            if (smbc_init_context(ctx)) {
                // Root browse: collect any servers listed directly plus the set
                // of workgroups to descend into.
                QStringList workgroups;
                const QVector<SmbHost> rootHosts =
                    serversUnder(ctx, QByteArrayLiteral("smb://"), &workgroups);
                emitHosts(rootHosts);

                for (const QString &wg : workgroups) {
                    const QByteArray wgUrl =
                        QByteArrayLiteral("smb://") +
                        QUrl::toPercentEncoding(wg);
                    const QVector<SmbHost> wgHosts = serversUnder(ctx, wgUrl, nullptr);
                    emitHosts(wgHosts);
                }
            }

            // shutdown_ctx=1 closes any open dirs and frees the context.
            smbc_free_context(ctx, 1);
        }

        // Always report completion and clear the running flag, on the object's
        // thread, even when the context could not be created.
        if (self) {
            QMetaObject::invokeMethod(
                self.data(),
                [self]() {
                    if (self) {
                        self->m_running = false;
                        emit self->discoveryFinished();
                    }
                },
                Qt::QueuedConnection);
        }
    });
}
