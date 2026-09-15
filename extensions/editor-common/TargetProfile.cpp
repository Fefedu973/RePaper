#include "TargetProfile.h"
#include <QCryptographicHash>
#include <QFile>
#include <QRegularExpression>
bool RePaperNative::matchesRunningXochitl() {
    QFile machine("/sys/devices/soc0/machine"),release("/etc/os-release"),executable("/proc/self/exe");
    if(QString::fromLatin1(qVersion())!=QtVersion||!machine.open(QIODevice::ReadOnly)||!machine.readAll().contains("Ferrari")
       ||!release.open(QIODevice::ReadOnly)||release.size()>65536)return false;
    const auto match=QRegularExpression("IMG_VERSION=\"([^\"]+)\"").match(QString::fromUtf8(release.readAll()));
    if(match.captured(1)!=Firmware||!executable.open(QIODevice::ReadOnly)||executable.size()>64*1024*1024)return false;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return hash.addData(&executable)&&QString::fromLatin1(hash.result().toHex())==XochitlSha256;
}
