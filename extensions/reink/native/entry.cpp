#include "EditorAdapter.h"
#include "NativePreviewItem.h"
#include "NativeInputScheduler.h"
#include "TargetProfile.h"
#include <QQmlEngine>
#include <QResource>
#include <QDebug>
#include "xovi.h"

extern "C" void _xovi_construct() {
    // The QMD is registered only for the pinned executable, before its resources
    // are loaded. Scene construction/observation are gated by this exact profile.
    if(!RePaperNative::matchesRunningXochitl())return;
    // xovigen exposes an untyped link-table function. This signature is the
    // exported C declaration from qt-resource-rebuilder/src/qmldiff.h (v19).
    using AddDiff = char (*)(const char *, const char *);
    using SlotGuard = void (*)();
    const auto addDiff = reinterpret_cast<AddDiff>(qt_resource_rebuilder$qmldiff_add_external_diff);
    const auto disableSlots = reinterpret_cast<SlotGuard>(qt_resource_rebuilder$qmldiff_disable_slots_while_processing);
    const auto enableSlots = reinterpret_cast<SlotGuard>(qt_resource_rebuilder$qmldiff_enable_slots_while_processing);
    if(!addDiff||!disableSlots||!enableSlots){qWarning()<<"[RePaper native] QMD exports unavailable; module disabled";return;}
    if(!addDiff(r$editorHooks,"RePaper native toolbar 3.28.0.169")){
        qWarning()<<"[RePaper native] QMD registration rejected; module disabled";return;
    }
    qmlRegisterType<EditorAdapter>("RePaper.Editor",1,0,"EditorAdapter");
    qmlRegisterType<NativePreviewItem>("RePaper.Editor",1,0,"NativePreviewItem");
    qmlRegisterType<NativeInputScheduler>("RePaper.Editor",1,0,"NativeInputScheduler");
    disableSlots();
    const bool panels = QResource::registerResource(reinterpret_cast<const unsigned char*>(r$editorPanels));
    const bool page = QResource::registerResource(reinterpret_cast<const unsigned char*>(r$nativePage));
    enableSlots();
    if(!panels||!page)qWarning()<<"[RePaper native] Embedded resources unavailable; toolbar loading will fail";
}
