// Device-only owner of one Qt app and its QTFB refresh process. No Qt/Python dependency.
#include "runtime.h"
#include <array>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
static volatile std::sig_atomic_t stopSignal = 0;
static void requestStop(int signal) { stopSignal = signal; }

struct PrivateInputs {
    static constexpr std::array<const char *, 6> names{{"touch", "pen", "keys", "buttons", "null", "framebuffer"}};
    fs::path directory;
    std::array<int, 6> fds{{-1, -1, -1, -1, -1, -1}};
    PrivateInputs() {
        char pattern[] = "/tmp/repaper-appload-XXXXXX";
        const char *created = mkdtemp(pattern);
        if (!created) repaper::fail("create private input directory");
        directory = created;
        try {
            for (size_t i = 0; i < names.size(); ++i) {
                fds[i] = open(path(i).c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
                if (fds[i] < 0) repaper::fail("create private input file");
            }
        } catch (...) { cleanup(); throw; }
    }
    std::string path(size_t i) const { return (directory / names[i]).string(); }
    std::string alias(size_t i) const { return "/dev/fd/" + std::to_string(fds[i]); }
    void cleanup() {
        for (int &fd : fds) { if (fd >= 0) close(fd); fd = -1; }
        // Only remove the six files and directory created by this launch; never recurse.
        for (size_t i = 0; i < names.size(); ++i) unlink(path(i).c_str());
        rmdir(directory.c_str());
    }
    ~PrivateInputs() { cleanup(); }
};

static fs::path runtimeDirectory() {
    std::array<char, 4096> path{};
    const ssize_t length = readlink("/proc/self/exe", path.data(), path.size() - 1);
    if (length <= 0 || static_cast<size_t>(length) >= path.size() - 1)
        throw std::runtime_error("Cannot locate the AppLoad runtime directory.");
    return fs::path(std::string(path.data(), static_cast<size_t>(length))).parent_path();
}

static void childSetup(pid_t parent, const PrivateInputs &inputs) {
    // This process is never allowed to outlive its supervisor, even on SIGKILL.
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != parent) _exit(125);
    if (setpgid(0, 0) != 0) _exit(125);
    for (int signal : {SIGTERM, SIGINT, SIGHUP, SIGPIPE}) std::signal(signal, SIG_DFL);
    if (dup2(inputs.fds[4], STDIN_FILENO) < 0) _exit(125);
    // stdin is a private regular file, so Qt's VT handler cannot use a controlling tty.
}

static void appEnvironment(const fs::path &runtime, const PrivateInputs &inputs, bool usesBridge) {
    using repaper::setEnvironment;
    for (const char *name : {"REPAPER_PC_EMULATOR", "REPAPER_PC_HANDOFF_HELPER",
            "REPAPER_EMULATOR_UI_SCALE", "PAPER_BRIDGE_SOCKET", "QT_SCREEN_SCALE_FACTORS",
            "QT_USE_PHYSICAL_DPI", "QT_QPA_EGLFS_INTEGRATION", "QT_QPA_FB_DRM"})
        unsetenv(name);
    setEnvironment("LD_LIBRARY_PATH", (runtime / "lib").string());
    setEnvironment("LD_PRELOAD", (runtime / "qtfb-shim.so").string());
    setEnvironment("QT_PLUGIN_PATH", (runtime / "plugins").string());
    setEnvironment("QT_QPA_PLATFORM_PLUGIN_PATH", (runtime / "plugins/platforms").string());
    setEnvironment("QT_QPA_FONTDIR", (runtime / "fonts").string());
    setEnvironment("FONTCONFIG_FILE", (runtime / "fonts/fonts.conf").string());
    setEnvironment("SSL_CERT_FILE", (runtime / "certs/ca-certificates.crt").string());
    setEnvironment("REPAPER_APPLOAD_TABLET_INPUT", "1");
    if (fs::exists(runtime / "office/convert")) setEnvironment("REPAPER_OFFICE_CONVERTER", (runtime / "office/convert").string());
    if (usesBridge) setEnvironment("REPAPER_BRIDGE_STARTER", (runtime / "bridge-start").string());
    else unsetenv("REPAPER_BRIDGE_STARTER");
    setEnvironment("QML_IMPORT_PATH", (runtime / "qml").string());
    setEnvironment("QML2_IMPORT_PATH", (runtime / "qml").string());
    setEnvironment("QT_QPA_PLATFORM", "linuxfb:fb=" + inputs.path(5)
        + ":tty=" + inputs.path(4) + ":nographicsmodeswitch");
    setEnvironment("QT_QPA_FB_NO_LIBINPUT", "1");
    // Disable all automatic discovery; explicit generic handlers open private FD aliases.
    setEnvironment("QT_QPA_FB_DISABLE_INPUT", "1");
    setEnvironment("QT_QPA_GENERIC_PLUGINS", "evdevtouch:" + inputs.alias(0)
        + ",evdevkeyboard:" + inputs.alias(2) + ",evdevtablet:" + inputs.alias(1));
    setEnvironment("QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS", inputs.alias(0));
    setEnvironment("QT_QPA_EVDEV_KEYBOARD_PARAMETERS", inputs.alias(2));
    setEnvironment("QT_QPA_EVDEV_TABLET_PARAMETERS", inputs.alias(1));
    setEnvironment("QT_QPA_PRESERVE_CONSOLE_STATE", "1");
    setEnvironment("QT_QPA_NO_SIGNAL_HANDLER", "1");
    setEnvironment("QT_QPA_FB_HIDECURSOR", "1");
    setEnvironment("QT_QUICK_BACKEND", "software");
    setEnvironment("QT_QUICK_CONTROLS_STYLE", "Basic");
    setEnvironment("QT_SCALE_FACTOR", "2");
    setEnvironment("QT_FONT_DPI", "96");
    setEnvironment("QTFB_SHIM_MODEL", "RMPP");
    setEnvironment("QTFB_SHIM_MODE", "RGB565");
    setEnvironment("QTFB_SHIM_INPUT_MODE", "RMPP");
    setEnvironment("QTFB_SHIM_INPUT", "1");
    setEnvironment("QTFB_SHIM_FB", "1");
    setEnvironment("QTFB_SHIM_FB_PATH", inputs.path(5));
    setEnvironment("QTFB_SHIM_INPUT_PATH_TOUCHSCREEN", inputs.path(0));
    setEnvironment("QTFB_SHIM_INPUT_PATH_DIGITIZER", inputs.path(1));
    setEnvironment("QTFB_SHIM_INPUT_PATH_KEYS", inputs.path(2));
    setEnvironment("QTFB_SHIM_INPUT_PATH_BUTTONS", inputs.path(3));
    setEnvironment("QTFB_SHIM_INPUT_PATH_NULL", inputs.path(4));
    setEnvironment("QTFB_SHIM_INITIAL_DISPLAY_MODE", "UI");
}

static bool exited(pid_t pid, siginfo_t *result = nullptr) {
    if (pid <= 0) return true;
    siginfo_t status{};
    if (waitid(P_PID, static_cast<id_t>(pid), &status, WEXITED | WNOHANG | WNOWAIT) != 0) {
        if (errno == EINTR) return false;
        repaper::fail("observe child process");
    }
    if (result) *result = status;
    return status.si_pid != 0;
}

struct Children {
    pid_t app = -1;
    pid_t pump = -1;
    pid_t starter = -1;
    void stop() noexcept {
        // Leaders remain unreaped until group cleanup, preventing PID/PGID reuse here.
        for (pid_t pid : {app, pump, starter}) if (pid > 0) kill(-pid, SIGTERM);
        const auto deadline = Clock::now() + std::chrono::seconds(2);
        try {
            while ((!exited(app) || !exited(pump) || !exited(starter)) && Clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } catch (...) { /* Finish the bounded cleanup even after a wait error. */ }
        for (pid_t pid : {app, pump, starter}) if (pid > 0) kill(-pid, SIGKILL);
        for (pid_t &pid : {std::ref(app), std::ref(pump), std::ref(starter)}) {
            if (pid > 0) {
                while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
                pid = -1;
            }
        }
    }
    ~Children() { stop(); }
};

static void startBridge(Children &children, const fs::path &runtime, const PrivateInputs &inputs) {
    const auto executable = (runtime / "bridge-start").string();
    if (access(executable.c_str(), X_OK) != 0) {
        std::cerr << "rePaper AppLoad: Paper Bridge starter unavailable; document actions can retry from the app.\n";
        return;
    }
    const pid_t parent = getpid();
    children.starter = fork();
    if (children.starter == 0) {
        childSetup(parent, inputs);
        unsetenv("LD_PRELOAD");
        unsetenv("LD_LIBRARY_PATH");
        unsetenv("QTFB_KEY");
        unsetenv("REPAPER_BRIDGE_STARTER");
        execl(executable.c_str(), executable.c_str(), nullptr);
        _exit(127);
    }
    if (children.starter < 0) {
        std::cerr << "rePaper AppLoad: Paper Bridge starter could not run; document actions can retry from the app.\n";
        return;
    }
    setpgid(children.starter, children.starter);
    const auto deadline = Clock::now() + std::chrono::seconds(15);
    siginfo_t status{};
    bool complete = false;
    while (!stopSignal && Clock::now() < deadline) {
        if (exited(children.starter, &status)) { complete = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    // Reap only this helper's group. The actual Bridge worker belongs to its systemd unit.
    kill(-children.starter, SIGKILL);
    while (waitpid(children.starter, nullptr, 0) < 0 && errno == EINTR) {}
    children.starter = -1;
    if (!stopSignal && (!complete || status.si_code != CLD_EXITED || status.si_status != 0))
        std::cerr << "rePaper AppLoad: Paper Bridge startup failed or timed out; the app remains available for retry.\n";
}

static void startPump(Children &children, const fs::path &runtime, const PrivateInputs &inputs) {
    int ready[2];
    if (pipe2(ready, O_CLOEXEC) != 0) repaper::fail("create refresh readiness pipe");
    const pid_t parent = getpid();
    children.pump = fork();
    if (children.pump == 0) {
        try {
        childSetup(parent, inputs);
        close(ready[0]);
        if (fcntl(ready[1], F_SETFD, 0) != 0) _exit(125);
        unsetenv("LD_PRELOAD");
        repaper::setEnvironment("LD_LIBRARY_PATH", (runtime / "lib").string());
        const auto fd = std::to_string(ready[1]);
        const auto executable = (runtime / "qt-linuxfb-refresh").string();
        execl(executable.c_str(), executable.c_str(), "--ready-fd", fd.c_str(), nullptr);
        _exit(127);
        } catch (...) { _exit(125); }
    }
    close(ready[1]);
    if (children.pump < 0) { close(ready[0]); repaper::fail("start refresh process"); }
    setpgid(children.pump, children.pump);
    pollfd watched{ready[0], POLLIN, 0};
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    bool connected = false;
    while (!stopSignal && Clock::now() < deadline) {
        const int count = poll(&watched, 1, 50);
        if (count > 0) {
            char response = 0;
            connected = read(ready[0], &response, 1) == 1 && response == 'R';
            break;
        }
        if (count < 0 && errno != EINTR) break;
    }
    close(ready[0]);
    if (!connected) throw std::runtime_error("QTFB refresh initialization failed; application was not started.");
}

int main(int argc, char **argv) {
    if (argc < 2 || std::string(argv[1]) == "--help") {
        std::cerr << "Usage: repaper-appload-launch APP_EXECUTABLE [APP_ARGUMENTS...]\n"
                     "Start through an AppLoad qtfb external manifest.\n";
        return argc < 2 ? 2 : 0;
    }
    try {
        repaper::framebufferKey();
        const fs::path runtime = runtimeDirectory();
        const auto app = fs::canonical(argv[1]).string();
        const auto appName = fs::path(app).filename();
        const bool usesBridge = appName == "remoodle" || appName == "reagenda";
        for (const auto &file : {fs::path(app), runtime / "qt-linuxfb-refresh", runtime / "qtfb-shim.so"})
            if (access(file.c_str(), file.extension() == ".so" ? R_OK : X_OK) != 0)
                throw std::runtime_error("Missing or inaccessible application runtime file: " + file.string());
        for (int signal : {SIGTERM, SIGINT, SIGHUP}) {
            struct sigaction action{};
            action.sa_handler = requestStop;
            sigemptyset(&action.sa_mask);
            if (sigaction(signal, &action, nullptr) != 0) repaper::fail("register supervisor signal handler");
        }
        PrivateInputs inputs;
        Children children;
        if (usesBridge) startBridge(children, runtime, inputs);
        if (stopSignal) return 128 + stopSignal;
        startPump(children, runtime, inputs);
        if (stopSignal) return 128 + stopSignal;
        const pid_t parent = getpid();
        children.app = fork();
        if (children.app < 0) repaper::fail("start application");
        if (children.app == 0) {
            try {
            childSetup(parent, inputs);
            for (size_t i : {0U, 1U, 2U})
                if (fcntl(inputs.fds[i], F_SETFD, 0) != 0) _exit(125);
            appEnvironment(runtime, inputs, usesBridge);
            std::vector<char *> args{const_cast<char *>(app.c_str())};
            for (int i = 2; i < argc; ++i) args.push_back(argv[i]);
            args.push_back(nullptr);
            execv(app.c_str(), args.data());
            _exit(127);
            } catch (...) { _exit(125); }
        }
        setpgid(children.app, children.app);
        for (;;) {
            if (stopSignal) return 128 + stopSignal;
            siginfo_t status{};
            if (exited(children.app, &status))
                return status.si_code == CLD_EXITED ? status.si_status : 128 + status.si_status;
            if (exited(children.pump))
                throw std::runtime_error("QTFB refresh process stopped; closing the application.");
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    } catch (const std::exception &error) {
        std::cerr << "rePaper AppLoad: " << error.what() << '\n';
        return stopSignal ? 128 + stopSignal : 1;
    }
}
