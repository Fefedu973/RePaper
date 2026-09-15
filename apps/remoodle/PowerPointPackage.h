#pragma once
#include <QString>

// Read only slide XML and package metadata. This never extracts archive paths.
// Returns a user-facing reason when the package cannot be converted faithfully.
QString validatePowerPointPackage(const QString &path);
