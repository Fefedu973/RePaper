#pragma once
#include <QJsonObject>
#include <QString>

namespace paper {
struct BridgeRuntimeConfig {
    QString runtime;
    QString endpoint = "/run/paper-bridge/core.sock";
    QString lock = "/run/repaper-paper-bridge.start.lock";
    QString manager = "/usr/bin/systemd-run";
    QString unit = "repaper-paper-bridge.service";
};

// Configuration is explicit so host tests can use a private socket and a fake manager.
// The production command supplies only the fixed device socket, lock and service name.
QJsonObject ensureBridgeRuntime(const BridgeRuntimeConfig &config);
QJsonObject probeBridgeRuntime(const QString &endpoint, int timeoutMs = 400);
}
