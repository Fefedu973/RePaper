#pragma once
#include <QCoreApplication>
#include <QProcess>
#include <QProcessEnvironment>

// Test-only child execution. Cross-compiled Qt tests can launch their own
// fixture modes through QEMU without registering a machine-wide binfmt rule.
inline void startTestProcess(QProcess &process, const QStringList &arguments) {
    const auto emulator = qEnvironmentVariable("REPAPER_TEST_EMULATOR");
    const auto sysroot = qEnvironmentVariable("REPAPER_TEST_SYSROOT");
    if (emulator.isEmpty()) {
        process.start(QCoreApplication::applicationFilePath(), arguments);
        return;
    }
    auto environment = QProcessEnvironment::systemEnvironment();
    // QEMU itself is an x86_64 executable; its guest library path is passed
    // explicitly with -E instead of affecting the native host loader.
    environment.remove("LD_LIBRARY_PATH");
    environment.remove("LD_PRELOAD");
    process.setProcessEnvironment(environment);
    QStringList command{"-L", sysroot, "-E", "LD_LIBRARY_PATH=" + sysroot + "/usr/lib:" + sysroot + "/lib",
                        QCoreApplication::applicationFilePath()};
    command.append(arguments);
    process.start(emulator, command);
}
