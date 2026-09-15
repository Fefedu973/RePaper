// Desktop-only update pump for Qt's generic linuxfb renderer.
// Links the pinned AppLoad QTFB client; upstream COPYING remains in the source snapshot.
#include "qtfb-client.h"
#include <chrono>
#include <csignal>
#include <cstdint>
#include <thread>

static volatile std::sig_atomic_t running = 1;
static void stop(int) { running = 0; }
int main() {
    std::signal(SIGTERM, stop);
    std::signal(SIGINT, stop);
    std::signal(SIGPIPE, SIG_IGN);
    qtfb::ClientConnection connection(qtfb::getIDFromAppload(), FBFMT_RM2FB, {}, false);
    uint64_t previous = 0;
    while (running) {
        uint64_t hash = 1469598103934665603ULL;
        for (size_t index = 0; index < connection.shmSize; ++index) hash = (hash ^ connection.shm[index]) * 1099511628211ULL;
        if (hash != previous) { connection.sendCompleteUpdate(); previous = hash; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
