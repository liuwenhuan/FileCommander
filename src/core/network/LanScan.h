#pragma once

#include <QPair>
#include <QString>
#include <QVector>

// Active LAN discovery for SMB hosts that do not advertise themselves over
// mDNS / WSD / NetBIOS broadcast, but do keep TCP 445 open (bare Samba boxes,
// hardened Windows servers). This is the fourth discovery path for the network
// neighborhood: a direct port sweep of the local /24 subnet.
namespace lanscan {

// Scans the local IPv4 /24 subnet(s) this machine is attached to and returns
// every host with TCP port 445 open.
//
// Each result is (name, ip):
//   - name is the host's NetBIOS name resolved via a UDP 137 node-status query
//     (e.g. "DEEPIN-PC"); empty when it cannot be determined. Callers should
//     fall back to displaying the ip when name is empty.
//   - ip is dotted-decimal IPv4.
//
// The local machine's own addresses are skipped. Results are de-duplicated by
// ip. perHostTimeoutMs bounds each connect attempt (default suggestion 400ms);
// the 445 sweep runs concurrently (non-blocking connect + select), so the whole
// call completes within a few seconds regardless of host count.
//
// This function is blocking and intended to run on a worker thread. It never
// throws and never crashes: on any failure it returns whatever it has gathered
// so far (possibly empty).
QVector<QPair<QString, QString>> scan445(int perHostTimeoutMs);

} // namespace lanscan
