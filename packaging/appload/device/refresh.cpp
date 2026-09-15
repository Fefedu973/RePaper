// Qt generic linuxfb writes RGB565 memory; publish changed row bounds to AppLoad.
// Wire types come from the reviewed AppLoad source; no Qt or input-device access.
#include "runtime.h"
#include "common.h"
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <vector>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static volatile std::sig_atomic_t stopping = 0;
static void stop(int) { stopping = 1; }
using Clock = std::chrono::steady_clock;
static constexpr size_t stride = RMPP_WIDTH * 2;
static constexpr size_t byteCount = stride * RMPP_HEIGHT;

struct Connection {
    int socket = -1;
    int memoryFile = -1;
    const unsigned char *memory = nullptr;
    ~Connection() {
        if (memory) munmap(const_cast<unsigned char *>(memory), byteCount);
        if (memoryFile >= 0) close(memoryFile);
        if (socket >= 0) close(socket);
    }
    void send(const qtfb::ClientMessage &message) {
        if (::send(socket, &message, sizeof(message), MSG_NOSIGNAL | MSG_DONTWAIT) != sizeof(message))
            repaper::fail("send QTFB refresh message");
    }
    void initialize() {
        socket = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (socket < 0) repaper::fail("create QTFB connection");
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, SOCKET_PATH, sizeof(address.sun_path) - 1);
        if (connect(socket, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0)
            repaper::fail("connect to AppLoad");
        qtfb::ClientMessage request{};
        request.type = MESSAGE_INITIALIZE;
        request.init.framebufferKey = repaper::framebufferKey();
        request.init.framebufferType = FBFMT_RMPP_RGB565;
        send(request);
        pollfd watched{socket, POLLIN, 0};
        int count;
        do { count = poll(&watched, 1, 3000); } while (count < 0 && errno == EINTR && !stopping);
        if (stopping || count <= 0) throw std::runtime_error("AppLoad framebuffer initialization timed out or was cancelled.");
        qtfb::ServerMessage response{};
        if (recv(socket, &response, sizeof(response), MSG_DONTWAIT) != sizeof(response)
                || response.type != MESSAGE_INITIALIZE || response.init.shmSize != byteCount)
            throw std::runtime_error("AppLoad returned an incompatible RGB565 framebuffer.");
        FORMAT_SHM(name, response.init.shmKeyDefined);
        memoryFile = shm_open(name, O_RDONLY | O_CLOEXEC, 0);
        if (memoryFile < 0) repaper::fail("open QTFB shared memory");
        struct stat info{};
        if (fstat(memoryFile, &info) != 0 || info.st_size != static_cast<off_t>(byteCount))
            throw std::runtime_error("AppLoad shared memory has an unexpected size.");
        void *mapping = mmap(nullptr, byteCount, PROT_READ, MAP_SHARED, memoryFile, 0);
        if (mapping == MAP_FAILED) repaper::fail("map QTFB shared memory");
        memory = static_cast<const unsigned char *>(mapping);
    }
    bool drain() {
        // Input and rotation packets belong to the app's shim; never log their payloads.
        for (int i = 0; i < 128; ++i) {
            qtfb::ServerMessage message{};
            const auto count = recv(socket, &message, sizeof(message), MSG_DONTWAIT);
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0 || message.type == MESSAGE_TERMINATE) return false;
        }
        return true;
    }
};

int main(int argc, char **argv) {
    try {
        int readyFd = -1;
        if (argc == 3 && std::string(argv[1]) == "--ready-fd") {
            char *end = nullptr;
            const long value = std::strtol(argv[2], &end, 10);
            if (!*argv[2] || *end || value < 3 || value > INT_MAX)
                throw std::runtime_error("Invalid refresh readiness descriptor.");
            readyFd = static_cast<int>(value);
        } else if (argc != 1) {
            throw std::runtime_error("Usage: qt-linuxfb-refresh [--ready-fd FD]");
        }
        for (int signal : {SIGTERM, SIGINT, SIGHUP}) {
            struct sigaction action{};
            action.sa_handler = stop;
            sigemptyset(&action.sa_mask);
            if (sigaction(signal, &action, nullptr) != 0) repaper::fail("register refresh signal handler");
        }
        std::signal(SIGPIPE, SIG_IGN);
        Connection connection;
        connection.initialize();
        if (readyFd >= 0) {
            if (write(readyFd, "R", 1) != 1) repaper::fail("acknowledge refresh initialization");
            close(readyFd);
        }
        std::vector<unsigned char> previous(byteCount);
        bool first = true;
        bool recentChange = false;
        bool moving = false;
        int refreshMode = DEFAULT_WAVEFORM_MODE;
        int motionTop = RMPP_HEIGHT;
        int motionBottom = -1;
        auto lastChange = Clock::now();
        const auto setRefreshMode = [&](int mode) {
            if (mode == refreshMode) return;
            qtfb::ClientMessage message{};
            message.type = MESSAGE_SET_REFRESH_MODE;
            message.refreshMode = mode;
            connection.send(message);
            refreshMode = mode;
        };
        const auto updateRows = [&](int top, int bottom, bool complete = false) {
            qtfb::ClientMessage update{};
            update.type = MESSAGE_UPDATE;
            update.update.type = complete ? UPDATE_ALL : UPDATE_PARTIAL;
            update.update.x = 0;
            update.update.y = top;
            update.update.w = RMPP_WIDTH;
            update.update.h = bottom - top + 1;
            connection.send(update);
        };
        auto nextUpdate = Clock::now();
        while (!stopping) {
            const auto now = Clock::now();
            if (now >= nextUpdate) {
                // A motion burst gets one quality redraw after it stops. An
                // isolated change (for example a blinking cursor) does not.
                if (recentChange && now - lastChange >= std::chrono::milliseconds(300)) {
                    if (moving) {
                        setRefreshMode(REFRESH_MODE_CONTENT);
                        updateRows(motionTop, motionBottom);
                    }
                    recentChange = false;
                    moving = false;
                    motionTop = RMPP_HEIGHT;
                    motionBottom = -1;
                }
                int top = RMPP_HEIGHT;
                int bottom = -1;
                for (int y = 0; y < RMPP_HEIGHT; ++y) {
                    const auto offset = static_cast<size_t>(y) * stride;
                    if (first || std::memcmp(previous.data() + offset, connection.memory + offset, stride) != 0) {
                        top = std::min(top, y);
                        bottom = y;
                        std::memcpy(previous.data() + offset, connection.memory + offset, stride);
                    }
                }
                if (bottom >= top) {
                    if (!first) {
                        // Consecutive changed samples identify scrolling or
                        // animation without hooks in individual applications.
                        if (recentChange && now - lastChange <= std::chrono::milliseconds(200))
                            moving = true;
                        if (moving) setRefreshMode(REFRESH_MODE_ANIMATE);
                        motionTop = std::min(motionTop, top);
                        motionBottom = std::max(motionBottom, bottom);
                        recentChange = true;
                        lastChange = now;
                    }
                    updateRows(top, bottom, first);
                    first = false;
                }
                nextUpdate = now + std::chrono::milliseconds(recentChange ? 50 : 100);
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(nextUpdate - Clock::now()).count();
            pollfd watched{connection.socket, POLLIN, 0};
            const int result = poll(&watched, 1, static_cast<int>(std::max<int64_t>(0, remaining)));
            if (result < 0 && errno != EINTR) repaper::fail("poll QTFB connection");
            if (result > 0 && !connection.drain()) {
                if (!stopping) throw std::runtime_error("AppLoad closed the framebuffer connection.");
            }
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "rePaper refresh: " << error.what() << '\n';
        return 1;
    }
}
