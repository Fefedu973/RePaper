#include "NativeLineFactory.h"
#include "TargetProfile.h"
#include <QCoreApplication>
#include <QMetaType>
#include <QThread>
#include <cstdint>
#include <cstring>

#if defined(REPAPER_WITH_NATIVE_ABI) && defined(__aarch64__)
#include "rm_SceneItem.hpp"
#endif

std::shared_ptr<SceneItem> RePaperNative::createNativeLineItem(QString *error) {
    if (error) error->clear();
    const auto refused = [error](const char *reason) {
        if (error) *error = QString::fromUtf8(reason);
        return std::shared_ptr<SceneItem>{};
    };
#if defined(REPAPER_WITH_NATIVE_ABI) && defined(__aarch64__)
    if (!QCoreApplication::instance() ||
        QThread::currentThread() != QCoreApplication::instance()->thread())
        return refused("La création native doit se faire sur le fil de l’interface.");
    // /proc/self/exe is immutable for this process. Hash once rather than once
    // per stroke. This is an ABI match, never a user activation/seed requirement.
    static const bool exactExecutable = matchesRunningXochitl();
    if (!exactExecutable) return refused("La fabrique native ne correspond pas à ce firmware.");
    if (QMetaType::fromName("Line").sizeOf() != 0x58 ||
        QMetaType::fromName("QList<std::shared_ptr<SceneItem>>").sizeOf() != 24)
        return refused("Les types natifs de trait ne correspondent pas au profil.");

    // e85a50 is the COMPLETE factory used by the binary and JSON scene readers.
    // Its uint8 tag 5 branch creates a SceneLineItem with Xochitl's allocator,
    // vtable, shared_ptr control block and destructor. AAPCS64 places the
    // nontrivial shared_ptr return storage in x8; the compiler handles that ABI.
    using Factory = std::shared_ptr<SceneItem> (*)(quint8);
    static_assert(sizeof(std::shared_ptr<SceneItem>) == 16);
    const auto factory = reinterpret_cast<Factory>(quintptr(0xe85a50));
    auto item = factory(quint8(5));
    if (!item || reinterpret_cast<quintptr>(item->vtable) != quintptr(0x1682940))
        return refused("La fabrique native n’a pas retourné un trait reconnu.");

    // Tag-5 deserialization starts with a zero timestamp, then fills RM field6.
    // New native drawing at e2b654 instead initializes that field to (0:1).
    // Match this default on the private object. This is NOT the item identity:
    // ID/parent at +10/+18 stay zero until the native insertion assigns them.
    auto *bytes = reinterpret_cast<unsigned char *>(item.get());
    quint64 timestamp = 0;
    std::memcpy(&timestamp, bytes + 0xa0, sizeof(timestamp));
    if (timestamp != 0) return refused("Le défaut de la fabrique native est inattendu.");
    timestamp = 1;
    std::memcpy(bytes + 0xa0, &timestamp, sizeof(timestamp));
    return item;
#else
    return refused("La fabrique Xochitl est indisponible sur cet hôte.");
#endif
}
