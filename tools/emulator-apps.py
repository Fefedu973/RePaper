#!/usr/bin/env python3
"""Build the pinned AppLoad PC emulator and register locally built native apps.

Run inside Linux/WSL; no SSH or tablet operation exists in this tool.
Third-party source stays under the chosen emulator state directory.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from contextlib import contextmanager

APPLOAD_COMMIT = "1a708d297df4b3c108e78a02ec7e2634ff33033c"
PR59_COMMIT = "40506d47427123f07030bb2e83453a43d035b16a"
NAMES = {"remoodle": "reMoodle", "reagenda": "reAgenda", "restencil": "reStencil", "recalc": "reCalc", "reink": "reInk"}
ARCHIVED_NAMES = {"rmchat": "RMChat (ChatGPT)"}
ALL_NAMES = dict(NAMES, **ARCHIVED_NAMES)
PROFILE_FORMAT = json.loads((Path(__file__).resolve().parents[1] / "packaging/appload/profile-version.json").read_text())
PROFILE_VERSION = PROFILE_FORMAT["profileVersion"]
DISPLAY_VERSION = PROFILE_FORMAT["displayVersion"]


def profile_is_current(state, info):
    return (isinstance(info, dict) and info.get("profile_version") == PROFILE_VERSION
            and info.get("display_version") == DISPLAY_VERSION
            and info.get("qtfb_socket") == str(state.resolve() / "runtime/qtfb.sock"))


def ui_scale(value):
    scale = float(value)
    if not math.isfinite(scale) or not 1 <= scale <= 2:
        raise argparse.ArgumentTypeError("UI scale must be between 1 and 2 (default: 1.5).")
    return scale


@contextmanager
def exclusive_instance(state):
    # Generated binaries namespace both their socket and shared memory per state directory.
    import fcntl
    directory = state.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    with (directory / "appload.lock").open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError("This AppLoad profile is already running or being built. Close its window before launching or rebuilding it.")
        yield


def run(args, **kwargs):
    subprocess.run([str(arg) for arg in args], check=True, **kwargs)


def atomic_json(path, value):
    """Publish a complete manifest without exposing a partially written file."""
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=path.parent,
                                         prefix=f".{path.name}.", suffix=".tmp", delete=False) as output:
            temporary = Path(output.name)
            json.dump(value, output, indent=2)
            output.write("\n")
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def app_manifest(state, sandbox, app_build, identifier):
    root = Path(__file__).resolve().parents[1]
    candidates = [app_build / "apps" / identifier / identifier, app_build / identifier]
    for candidate in candidates:
        binary = candidate.resolve()
        if not binary.is_file() or not os.access(binary, os.X_OK):
            continue
        core = binary.with_name("rmchat-core")
        if identifier == "rmchat" and (not core.is_file() or not os.access(core, os.X_OK)):
            continue
        folder = state / "runtime/applications_root" / identifier
        environment = {"XDG_DATA_HOME": str(sandbox / "state/inputs"),
                       "PAPER_BRIDGE_SOCKET": str(sandbox / "core.sock")}
        if identifier == "rmchat":
            environment.update(RMCHAT_CORE_PATH=str(core),
                               XDG_CONFIG_HOME=str(sandbox / "state/config"),
                               XDG_CACHE_HOME=str(sandbox / "state/cache"))
        return {"name": ALL_NAMES[identifier], "application": sys.executable, "workingDirectory": str(folder),
                "qtfb": True, "disablesWindowedMode": False, "aspectRatio": "original",
                "environment": environment,
                "args": [str(root / "packaging/appload/run-native.py"), "--state", str(state), str(binary)]
                        + (["--emulator"] if identifier == "restencil" else [])}
    return None


def publish_manifests(state, manifests):
    for identifier, manifest in manifests.items():
        folder = state / "runtime/applications_root" / identifier
        folder.mkdir(parents=True, exist_ok=True)
        atomic_json(folder / "external.manifest.json", manifest)


def require_archive_opt_in(args):
    if getattr(args, "app", None) in ARCHIVED_NAMES and not getattr(args, "allow_archived", False):
        raise RuntimeError("RMChat is archived and excluded from the standard release. "
                           "For explicit research use only, add --allow-archived; see apps/rmchat/ARCHIVED.md.")


def archive_registration(state, info, identifier="rmchat"):
    """Hide an archived app, retaining its manifest and all application data."""
    manifest = state / "runtime/applications_root" / identifier / "external.manifest.json"
    if manifest.is_file():
        archive = state / "archived-applications" / identifier
        archive.mkdir(parents=True, exist_ok=True)
        destination = archive / f"external.manifest.{time.time_ns()}.json"
        manifest.rename(destination)
        print(f"Archived {identifier} registration: {destination}")
    registered = [app for app in info.get("apps", []) if app != identifier]
    if registered != info.get("apps", []):
        info = dict(info, apps=registered)
        atomic_json(state / "build-info.json", info)
    return info


def archive(args):
    state = args.state_dir.resolve()
    info = json.loads((state / "build-info.json").read_text())
    archive_registration(state, info, args.app)
    print(f"{args.app} is archived. Sources, binaries, credentials and application data are preserved.")


def register(args):
    """Register ready binaries in an existing profile; never rebuild or launch it."""
    require_archive_opt_in(args)
    state = args.state_dir.resolve()
    info = json.loads((state / "build-info.json").read_text())
    if not profile_is_current(state, info) or not (state / "build/appload").is_file():
        raise RuntimeError("Run setup for this emulator profile before registering applications.")
    sandbox = Path(info["sandbox"]).resolve()
    identifiers = [args.app] if args.app else list(NAMES)
    manifests = {}
    for identifier in identifiers:
        manifest = app_manifest(state, sandbox, args.app_build, identifier)
        if manifest is not None:
            manifests[identifier] = manifest
        elif args.app:
            required = "rmchat and its adjacent rmchat-core" if identifier == "rmchat" else identifier
            raise RuntimeError(f"No executable {required} found in {args.app_build}.")
    if not manifests:
        raise RuntimeError(f"No executable applications found in {args.app_build}.")
    if args.app not in ARCHIVED_NAMES:
        info = archive_registration(state, info)
    registered = list(info.get("apps", []))
    for identifier in manifests:
        if identifier not in registered:
            registered.append(identifier)
    publish_manifests(state, manifests)
    atomic_json(state / "build-info.json", dict(info, apps=registered))
    print(f"Registered: {', '.join(manifests)}. AppLoad and the other applications were not rebuilt or launched.")


def replace_checked(path, before, after):
    content = path.read_text()
    if content.count(before) != 1:
        raise RuntimeError(f"Pinned upstream patch no longer matches {path.name}")
    path.write_text(content.replace(before, after))


def prepare_source(repo, destination):
    endpoint = destination.parent.resolve() / "runtime/qtfb.sock"
    if len(os.fsencode(endpoint)) > 107:
        raise RuntimeError("Emulator state path is too long for its Unix socket (107 bytes maximum).")
    if not repo.exists():
        repo.parent.mkdir(parents=True, exist_ok=True)
        run(["git", "clone", "--no-checkout", "https://github.com/asivery/rm-appload.git", repo])
        run(["git", "-C", repo, "checkout", "--detach", APPLOAD_COMMIT])
    actual = subprocess.check_output(["git", "-C", str(repo), "rev-parse", "HEAD"], text=True).strip()
    if actual != APPLOAD_COMMIT:
        raise RuntimeError(f"AppLoad checkout must be pinned to {APPLOAD_COMMIT}; found {actual}")
    # A Windows checkout can contain CRLF even when WSL's Git defaults to core.autocrlf=false.
    if subprocess.check_output(["git", "-c", "core.autocrlf=true", "-C", str(repo), "status", "--porcelain", "--untracked-files=no"], text=True).strip():
        raise RuntimeError("AppLoad reference checkout has local changes; preserve or move them before setup.")
    # Only refresh our own generated source snapshot; never mutate the reference checkout.
    if destination.exists():
        if destination.name != "source" or destination.resolve() == repo.resolve() or destination.is_symlink():
            raise RuntimeError("Unsafe emulator source directory")
        shutil.rmtree(destination)
    shutil.copytree(repo, destination, dirs_exist_ok=True, symlinks=True,
                    ignore=shutil.ignore_patterns(".git", "applications_root", "*.o", "Makefile", ".qmake.stash"))
    replace_checked(destination / "src/qtfb/common.h", '#define SOCKET_PATH "/tmp/qtfb.sock"',
                    "#define SOCKET_PATH " + json.dumps(str(endpoint)))
    namespace = hashlib.sha256(os.fsencode(destination.parent.resolve())).hexdigest()[:32]
    replace_checked(destination / "src/qtfb/common.h",
                    '#define FORMAT_SHM(var, key) char var[20]; snprintf(var, 20, "/qtfb_%d", key)',
                    f'#define FORMAT_SHM(var, key) char var[64]; snprintf(var, 64, "/repaper_{namespace}_%d", key)')
    replace_checked(destination / "src/AppLibrary.h",
        "for(const auto [key, value] : _extraEnv.asKeyValueRange()) extraEnv.insert(key, value.toString());",
        "for(auto it = _extraEnv.cbegin(); it != _extraEnv.cend(); ++it) extraEnv.insert(it.key(), it.value().toString());")
    replace_checked(destination / "src/libraryexternals.cpp",
        "for (const auto [key, value] : extraEnv.asKeyValueRange()) {\n        env.insert(key, value);\n    }",
        "for (auto it = extraEnv.cbegin(); it != extraEnv.cend(); ++it) {\n        env.insert(it.key(), it.value());\n    }")
    replace_checked(destination / "src/qtfb/FBController.h", "#include <QQuickPaintedItem>",
                    "#include <QQuickPaintedItem>\n#include <QQuickWindow>")
    replace_checked(destination / "src/qtfb/FBController.h", "setFocusPolicy(Qt::StrongFocus);", """setActiveFocusOnTab(true); setFocus(true); setSmooth(true); setMipmap(true);
        connect(this, &QQuickItem::widthChanged, this, &FBController::resizeViewportTexture);
        connect(this, &QQuickItem::heightChanged, this, &FBController::resizeViewportTexture);
        connect(this, &QQuickItem::windowChanged, this, [this](QQuickWindow *w) {
            if (!w) return;
            connect(w, &QWindow::widthChanged, this, [this] { QMetaObject::invokeMethod(this, &FBController::resizeViewportTexture, Qt::QueuedConnection); });
            connect(w, &QWindow::heightChanged, this, [this] { QMetaObject::invokeMethod(this, &FBController::resizeViewportTexture, Qt::QueuedConnection); });
            resizeViewportTexture();
        });""")
    replace_checked(destination / "src/qtfb/FBController.h", "    void mouseEvent(QMouseEvent *me, int inputType);",
                    "    void resizeViewportTexture();\n    void mouseEvent(QMouseEvent *me, int inputType);")
    # Qt 6.2's software painter node ignores its linear-filtering flag on the final drawPixmap.
    # Render directly at the PC viewport size, so QPainter performs one smooth downsample and
    # the outer QML transform displays that texture 1:1. Qt adds the window DPR automatically.
    replace_checked(destination / "src/qtfb/FBController.cpp", "void FBController::paint(QPainter *painter) {",
        """void FBController::resizeViewportTexture() {
    if (!window() || width() <= 0 || height() <= 0) return;
    const QSize viewport = mapRectToScene(boundingRect()).size().toSize();
    if (viewport.isValid() && viewport != textureSize()) {
        setTextureSize(viewport);
        update();
    }
}

void FBController::paint(QPainter *painter) {
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);""")
    replace_checked(destination / "src/qtfb/FBController.cpp", "        this->setActive(this->image != nullptr);",
        "        this->resizeViewportTexture();\n        this->setActive(this->image != nullptr);\n        if (this->image) this->forceActiveFocus();")
    replace_checked(destination / "src/qtfb/fbmanagement.cpp", "RMPPPURE_HEIGHT", "RMPPURE_HEIGHT")
    patch_root = Path(__file__).resolve().parents[1] / "packaging/appload"
    old_keys = """void FBController::keyPressEvent(QKeyEvent *ke) {
    int k = translateKey(ke->key());
    if(k != -1)
        specialKeyDown(k);
}

void FBController::keyReleaseEvent(QKeyEvent *ke) {
    int k = translateKey(ke->key());
    if(k != -1)
        specialKeyUp(k);
}"""
    replace_checked(destination / "src/qtfb/FBController.cpp", old_keys,
                    (patch_root / "pc-keyboard-forwarding.inc").read_text())
    replace_checked(destination / "shim/src/input-shim.cpp", "int mapAsciiToX11Key(int ascii) {",
                    (patch_root / "pc-evdev-keymap.inc").read_text() + "\nint mapAsciiToX11Key(int ascii) {\n    if (getenv(\"REPAPER_PC_EMULATOR\")) return mapAsciiToLinuxKey(ascii);")
    replace_checked(destination / "shim/src/input-shim.cpp",
                    '            CERR << "[QTFB SHIM INPUT]: " << (int) message.userInput.inputType << ", " << message.userInput.x << ", " << message.userInput.y << " (Translated to " << xTranslate << ", " << yTranslate << ")" << std::endl;',
                    '            // Do not log input values: physical keyboard events may contain secrets.')
    replace_checked(destination / "shim/src/fb-shim.cpp", '#define FILE_FB "/dev/fb0"',
        '#define FILE_FB (getenv("QTFB_SHIM_FB_PATH") ? getenv("QTFB_SHIM_FB_PATH") : "/dev/fb0")')
    replace_checked(destination / "shim/src/shim.cpp", 'pathVirtualKeyboard = "/dev/input/virtual_keyboard";',
        'pathVirtualKeyboard = getenv("QTFB_SHIM_INPUT_PATH_KEYS") ? getenv("QTFB_SHIM_INPUT_PATH_KEYS") : "/dev/input/virtual_keyboard";')
    replace_checked(destination / "src/qtfb/FBController.cpp", "void FBController::mouseEvent(QMouseEvent *me, int inputType) {",
        'void FBController::mouseEvent(QMouseEvent *me, int inputType) {\n    if (getenv("REPAPER_EMULATOR_CLICK")) QDEBUG << "[PC input] canvas mouse " << inputType << " at " << me->position();')
    # In the PC harness a mouse acts as a finger; physical stylus pressure remains device-only.
    for phase in ("PRESS", "UPDATE", "RELEASE"):
        replace_checked(destination / "src/qtfb/FBController.cpp", f"mouseEvent(me, INPUT_PEN_{phase});", f"mouseEvent(me, INPUT_TOUCH_{phase});")
    replace_checked(destination / "src/qtfb/FBController.cpp", "void FBController::mousePressEvent(QMouseEvent *me) {",
                    "void FBController::mousePressEvent(QMouseEvent *me) {\n    forceActiveFocus();")
    replace_checked(destination / "resources/qml/appload.qml", "            for(let i of library.applications) {",
        "            const installed = library.applications;\n            for(let index = 0; index < installed.length; ++index) {\n                const i = installed[index];")
    replace_checked(destination / "src/main.cpp", "#include <QGuiApplication>", "#include <QGuiApplication>\n#include <QQuickWindow>\n#include <QTimer>\n#include <QMouseEvent>\n#include <QKeyEvent>")
    replace_checked(destination / "src/main.cpp", "    return a.exec();", """    const bool keyboardQa = qEnvironmentVariable("REPAPER_EMULATOR_KEYBOARD_QA") == "1";
    const QString screenshot = qEnvironmentVariable("REPAPER_EMULATOR_SCREENSHOT");
    if (!screenshot.isEmpty()) QTimer::singleShot(keyboardQa ? 5500 : 3000, &a, [&]() {
        auto *window = engine.rootObjects().isEmpty() ? nullptr : qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        const bool saved = window && window->grabWindow().save(screenshot);
        a.exit(saved ? 0 : 2);
    });
    if (keyboardQa) for (int delay : {3000, 4200}) QTimer::singleShot(delay, &a, [&]() {
        auto *window = engine.rootObjects().isEmpty() ? nullptr : qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        if (!window) return;
        QKeyEvent ctrlDown(QEvent::KeyPress, Qt::Key_Control, Qt::ControlModifier);
        QKeyEvent vDown(QEvent::KeyPress, Qt::Key_V, Qt::ControlModifier);
        QKeyEvent vUp(QEvent::KeyRelease, Qt::Key_V, Qt::ControlModifier);
        QKeyEvent ctrlUp(QEvent::KeyRelease, Qt::Key_Control, Qt::NoModifier);
        for (QKeyEvent *event : {&ctrlDown, &vDown, &vUp, &ctrlUp}) QCoreApplication::sendEvent(window, event);
    });
    const QStringList click = qEnvironmentVariable("REPAPER_EMULATOR_CLICK").split(',');
    if (click.size() == 2) {
        const QPointF point(click[0].toDouble(), click[1].toDouble());
        QTimer::singleShot(1500, &a, [&, point]() {
            auto *window = engine.rootObjects().isEmpty() ? nullptr : qobject_cast<QQuickWindow *>(engine.rootObjects().first());
            if (!window) return;
            QMouseEvent press(QEvent::MouseButtonPress, point, point, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(window, &press);
        });
        QTimer::singleShot(1501, &a, [&, point]() {
            auto *window = engine.rootObjects().isEmpty() ? nullptr : qobject_cast<QQuickWindow *>(engine.rootObjects().first());
            if (!window) return;
            QMouseEvent release(QEvent::MouseButtonRelease, point, point, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QCoreApplication::sendEvent(window, &release);
        });
    }
    return a.exec();""")
    replace_checked(destination / "_start.qml", "width: 1620 / 4", "width: 1620 / 2")
    replace_checked(destination / "_start.qml", "height: 2160 / 4", "height: 2160 / 2")


def setup(args):
    root = Path(__file__).resolve().parents[1]
    state = args.state_dir.resolve()
    state.mkdir(parents=True, exist_ok=True)
    source = state / "source"
    prepare_source(root / ".tools/rm-appload", source)
    build = state / "build"
    build.mkdir(exist_ok=True)
    run(["qmake6", source / "appload.pro", "CONFIG+=c++17"], cwd=build)
    run(["make", "-j", str(args.jobs)], cwd=build)
    run(["cmake", "-S", source / "shim", "-B", state / "shim-build", "-DCMAKE_BUILD_TYPE=Release"])
    run(["cmake", "--build", state / "shim-build", "-j", str(args.jobs)])
    run(["g++", "-std=c++17", "-O2", "-pthread", "-I", source / "backends/qtfb-clients/cpp",
         root / "packaging/appload/qt-linuxfb-refresh.cpp", source / "backends/qtfb-clients/cpp/qtfb-client.cpp",
         "-o", state / "qt-linuxfb-refresh"])
    runtime = state / "runtime"
    runtime.mkdir(exist_ok=True)
    shutil.copy2(source / "_start.qml", runtime / "_start.qml")
    (runtime / "testing_extensions").mkdir(exist_ok=True)
    manifest_root = runtime / "applications_root"
    manifest_root.mkdir(exist_ok=True)
    # An older build may still contain RMChat. Keep its registration out of the menu.
    archive_registration(state, {})
    sandbox = args.sandbox_dir.resolve()
    manifests = {identifier: manifest for identifier in NAMES
                 if (manifest := app_manifest(state, sandbox, args.app_build, identifier)) is not None}
    publish_manifests(state, manifests)
    registered = list(manifests)
    metadata = {"appload_commit": APPLOAD_COMMIT, "reviewed_pr59_commit": PR59_COMMIT,
                "pr59_changes": "Xochitl 3.28 hooks only; not applied to PC emulator", "apps": registered,
                "mode": "qtfb-linuxfb", "tablet_writes": False, "sandbox": str(sandbox),
                "qtfb_socket": str(state / "runtime/qtfb.sock"), "display_version": DISPLAY_VERSION,
                "profile_version": PROFILE_VERSION,
                "bridge_binary": str((args.app_build / "bridge/paper-bridge").resolve())}
    atomic_json(state / "build-info.json", metadata)
    print(f"AppLoad PC emulator built. Registered: {', '.join(registered) or '(none)'}")
    print("Current mode: native Qt applications run through the AppLoad QTFB framebuffer.")


def launch(args):
    require_archive_opt_in(args)
    state = args.state_dir.resolve()
    binary = state / "build/appload"
    if not binary.is_file():
        raise RuntimeError("Run setup first.")
    info = json.loads((state / "build-info.json").read_text())
    if not profile_is_current(state, info):
        raise RuntimeError("Rebuild this emulator profile with setup to enable isolated sockets, readable scaling and keyboard forwarding.")
    if args.app not in ARCHIVED_NAMES:
        info = archive_registration(state, info)
    environment = os.environ.copy()
    environment.update(QT_QPA_PLATFORM="xcb", QT_QUICK_BACKEND="software")
    # Only the child apps use this scale. Scaling AppLoad itself would also enlarge its PC window.
    environment["REPAPER_EMULATOR_UI_SCALE"] = str(args.ui_scale)
    if args.screenshot:
        environment["REPAPER_EMULATOR_SCREENSHOT"] = str(args.screenshot.resolve())
    if args.click:
        environment["REPAPER_EMULATOR_CLICK"] = args.click
        environment["QT_LOGGING_RULES"] = "qt.qpa.input=true"
    command = [binary]
    if args.app:
        manifest = state / "runtime/applications_root" / args.app / "external.manifest.json"
        if not manifest.is_file():
            raise RuntimeError(f"{args.app} has no registered executable; use register --app {args.app} --app-build with its build directory.")
        command.append("external::" + args.app)
    sandbox = Path(info["sandbox"])
    sandbox.mkdir(parents=True, exist_ok=True)
    (sandbox / "state/inputs").mkdir(parents=True, exist_ok=True)
    bridge_binary = Path(info["bridge_binary"])
    bridge = None
    probe = socket.socket(socket.AF_UNIX)
    try:
        probe.connect(str(sandbox / "core.sock"))
        bridge_ready = True
    except OSError:
        bridge_ready = False
    finally:
        probe.close()
    log = (state / "emulator.log").open("w")
    if not bridge_ready and bridge_binary.is_file():
        bridge = subprocess.Popen([str(bridge_binary), "--sandbox", str(sandbox), "--socket", str(sandbox / "core.sock")], stdout=log, stderr=subprocess.STDOUT)
        for _ in range(100):
            check = socket.socket(socket.AF_UNIX)
            try:
                check.connect(str(sandbox / "core.sock"))
                bridge_ready = True
                break
            except OSError:
                if bridge.poll() is not None: break
                time.sleep(0.05)
            finally:
                check.close()
    if not bridge_ready:
        print("Paper Bridge sandbox unavailable: native note/import actions will show an explicit error.", file=sys.stderr)
    process = subprocess.Popen([str(part) for part in command], cwd=state / "runtime", env=environment, start_new_session=True, stdout=log, stderr=subprocess.STDOUT)
    try:
        result = process.wait()
        if result:
            raise RuntimeError(f"AppLoad exited with code {result}")
    finally:
        # Only terminate this launch's process group, including its framebuffer children.
        try: os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError: pass
        if bridge:
            bridge.terminate()
            try: bridge.wait(timeout=3)
            except subprocess.TimeoutExpired: bridge.kill(); bridge.wait()
        log.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--state-dir", type=Path, default=Path.home() / ".local/share/repaper-emulator")
    parser.add_argument("--sandbox-dir", type=Path, default=Path.home() / "repaper-emulator")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("check-available", help="Check the profile lock without launching or rebuilding anything.")
    setup_parser = sub.add_parser("setup")
    setup_parser.add_argument("--app-build", required=True, type=Path)
    setup_parser.add_argument("--jobs", type=int, default=4)
    archive_parser = sub.add_parser("archive", help="Hide an archived app's registration, retaining its manifest and all user data (close AppLoad first).")
    archive_parser.add_argument("--app", required=True, choices=list(ARCHIVED_NAMES))
    register_parser = sub.add_parser("register", help="Register ready native binaries without rebuilding or launching AppLoad (close its window first).")
    register_parser.add_argument("--app-build", required=True, type=Path)
    register_parser.add_argument("--app", choices=list(ALL_NAMES), help="Register one application; otherwise update the five supported apps only.")
    register_parser.add_argument("--allow-archived", action="store_true", help="Explicitly opt into registering an archived app selected with --app.")
    launch_parser = sub.add_parser("launch")
    launch_parser.add_argument("--app", choices=list(ALL_NAMES))
    launch_parser.add_argument("--allow-archived", action="store_true", help="Explicitly opt into launching an archived app selected with --app.")
    launch_parser.add_argument("--screenshot", type=Path)
    launch_parser.add_argument("--click", help="Inject a mouse press/release at emulator X,Y for validation.")
    launch_parser.add_argument("--ui-scale", type=ui_scale, default=1.5,
                               help="Native app UI scale, 1 to 2 (default 1.5; 936×1248 logical pixels).")
    args = parser.parse_args()
    if not sys.platform.startswith("linux"):
        parser.error("Run this tool in Linux/WSL with Qt 6 development packages installed.")
    try:
        with exclusive_instance(args.state_dir):
            if args.command == "setup":
                setup(args)
            elif args.command == "register":
                register(args)
            elif args.command == "archive":
                archive(args)
            elif args.command == "launch":
                launch(args)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
