#pragma once
#include <QVariantMap>

class QObject;
namespace RePaperNative {
// Exact-firmware, read-only observation of the sources used by native Copy.
// This does not validate reconciliation, ownership of semantic objects or edits.
// No call site is enabled by adding this file; NativeScene remains unchanged.
QVariantMap observeOriginalSelection(QObject *controller, int layer);
}
