#!/usr/bin/env python3
"""Host-only integration tests; Python and test stubs are not device dependencies."""
import argparse
import json
import mmap
import os
from pathlib import Path
import shutil
import signal
import socket
import struct
import subprocess
import tempfile
import time
import unittest

SOURCE = Path(__file__).resolve().parent
OPTIONS = None

PUMP_FIXTURE = r'''#!/usr/bin/python3
import json, os, pathlib, signal, sys, time
root = pathlib.Path(os.environ['REPAPER_TEST_RECORD'])
(root/'pump.json').write_text(json.dumps({'pid': os.getpid(), 'preload': os.getenv('LD_PRELOAD')}))
mode = os.getenv('REPAPER_TEST_PUMP', 'hold')
if mode == 'fail': sys.exit(9)
fd = int(sys.argv[2]); os.write(fd, b'R'); os.close(fd)
if mode == 'die': time.sleep(.3); sys.exit(9)
while True: time.sleep(1)
'''

BRIDGE_FIXTURE = r'''#!/usr/bin/python3
import json, os, pathlib, signal, subprocess, sys, time
root = pathlib.Path(os.environ['REPAPER_TEST_RECORD'])
mode = os.getenv('REPAPER_TEST_BRIDGE', 'ready')
record = {'pid': os.getpid(), 'preload': os.getenv('LD_PRELOAD'),
          'libraries': os.getenv('LD_LIBRARY_PATH'), 'key': os.getenv('QTFB_KEY')}
if mode == 'hang':
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    child = subprocess.Popen(['/bin/sleep', '60'])
    record['descendant'] = child.pid
if mode == 'service':
    # Model systemd's independent service group without touching the system manager.
    child = subprocess.Popen(['/bin/sleep', '60'], start_new_session=True,
                             stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    record['service'] = child.pid
(root/'bridge.json').write_text(json.dumps(record))
if mode == 'hang':
    while True: time.sleep(1)
if mode == 'fail': sys.exit(9)
(root/'bridge-finished').write_text('ready')
'''

APP_FIXTURE = r'''#!/usr/bin/python3
import json, os, pathlib, signal, stat, subprocess, sys, time
root = pathlib.Path(os.environ['REPAPER_TEST_RECORD'])
inputs = {}
for kind, setting in [('touch','TOUCHSCREEN'), ('pen','TABLET'), ('keys','KEYBOARD')]:
    alias = os.environ['QT_QPA_EVDEV_'+setting+'_PARAMETERS']
    fd = int(alias.rsplit('/', 1)[1]); info = os.fstat(fd)
    inputs[kind] = {'alias': alias, 'path': os.readlink('/proc/self/fd/'+str(fd)),
                    'regular': stat.S_ISREG(info.st_mode), 'mode': stat.S_IMODE(info.st_mode)}
child = subprocess.Popen(['/bin/sleep', '60'])
record = {'pid': os.getpid(), 'descendant': child.pid, 'inputs': inputs,
          'environment': dict(os.environ), 'stdin_tty': os.isatty(0),
          'stdin': os.readlink('/proc/self/fd/0'), 'bridge_completed': (root/'bridge-finished').exists()}
temporary = root/'app.tmp'; temporary.write_text(json.dumps(record)); temporary.rename(root/'app.json')
if os.getenv('REPAPER_TEST_APP') == 'ignore': signal.signal(signal.SIGTERM, signal.SIG_IGN)
if len(sys.argv) == 3 and sys.argv[1] == '--return': time.sleep(.2); sys.exit(int(sys.argv[2]))
while True: time.sleep(1)
'''

def wait_for(path, process=None, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            try: return json.loads(path.read_text())
            except json.JSONDecodeError: pass
        if process and process.poll() is not None:
            raise AssertionError('Supervisor exited before fixture was ready: ' + process.stderr.read())
        time.sleep(.02)
    raise AssertionError('Fixture did not become ready: ' + str(path))

def alive(pid):
    # A descendant zombie already exited and cannot consume input or render.
    try: return Path('/proc', str(pid), 'stat').read_text().split()[2] != 'Z'
    except FileNotFoundError: return False

class SupervisorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='repaper-runtime-tests-')
        cls.runtime = Path(cls.temp.name)/'runtime'; cls.runtime.mkdir()
        shutil.copy2(OPTIONS.build/'repaper-appload-launch', cls.runtime)
        for name, contents in [('qt-linuxfb-refresh', PUMP_FIXTURE), ('app', APP_FIXTURE),
                               ('remoodle', APP_FIXTURE), ('reagenda', APP_FIXTURE),
                               ('recalc', APP_FIXTURE), ('bridge-start', BRIDGE_FIXTURE)]:
            path = cls.runtime/name; path.write_text(contents); path.chmod(0o755)
        subprocess.run(['cc', '-shared', '-fPIC', '-x', 'c', '-', '-o', str(cls.runtime/'qtfb-shim.so')],
                       input='void repaper_test_stub(void) {}\n', text=True, check=True)

    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()

    def setUp(self):
        self.records = []
        self.processes = []
        self.services = []

    def tearDown(self):
        for process in self.processes:
            if process.poll() is None:
                process.terminate()
                try: process.wait(timeout=4)
                except subprocess.TimeoutExpired: process.kill(); process.wait()
            process.stderr.close()
        for pid in self.services:
            if alive(pid): os.kill(pid, signal.SIGKILL)
        for record in self.records: record.cleanup()

    def launch(self, extra=(), app='app', **overrides):
        directory = tempfile.TemporaryDirectory(prefix='repaper-child-test-')
        self.records.append(directory)
        env = dict(os.environ, QTFB_KEY='1234', REPAPER_TEST_RECORD=directory.name,
                   REPAPER_PC_EMULATOR='1', REPAPER_PC_HANDOFF_HELPER='/invalid/pc',
                   PAPER_BRIDGE_SOCKET='/invalid/sandbox', QT_QPA_FB_DRM='1',
                   QT_QPA_GENERIC_PLUGINS='evdevmouse:/dev/input/event0',
                   REPAPER_NATIVE_KEYBOARD_SOCKET='/private/native-keyboard.sock',
                   REPAPER_NATIVE_KEYBOARD_SESSION='synthetic-session-only',
                   REPAPER_BRIDGE_STARTER='/invalid/inherited-helper')
        env.update(overrides)
        process = subprocess.Popen([str(self.runtime/'repaper-appload-launch'), str(self.runtime/app), *extra],
                                   env=env, stderr=subprocess.PIPE, text=True)
        self.processes.append(process)
        return process, Path(directory.name)

    def assert_stopped(self, record):
        self.assertFalse(alive(record['pid']))
        self.assertFalse(alive(record['descendant']))
        self.assertFalse(Path(record['inputs']['touch']['path']).parent.exists())

    def test_private_inputs_and_environment(self):
        process, root = self.launch(('--return', '23'))
        record = wait_for(root/'app.json', process)
        pump = wait_for(root/'pump.json', process)
        env = record['environment']
        for item in record['inputs'].values():
            self.assertTrue(item['regular']); self.assertEqual(item['mode'], 0o600)
            self.assertTrue(item['alias'].startswith('/dev/fd/'))
            self.assertTrue(item['path'].startswith('/tmp/repaper-appload-'))
        directory = Path(record['inputs']['touch']['path']).parent
        self.assertEqual(directory.stat().st_mode & 0o777, 0o700)
        self.assertEqual(env['QTFB_SHIM_MODE'], 'RGB565')
        self.assertEqual(env['QT_QPA_FB_DISABLE_INPUT'], '1')
        self.assertEqual(env['QT_QPA_PRESERVE_CONSOLE_STATE'], '1')
        self.assertEqual(env['QT_QPA_NO_SIGNAL_HANDLER'], '1')
        self.assertNotIn('evdevmouse', env['QT_QPA_GENERIC_PLUGINS'])
        self.assertEqual(env['QT_QPA_PLATFORM'], 'linuxfb:fb='+str(directory/'framebuffer')
                         + ':tty='+str(directory/'null')+':nographicsmodeswitch')
        self.assertEqual(env['QTFB_SHIM_FB_PATH'], str(directory/'framebuffer'))
        self.assertEqual(env['QT_QPA_FONTDIR'], str(self.runtime/'fonts'))
        self.assertEqual(env['FONTCONFIG_FILE'], str(self.runtime/'fonts/fonts.conf'))
        self.assertEqual(env['SSL_CERT_FILE'], str(self.runtime/'certs/ca-certificates.crt'))
        self.assertEqual(env['REPAPER_APPLOAD_TABLET_INPUT'], '1')
        self.assertEqual(env['REPAPER_NATIVE_KEYBOARD_SOCKET'], '/private/native-keyboard.sock')
        self.assertEqual(env['REPAPER_NATIVE_KEYBOARD_SESSION'], 'synthetic-session-only')
        self.assertNotIn('REPAPER_BRIDGE_STARTER', env)
        self.assertFalse(record['stdin_tty']); self.assertEqual(record['stdin'], str(directory/'null'))
        for key in ('REPAPER_PC_EMULATOR', 'REPAPER_PC_HANDOFF_HELPER', 'PAPER_BRIDGE_SOCKET', 'QT_QPA_FB_DRM'):
            self.assertNotIn(key, env)
        self.assertIsNone(pump['preload'])
        self.assertEqual(process.wait(timeout=4), 23)
        self.assert_stopped(record); self.assertFalse(alive(pump['pid']))

    def test_bridge_starts_before_only_document_apps(self):
        for app in ('remoodle', 'reagenda', 'recalc'):
            process, root = self.launch(('--return', '0'), app=app)
            record = wait_for(root/'app.json', process)
            if app == 'recalc':
                self.assertFalse((root/'bridge.json').exists())
                self.assertNotIn('REPAPER_BRIDGE_STARTER', record['environment'])
            else:
                bridge = wait_for(root/'bridge.json', process)
                self.assertTrue(record['bridge_completed'])
                self.assertIsNone(bridge['preload']); self.assertIsNone(bridge['key'])
                self.assertIsNone(bridge['libraries'])
                self.assertEqual(record['environment']['REPAPER_BRIDGE_STARTER'], str(self.runtime/'bridge-start'))
                self.assertFalse(alive(bridge['pid']))
            self.assertEqual(process.wait(timeout=4), 0)
            self.assert_stopped(record)

    def test_bridge_failure_keeps_app_alive_and_retry_configured(self):
        process, root = self.launch(app='reagenda', REPAPER_TEST_BRIDGE='fail')
        record = wait_for(root/'app.json', process)
        self.assertIsNone(process.poll())
        self.assertTrue(alive(record['pid']))
        self.assertEqual(record['environment']['REPAPER_BRIDGE_STARTER'], str(self.runtime/'bridge-start'))
        process.terminate(); self.assertEqual(process.wait(timeout=4), 143)
        self.assert_stopped(record)

    def test_bridge_timeout_is_bounded_and_cleans_helper_group(self):
        start = time.monotonic()
        process, root = self.launch(app='remoodle', REPAPER_TEST_BRIDGE='hang')
        bridge = wait_for(root/'bridge.json', process)
        record = wait_for(root/'app.json', process, timeout=18)
        self.assertLess(time.monotonic()-start, 17)
        self.assertFalse(alive(bridge['pid'])); self.assertFalse(alive(bridge['descendant']))
        self.assertTrue(alive(record['pid']))
        self.assertEqual(record['environment']['REPAPER_BRIDGE_STARTER'], str(self.runtime/'bridge-start'))
        process.terminate(); self.assertEqual(process.wait(timeout=4), 143)
        self.assert_stopped(record)

    def test_sigterm_during_bridge_start_never_starts_app(self):
        process, root = self.launch(app='reagenda', REPAPER_TEST_BRIDGE='hang')
        bridge = wait_for(root/'bridge.json', process)
        start = time.monotonic(); process.terminate()
        self.assertEqual(process.wait(timeout=4), 143)
        self.assertLess(time.monotonic()-start, 3.5)
        self.assertFalse(alive(bridge['pid'])); self.assertFalse(alive(bridge['descendant']))
        self.assertFalse((root/'app.json').exists()); self.assertFalse((root/'pump.json').exists())

    def test_stopping_app_preserves_independent_bridge_service(self):
        process, root = self.launch(app='reagenda', REPAPER_TEST_BRIDGE='service')
        bridge = wait_for(root/'bridge.json', process)
        self.services.append(bridge['service'])
        record = wait_for(root/'app.json', process)
        process.terminate(); self.assertEqual(process.wait(timeout=4), 143)
        self.assert_stopped(record)
        self.assertTrue(alive(bridge['service']))

    def test_invalid_key_never_starts_children(self):
        for key in ('', '-1', '12junk', '2147483648'):
            process, root = self.launch(QTFB_KEY=key)
            self.assertEqual(process.wait(timeout=2), 1)
            self.assertFalse((root/'pump.json').exists())
            self.assertFalse((root/'app.json').exists())

    def test_refresh_start_failure_never_starts_app(self):
        process, root = self.launch(REPAPER_TEST_PUMP='fail')
        self.assertEqual(process.wait(timeout=4), 1)
        pump = wait_for(root/'pump.json')
        self.assertFalse(alive(pump['pid'])); self.assertFalse((root/'app.json').exists())

    def test_refresh_death_stops_app(self):
        process, root = self.launch(REPAPER_TEST_PUMP='die')
        record = wait_for(root/'app.json', process)
        self.assertEqual(process.wait(timeout=4), 1); self.assert_stopped(record)

    def test_sigterm_is_bounded_for_uncooperative_app(self):
        process, root = self.launch(REPAPER_TEST_APP='ignore')
        record = wait_for(root/'app.json', process)
        time.sleep(.1)  # Fixture has installed its deliberate SIGTERM ignore handler.
        start = time.monotonic(); process.terminate()
        self.assertEqual(process.wait(timeout=4), 143)
        self.assertLess(time.monotonic()-start, 3.5); self.assert_stopped(record)

    def test_concurrent_launches_have_independent_inputs_and_lifetimes(self):
        first, aroot = self.launch(); second, broot = self.launch()
        a = wait_for(aroot/'app.json', first); b = wait_for(broot/'app.json', second)
        self.assertNotEqual(a['inputs']['touch']['path'], b['inputs']['touch']['path'])
        first.terminate(); self.assertEqual(first.wait(timeout=4), 143); self.assert_stopped(a)
        self.assertIsNone(second.poll()); self.assertTrue(alive(b['pid']))
        second.send_signal(signal.SIGINT); self.assertEqual(second.wait(timeout=4), 130); self.assert_stopped(b)


class RefreshTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='rp-refresh-')
        cls.directory = Path(cls.temp.name)
        cls.address = str(cls.directory/'qtfb.sock')
        # Compile the real helper with only a private host socket substituted.
        common = (OPTIONS.appload/'src/qtfb/common.h').read_text()
        common = common.replace('#define SOCKET_PATH "/tmp/qtfb.sock"', '#define SOCKET_PATH "'+cls.address+'"')
        (cls.directory/'common.h').write_text(common)
        subprocess.run(['c++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-I', str(cls.directory),
                        str(SOURCE/'refresh.cpp'), '-o', str(cls.directory/'refresh'), '-lrt'], check=True)

    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()

    def setUp(self):
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.listener.bind(self.address); self.listener.listen(1); self.listener.settimeout(4)
        self.process = subprocess.Popen([str(self.directory/'refresh')], env=dict(os.environ, QTFB_KEY='4567'),
                                        stderr=subprocess.PIPE, text=True)
        self.peer, _ = self.listener.accept(); self.peer.settimeout(4)
        request = self.peer.recv(128)
        self.assertEqual(len(request), 24)
        self.assertEqual(request[0], 0)
        self.assertEqual(struct.unpack_from('=i', request, 4)[0], 4567)
        self.assertEqual(request[8], 3)  # FBFMT_RMPP_RGB565
        self.shm_path = None; self.mapping = None

    def tearDown(self):
        self.peer.close(); self.listener.close()
        if self.process.poll() is None: self.process.terminate(); self.process.wait(timeout=3)
        self.process.stderr.close()
        Path(self.address).unlink()
        if self.mapping: self.mapping.close()
        if self.shm_path: self.shm_path.unlink()

    def initialize(self, size=1620*2160*2):
        # O_EXCL prevents touching any existing AppLoad shared-memory region.
        for key in range(os.getpid()*1000, os.getpid()*1000+100):
            path = Path('/dev/shm')/('qtfb_'+str(key))
            try: fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600); break
            except FileExistsError: continue
        else: self.fail('No unique test shared-memory name')
        self.shm_path = path
        os.ftruncate(fd, size); self.mapping = mmap.mmap(fd, size); os.close(fd)
        response = bytearray(32)
        struct.pack_into('=i', response, 8, key); struct.pack_into('=Q', response, 16, size)
        self.peer.send(response)

    def test_updates_changed_rows_and_exits_on_disconnect(self):
        self.initialize()
        first = struct.unpack('=B3xiiiii', self.peer.recv(128))
        self.assertEqual(first, (1, 0, 0, 0, 1620, 2160))
        self.peer.settimeout(.25)
        with self.assertRaises(socket.timeout): self.peer.recv(128)
        self.mapping[123*3240:123*3240+2] = b'\xff\xff'
        self.peer.settimeout(3)
        partial = struct.unpack('=B3xiiiii', self.peer.recv(128))
        self.assertEqual(partial, (1, 1, 0, 123, 1620, 1))
        self.peer.close()
        self.assertEqual(self.process.wait(timeout=3), 1)

    def test_scroll_burst_uses_animation_then_one_quality_partial_redraw(self):
        self.initialize()
        first = struct.unpack('=B3xiiiii', self.peer.recv(128))
        self.assertEqual(first, (1, 0, 0, 0, 1620, 2160))
        modes = []
        # Consecutive changed samples simulate a scrolling framebuffer. Every
        # change waits for its real wire update before changing the next row.
        for row in range(120, 132):
            self.mapping[row*3240:row*3240+2] = b'\xff\xff'
            while True:
                message = struct.unpack('=B3xiiiii', self.peer.recv(128))
                if message[0] == 5:
                    modes.append(message[1])
                    self.assertEqual(message[1], 2)  # REFRESH_MODE_ANIMATE
                    continue
                self.assertEqual(message, (1, 1, 0, row, 1620, 1))
                break
        self.assertEqual(modes, [2])  # No repeated mode requests during motion.
        settled_mode = struct.unpack('=B3xiiiii', self.peer.recv(128))
        self.assertEqual(settled_mode[:2], (5, 3))  # REFRESH_MODE_CONTENT
        settled = struct.unpack('=B3xiiiii', self.peer.recv(128))
        self.assertEqual(settled, (1, 1, 0, 120, 1620, 12))
        self.peer.settimeout(.45)
        with self.assertRaises(socket.timeout): self.peer.recv(128)
        # No MESSAGE_REQUEST_FULL_REFRESH or UPDATE_ALL follows startup.

    def test_isolated_cursor_changes_have_no_extra_cleanup_or_fast_mode(self):
        self.initialize()
        self.assertEqual(struct.unpack('=B3xiiiii', self.peer.recv(128)), (1, 0, 0, 0, 1620, 2160))
        for value in (b'\xff\xff', b'\x00\x00', b'\xff\xff'):
            self.mapping[500*3240:500*3240+2] = value
            self.peer.settimeout(3)
            self.assertEqual(struct.unpack('=B3xiiiii', self.peer.recv(128)), (1, 1, 0, 500, 1620, 1))
            self.peer.settimeout(.45)
            with self.assertRaises(socket.timeout): self.peer.recv(128)

    def test_wrong_framebuffer_size_fails_before_mapping(self):
        self.initialize(4096)
        self.assertEqual(self.process.wait(timeout=3), 1)
        self.assertIn('incompatible RGB565 framebuffer', self.process.stderr.read())


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--appload', type=Path, required=True)
    OPTIONS, remaining = parser.parse_known_args()
    unittest.main(argv=[__file__, *remaining])
