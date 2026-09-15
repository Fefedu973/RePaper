#!/usr/bin/env python3
"""Launcher isolation checks; no GUI, QTFB socket or tablet access."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("emulator_apps", ROOT / "tools/emulator-apps.py")
EMULATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EMULATOR)


class EmulatorProfiles(unittest.TestCase):
    def profile(self, directory):
        state = Path(directory) / "profile"
        (state / "build").mkdir(parents=True)
        (state / "build/appload").touch()
        info = {"profile_version": EMULATOR.PROFILE_VERSION, "display_version": EMULATOR.DISPLAY_VERSION,
                "qtfb_socket": str(state / "runtime/qtfb.sock"), "sandbox": str(Path(directory) / "sandbox"),
                "apps": ["remoodle"], "bridge_binary": "/existing/paper-bridge", "custom": "preserved"}
        (state / "build-info.json").write_text(json.dumps(info))
        manifest = state / "runtime/applications_root/remoodle/external.manifest.json"
        manifest.parent.mkdir(parents=True)
        manifest.write_text('{"existing":"unchanged"}\n')
        return state, info

    def executable(self, path):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("#!/bin/sh\nexit 0\n")
        path.chmod(0o700)
        return path

    def test_register_rmchat_preserves_profile_and_uses_sandbox_without_build(self):
        with tempfile.TemporaryDirectory(prefix="repaper-register-") as directory:
            state, info = self.profile(directory)
            build = Path(directory) / "app-build"
            binary = self.executable(build / "apps/rmchat/rmchat")
            core = self.executable(binary.with_name("rmchat-core"))
            args = argparse.Namespace(state_dir=state, app_build=build, app="rmchat", allow_archived=True)
            with mock.patch.object(EMULATOR, "run") as run, mock.patch.object(EMULATOR, "prepare_source") as prepare:
                with mock.patch.dict(os.environ, {"RMCHAT_TEST_SECRET": "never-copy-this-fixture"}):
                    EMULATOR.register(args)
                run.assert_not_called()
                prepare.assert_not_called()
            manifest_path = state / "runtime/applications_root/rmchat/external.manifest.json"
            manifest = json.loads(manifest_path.read_text())
            self.assertEqual(manifest["name"], "RMChat (ChatGPT)")
            self.assertEqual(manifest["args"][-1], str(binary))
            self.assertEqual(manifest["environment"], {
                "RMCHAT_CORE_PATH": str(core), "XDG_DATA_HOME": info["sandbox"] + "/state/inputs",
                "XDG_CONFIG_HOME": info["sandbox"] + "/state/config", "XDG_CACHE_HOME": info["sandbox"] + "/state/cache",
                "PAPER_BRIDGE_SOCKET": info["sandbox"] + "/core.sock"})
            self.assertNotIn("never-copy-this-fixture", manifest_path.read_text())
            self.assertEqual((state / "runtime/applications_root/remoodle/external.manifest.json").read_text(), '{"existing":"unchanged"}\n')
            updated = json.loads((state / "build-info.json").read_text())
            self.assertEqual(updated, dict(info, apps=["remoodle", "rmchat"]))
            EMULATOR.register(args)
            self.assertEqual(json.loads((state / "build-info.json").read_text())["apps"], ["remoodle", "rmchat"])

    def test_rmchat_registration_requires_both_executables_before_publication(self):
        for missing in ("rmchat", "rmchat-core", "rmchat-mode", "core-mode"):
            with self.subTest(missing=missing), tempfile.TemporaryDirectory(prefix="repaper-pair-") as directory:
                state, _ = self.profile(directory)
                before = (state / "build-info.json").read_bytes()
                build = Path(directory) / "app-build"
                binary = self.executable(build / "rmchat")
                core = self.executable(build / "rmchat-core")
                if missing == "rmchat":
                    binary.unlink()
                elif missing == "rmchat-core":
                    core.unlink()
                elif missing == "rmchat-mode":
                    binary.chmod(0o600)
                else:
                    core.chmod(0o600)
                self.assertIsNone(EMULATOR.app_manifest(state, Path(directory) / "sandbox", build, "rmchat"))
                with self.assertRaisesRegex(RuntimeError, "adjacent rmchat-core"):
                    EMULATOR.register(argparse.Namespace(state_dir=state, app_build=build, app="rmchat", allow_archived=True))
                self.assertFalse((state / "runtime/applications_root/rmchat").exists())
                self.assertEqual((state / "build-info.json").read_bytes(), before)

    def test_standalone_rmchat_build_requires_explicit_research_registration(self):
        with tempfile.TemporaryDirectory(prefix="repaper-standalone-") as directory:
            state, _ = self.profile(directory)
            build = Path(directory) / "app-build"
            binary = self.executable(build / "rmchat")
            self.executable(build / "rmchat-core")
            args = argparse.Namespace(state_dir=state, app_build=build, app=None)
            with self.assertRaisesRegex(RuntimeError, "No executable applications"):
                EMULATOR.register(args)
            args.app = "rmchat"
            with self.assertRaisesRegex(RuntimeError, "RMChat is archived"):
                EMULATOR.register(args)
            self.assertFalse((state / "runtime/applications_root/rmchat").exists())
            args.allow_archived = True
            EMULATOR.register(args)
            manifest = json.loads((state / "runtime/applications_root/rmchat/external.manifest.json").read_text())
            self.assertEqual(manifest["args"][-1], str(binary))
            self.assertEqual(json.loads((state / "build-info.json").read_text())["apps"], ["remoodle", "rmchat"])

    def test_default_registration_keeps_five_apps_and_archives_stale_rmchat_manifest(self):
        with tempfile.TemporaryDirectory(prefix="repaper-archive-") as directory:
            state, info = self.profile(directory)
            info["apps"] = list(EMULATOR.NAMES) + ["rmchat"]
            (state / "build-info.json").write_text(json.dumps(info))
            old_manifest = state / "runtime/applications_root/rmchat/external.manifest.json"
            old_manifest.parent.mkdir()
            original = b'{"historical":"manifest"}\n'
            old_manifest.write_bytes(original)
            credentials = Path(info["sandbox"]) / "state/config/rmchat/session.enc"
            credentials.parent.mkdir(parents=True)
            credentials.write_bytes(b"local-test-fixture")
            build = Path(directory) / "app-build"
            for app in EMULATOR.ALL_NAMES:
                self.executable(build / "apps" / app / app)
            self.executable(build / "apps/rmchat/rmchat-core")
            args = argparse.Namespace(state_dir=state, app_build=build, app=None)
            EMULATOR.register(args)
            self.assertEqual(list(EMULATOR.NAMES), ["remoodle", "reagenda", "restencil", "recalc", "reink"])
            for app in EMULATOR.NAMES:
                manifest = state / "runtime/applications_root" / app / "external.manifest.json"
                self.assertTrue(manifest.is_file())
            self.assertFalse(old_manifest.exists())
            archived = list((state / "archived-applications/rmchat").iterdir())
            self.assertEqual(len(archived), 1)
            self.assertEqual(archived[0].read_bytes(), original)
            self.assertEqual(credentials.read_bytes(), b"local-test-fixture")
            self.assertTrue((build / "apps/rmchat/rmchat").is_file())
            self.assertEqual(json.loads((state / "build-info.json").read_text()), dict(info, apps=list(EMULATOR.NAMES)))
            EMULATOR.register(args)
            self.assertEqual(list((state / "archived-applications/rmchat").iterdir()), archived)

    def test_archive_command_preserves_other_manifests_and_metadata(self):
        with tempfile.TemporaryDirectory(prefix="repaper-archive-command-") as directory:
            state, info = self.profile(directory)
            info["apps"].append("rmchat")
            (state / "build-info.json").write_text(json.dumps(info))
            other = state / "runtime/applications_root/remoodle/external.manifest.json"
            before = other.read_bytes()
            archived = state / "runtime/applications_root/rmchat/external.manifest.json"
            archived.parent.mkdir()
            archived.write_text('{"historical":true}\n')
            command = [sys.executable, str(ROOT / "tools/emulator-apps.py"), "--state-dir", str(state), "archive", "--app", "rmchat"]
            with EMULATOR.exclusive_instance(state):
                result = subprocess.run(command, capture_output=True, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"already running", result.stderr)
            self.assertTrue(archived.exists())
            result = subprocess.run(command, capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(other.read_bytes(), before)
            self.assertEqual(json.loads((state / "build-info.json").read_text()), dict(info, apps=["remoodle"]))
            self.assertFalse(archived.exists())

    def test_archived_launch_is_rejected_before_process_or_profile_access(self):
        with mock.patch.object(EMULATOR.subprocess, "Popen") as process:
            with self.assertRaisesRegex(RuntimeError, "RMChat is archived"):
                EMULATOR.launch(argparse.Namespace(app="rmchat"))
            process.assert_not_called()

    def test_standard_setup_and_launch_hide_a_previously_registered_rmchat(self):
        for action in ("setup", "launch"):
            with self.subTest(action=action), tempfile.TemporaryDirectory(prefix="repaper-standard-") as directory:
                state, info = self.profile(directory)
                info["apps"].append("rmchat")
                (state / "build-info.json").write_text(json.dumps(info))
                old_manifest = state / "runtime/applications_root/rmchat/external.manifest.json"
                old_manifest.parent.mkdir()
                old_manifest.write_text('{"historical":true}\n')
                build = Path(directory) / "app-build"
                for app in EMULATOR.ALL_NAMES:
                    self.executable(build / "apps" / app / app)
                self.executable(build / "apps/rmchat/rmchat-core")
                args = argparse.Namespace(state_dir=state, app_build=build, sandbox_dir=Path(info["sandbox"]),
                                          jobs=1, app=None, ui_scale=1.5, screenshot=None, click=None)
                (state / "source").mkdir()
                (state / "source/_start.qml").write_text("// fixture\n")
                with mock.patch.object(EMULATOR, "prepare_source"), mock.patch.object(EMULATOR, "run"), \
                        mock.patch.object(EMULATOR.subprocess, "Popen") as process, mock.patch.object(EMULATOR.os, "killpg"):
                    process.return_value.wait.return_value = 0
                    getattr(EMULATOR, action)(args)
                self.assertFalse(old_manifest.exists())
                self.assertEqual(len(list((state / "archived-applications/rmchat").iterdir())), 1)
                updated = json.loads((state / "build-info.json").read_text())
                self.assertNotIn("rmchat", updated["apps"])
                if action == "setup":
                    self.assertEqual(updated["apps"], list(EMULATOR.NAMES))
                else:
                    self.assertEqual(updated, dict(info, apps=["remoodle"]))

    def test_register_busy_profile_does_not_publish_or_rebuild(self):
        with tempfile.TemporaryDirectory(prefix="repaper-register-lock-") as directory:
            state, _ = self.profile(directory)
            before = (state / "build-info.json").read_bytes()
            build = Path(directory) / "app-build"
            self.executable(build / "rmchat")
            self.executable(build / "rmchat-core")
            command = [sys.executable, str(ROOT / "tools/emulator-apps.py"), "--state-dir", str(state),
                       "register", "--app", "rmchat", "--app-build", str(build)]
            with EMULATOR.exclusive_instance(state):
                result = subprocess.run(command, capture_output=True, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"already running", result.stderr)
            self.assertFalse((state / "runtime/applications_root/rmchat").exists())
            self.assertEqual((state / "build-info.json").read_bytes(), before)

    def test_atomic_manifest_failure_preserves_existing_file_and_cleans_temporary(self):
        with tempfile.TemporaryDirectory(prefix="repaper-atomic-") as directory:
            path = Path(directory) / "external.manifest.json"
            original = '{"existing":"unchanged"}\n'
            path.write_text(original)
            with mock.patch.object(EMULATOR.os, "replace", side_effect=OSError("fixture write failure")):
                with self.assertRaises(OSError):
                    EMULATOR.atomic_json(path, {"updated": True})
            self.assertEqual(path.read_text(), original)
            self.assertEqual(list(Path(directory).iterdir()), [path])

    def test_old_display_only_profile_is_rejected_before_process_launch(self):
        with tempfile.TemporaryDirectory(prefix="repaper-upgrade-") as directory:
            state = Path(directory)
            (state / "build").mkdir()
            (state / "build/appload").touch()
            old = {"display_version": 2, "qtfb_socket": str(state / "runtime/qtfb.sock")}
            (state / "build-info.json").write_text(json.dumps(old))
            with mock.patch.object(EMULATOR.subprocess, "Popen") as process:
                with self.assertRaisesRegex(RuntimeError, "keyboard forwarding"):
                    EMULATOR.launch(argparse.Namespace(state_dir=state))
                process.assert_not_called()
            current = dict(old, profile_version=EMULATOR.PROFILE_VERSION)
            self.assertTrue(EMULATOR.profile_is_current(state, current))
            self.assertFalse(EMULATOR.profile_is_current(state, dict(current, profile_version=EMULATOR.PROFILE_VERSION - 1)))
            self.assertFalse(EMULATOR.profile_is_current(state, dict(current, qtfb_socket="/tmp/another-profile.sock")))
            self.assertFalse(EMULATOR.profile_is_current(state, None))

    def test_shortcut_preflight_rejects_busy_profile_without_build_or_gui(self):
        with tempfile.TemporaryDirectory(prefix="repaper-preflight-") as directory:
            state = Path(directory)
            command = [sys.executable, str(ROOT / "tools/emulator-apps.py"), "--state-dir", str(state), "check-available"]
            with EMULATOR.exclusive_instance(state):
                result = subprocess.run(command, capture_output=True, timeout=5)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(b"already running", result.stderr)
                self.assertFalse((state / "build").exists())
            result = subprocess.run(command, capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0)
            self.assertFalse((state / "build").exists())
    def test_same_profile_is_exclusive_and_other_profile_is_independent(self):
        with tempfile.TemporaryDirectory(prefix="repaper-lock-") as directory:
            first, second = Path(directory) / "first", Path(directory) / "second"
            with EMULATOR.exclusive_instance(first):
                with self.assertRaises(RuntimeError):
                    with EMULATOR.exclusive_instance(first):
                        self.fail("Duplicate launch acquired the profile lock")
                with EMULATOR.exclusive_instance(second):
                    pass
            with EMULATOR.exclusive_instance(first):
                pass

    def test_invalid_ui_scale_is_rejected_before_device_files_are_created(self):
        for value in ("0", "3", "nan", "inf", "text"):
            with self.subTest(value=value):
                with self.assertRaises((ValueError, argparse.ArgumentTypeError)):
                    EMULATOR.ui_scale(value)
                with tempfile.TemporaryDirectory(prefix="repaper-scale-") as directory:
                    state = Path(directory) / "state"
                    environment = dict(os.environ, QTFB_KEY="123", REPAPER_EMULATOR_UI_SCALE=value)
                    result = subprocess.run([sys.executable, str(ROOT / "packaging/appload/run-native.py"),
                                             "--state", str(state), "/unused"],
                                            env=environment, capture_output=True, timeout=5)
                    self.assertEqual(result.returncode, 2)
                    self.assertFalse(state.exists())
        self.assertEqual(EMULATOR.ui_scale("1.5"), 1.5)

    def test_oversized_socket_path_is_rejected_before_source_checkout(self):
        with tempfile.TemporaryDirectory(prefix="repaper-path-") as directory:
            target = Path(directory) / ("long" * 40) / "source"
            with self.assertRaisesRegex(RuntimeError, "Unix socket"):
                EMULATOR.prepare_source(Path(directory) / "not-cloned", target)
            self.assertFalse(target.exists())


if __name__ == "__main__":
    unittest.main()
