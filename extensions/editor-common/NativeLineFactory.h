#pragma once
#include <QString>
#include <memory>

struct SceneItem;
namespace RePaperNative {
// Allocates a private empty line through the exact firmware's own factory.
// Does not read a user's selection, insert anything, or access a page/worker.
// The caller can then assign its attested Line subobject and submit the item
// through the native batch API. Unsupported host/firmware returns an empty ptr.
std::shared_ptr<SceneItem> createNativeLineItem(QString *error = nullptr);
}
