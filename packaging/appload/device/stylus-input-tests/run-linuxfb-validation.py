#!/usr/bin/env python3
"""Run the actual ARM shim and LinuxFB tablet plugin in private offline namespaces."""
import argparse
import hashlib
import json
import mmap
import os
from pathlib import Path
import socket
import struct
import subprocess
import threading
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--client', type=Path, required=True)
parser.add_argument('--shim', type=Path, required=True)
parser.add_argument('--stage', type=Path, required=True)
parser.add_argument('--generic-plugins', type=Path,
                    help='Directory containing rebuilt generic plugins to load before the stage plugins')
parser.add_argument('--sdk-root', type=Path, default=Path('/opt/repaper-sdk/5.8.203'))
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--packets', type=Path, help='QTFB packets captured from the real FBController suite')
args = parser.parse_args()
assert os.readlink('/proc/self/ns/mnt') != os.readlink('/proc/1/ns/mnt'), 'Use unshare -m -n --propagation private'
assert os.readlink('/proc/self/ns/net') != os.readlink('/proc/1/ns/net'), 'Use unshare -m -n --propagation private'
for target in ['/tmp', '/dev/shm']:
    subprocess.run(['/bin/mount', '-t', 'tmpfs', '-o', 'mode=1777,size=32m', 'tmpfs', target], check=True)
assert not Path('/tmp/qtfb.sock').exists()
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=False)
private = Path('/tmp/repaper-stylus-input')
private.mkdir(mode=0o700)
fds = {name: os.open(private / name, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
       for name in ['touch', 'pen', 'keys', 'buttons', 'null', 'framebuffer']}
alias = lambda name: f'/dev/fd/{fds[name]}'
runtime = args.stage.resolve() / 'runtime'
tablet_directory = args.generic_plugins.resolve() if args.generic_plugins else runtime / 'plugins/generic'
tablet_candidates = [tablet_directory / name for name in ['libqevdevtablet.so', 'libqevdevtabletplugin.so']
                     if (tablet_directory / name).is_file()]
assert len(tablet_candidates) == 1, f'Expected one tablet plugin in {tablet_directory}'
tablet_plugin = tablet_candidates[0]
plugin_roots = [runtime / 'plugins']
if args.generic_plugins:
    # QT_PLUGIN_PATH expects a root containing generic/, so create a private
    # root that overrides only the rebuilt plugin without changing the stage.
    override_root = private / 'plugin-override'
    override_root.mkdir(mode=0o700)
    (override_root / 'generic').symlink_to(args.generic_plugins.resolve(), target_is_directory=True)
    plugin_roots.insert(0, override_root)
sdk = args.sdk_root.resolve() / 'sysroots/cortexa53-crypto-remarkable-linux'
qemu = args.sdk_root.resolve() / 'sysroots/x86_64-codexsdk-linux/usr/bin/qemu-aarch64'
env = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'TZ': 'UTC'}
for key in ['HOME', 'XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_CACHE_HOME', 'XDG_RUNTIME_DIR']:
    path = private / key.lower()
    path.mkdir(mode=0o700)
    env[key] = str(path)
env.update({
    'QTFB_KEY': '4567', 'QT_PLUGIN_PATH': ':'.join(str(path) for path in plugin_roots),
    'QT_QPA_PLATFORM_PLUGIN_PATH': str(runtime / 'plugins/platforms'),
    'QT_QPA_FONTDIR': str(runtime / 'fonts'), 'FONTCONFIG_FILE': str(runtime / 'fonts/fonts.conf'),
    'QML_IMPORT_PATH': str(runtime / 'qml'), 'QML2_IMPORT_PATH': str(runtime / 'qml'),
    'QT_QPA_PLATFORM': f'linuxfb:fb={private / "framebuffer"}:tty={private / "null"}:nographicsmodeswitch',
    'QT_QPA_FB_NO_LIBINPUT': '1', 'QT_QPA_FB_DISABLE_INPUT': '1',
    'QT_QPA_GENERIC_PLUGINS': f'evdevtouch:{alias("touch")},evdevkeyboard:{alias("keys")},evdevtablet:{alias("pen")}',
    'QT_QPA_EVDEV_TOUCHSCREEN_PARAMETERS': alias('touch'), 'QT_QPA_EVDEV_KEYBOARD_PARAMETERS': alias('keys'),
    'QT_QPA_EVDEV_TABLET_PARAMETERS': alias('pen'), 'QT_QPA_PRESERVE_CONSOLE_STATE': '1', 'QT_QPA_NO_SIGNAL_HANDLER': '1',
    'QT_QPA_FB_HIDECURSOR': '1', 'QT_QUICK_BACKEND': 'software', 'QT_QUICK_CONTROLS_STYLE': 'Basic',
    'QT_SCALE_FACTOR': '2', 'QT_FONT_DPI': '96', 'QTFB_SHIM_MODEL': 'RMPP', 'QTFB_SHIM_MODE': 'RGB565',
    'QTFB_SHIM_INPUT_MODE': 'RMPP', 'QTFB_SHIM_INPUT': '1', 'QTFB_SHIM_FB': '1',
    'QTFB_SHIM_FB_PATH': str(private / 'framebuffer'),
    'QTFB_SHIM_INPUT_PATH_TOUCHSCREEN': str(private / 'touch'), 'QTFB_SHIM_INPUT_PATH_DIGITIZER': str(private / 'pen'),
    'QTFB_SHIM_INPUT_PATH_KEYS': str(private / 'keys'), 'QTFB_SHIM_INPUT_PATH_BUTTONS': str(private / 'buttons'),
    'QTFB_SHIM_INPUT_PATH_NULL': str(private / 'null'), 'QTFB_SHIM_INITIAL_DISPLAY_MODE': 'UI',
    'QT_DEBUG_PLUGINS': '1',
    'REPAPER_APPLOAD_TABLET_INPUT': '1',
})
size = 1620 * 2160 * 2
shm_fd = os.open('/dev/shm/qtfb_998877', os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
os.ftruncate(shm_fd, size)
memory = mmap.mmap(shm_fd, size)
os.close(shm_fd)
listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
listener.bind('/tmp/qtfb.sock')
listener.listen(2)
listener.settimeout(20)
errors, messages, sent = [], [], []
ready = private / 'ready'
report_path = output / 'client.json'
captured = json.loads(args.packets.read_text()) if args.packets else None
input_packets = captured['packets'] if captured else [
    {'type': kind, 'devId': 0, 'x': 400, 'y': 300, 'pressure': pressure}
    for kind, pressure in [(0x20, 50), (0x22, 75), (0x21, 0), (0x10, 0), (0x11, 0)]]
expected_types = [0x20, 0x22, 0x21, 0x10, 0x11]
if captured and captured.get('expectScroll'):
    expected_types += [0x20, 0x22, 0x22, 0x22, 0x22, 0x21]
    env['REPAPER_STYLUS_EXPECT_SCROLL'] = '1'
assert [packet['type'] for packet in input_packets] == expected_types

def serve():
    try:
        peer, _ = listener.accept()
        with peer:
            peer.settimeout(8)
            first = peer.recv(128)
            assert len(first) == 24 and first[0] == 0 and struct.unpack_from('=i', first, 4)[0] == 4567 and first[8] == 3, first.hex()
            messages.append({'type': first[0], 'length': len(first), 'key': 4567, 'format': first[8]})
            response = bytearray(32)
            struct.pack_into('=i', response, 8, 998877)
            struct.pack_into('=Q', response, 16, size)
            peer.send(response)
            state = bytearray(32)
            state[0] = 8
            peer.send(state)
            deadline = time.monotonic() + 15
            while not ready.exists():
                if time.monotonic() > deadline:
                    raise RuntimeError('Client did not become ready')
                time.sleep(.025)
            # Physical framebuffer position (400,300) is logical (200,150) at scale 2.
            # First click is genuine tablet input with a pressure update; second is touch.
            for contents in input_packets:
                packet = bytearray(32)
                packet[0] = 4
                struct.pack_into('=iiiii', packet, 8, contents['type'], contents['devId'], contents['x'], contents['y'], contents['pressure'])
                peer.send(packet)
                sent.append(contents)
                time.sleep(.15)
            while True:
                packet = peer.recv(128)
                if not packet:
                    break
                messages.append({'type': packet[0], 'length': len(packet)})
    except Exception as error:
        errors.append(repr(error))

server = threading.Thread(target=serve, daemon=True)
server.start()
command = [str(qemu), '-L', str(sdk), '-E', f'LD_LIBRARY_PATH={runtime / "lib"}',
           '-E', f'LD_PRELOAD={args.shim.resolve()}', str(args.client.resolve()), str(ready), str(report_path)]
started = time.monotonic()
with (output / 'startup.log').open('wb') as log:
    result = subprocess.run(command, env=env, stdin=fds['null'], stdout=log, stderr=subprocess.STDOUT,
                            pass_fds=(fds['touch'], fds['pen'], fds['keys']), timeout=35)
server.join(timeout=2)
listener.close()
client = json.loads(report_path.read_text()) if report_path.exists() else {}
events = client.get('tabletEvents', [])
pen_packets = [packet for packet in input_packets if packet['type'] in [0x20, 0x22, 0x21]]
pressure_ok = len(events) == len(pen_packets) and all(
    abs(event['pressure'] - packet['pressure'] / 100) < .01 for event, packet in zip(events, pen_packets))
positions_ok = len(events) == len(pen_packets) and all(
    abs(event['x'] - packet['x'] / 2) < 1 and abs(event['y'] - packet['y'] / 2) < 1
    for event, packet in zip(events, pen_packets))
report = {'status': 'PASS' if result.returncode == 0 and not errors and pressure_ok and positions_ok else 'FAIL',
          'exit_code': result.returncode, 'elapsed_seconds': round(time.monotonic() - started, 3),
          'shim_sha256': hashlib.sha256(args.shim.read_bytes()).hexdigest(), 'command': command,
          'client_sha256': hashlib.sha256(args.client.read_bytes()).hexdigest(),
          'tablet_plugin_path': str(tablet_plugin),
          'tablet_plugin_sha256': hashlib.sha256(tablet_plugin.read_bytes()).hexdigest(),
          'environment': env, 'client': client, 'pressure_normalization_ok': pressure_ok,
          'scaled_positions_ok': bool(positions_ok), 'server_errors': errors, 'messages': messages, 'sent': sent,
          'mount_namespace': os.readlink('/proc/self/ns/mnt'), 'network_namespace': os.readlink('/proc/self/ns/net')}
report['packet_origin'] = captured['origin'] if captured else 'Hand-authored QTFB protocol fixture'
report['captured_packets_sha256'] = hashlib.sha256(args.packets.read_bytes()).hexdigest() if args.packets else None
(output / 'validation.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({key: report[key] for key in ['status', 'exit_code', 'client', 'pressure_normalization_ok', 'scaled_positions_ok', 'server_errors', 'shim_sha256']}, indent=2))
raise SystemExit(0 if report['status'] == 'PASS' else 1)
