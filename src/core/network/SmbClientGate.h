#pragma once

#include <QMutex>

class SmbClientGate {
public:
    static QMutex &mutex() {
        static QMutex gate;
        return gate;
    }
};
