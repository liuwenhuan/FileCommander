#pragma once
#include <QPair>
#include <QString>
#include <QVector>

namespace wsd {
// 组播探测 WS-Discovery 设备,收集 timeoutMs 毫秒内的应答。返回 (name, address) 列表:
// name 可能为空(WSD 的 ProbeMatch 通常只给 IP,不给友好名),address 是主机 IP。
// 按 address 去重。纯阻塞、无 GUI、失败静默返回已收集到的(可能为空)。
QVector<QPair<QString, QString>> probe(int timeoutMs);
}
