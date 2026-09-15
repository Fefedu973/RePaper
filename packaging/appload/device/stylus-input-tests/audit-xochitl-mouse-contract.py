#!/usr/bin/env python3
"""Verify the native pen-to-Qt contract in the supplied, offline Xochitl 3.28 ELF.

This checks the original firmware bytes; it does not execute or modify Xochitl.
The pinned digest prevents instruction addresses from being reused on another
firmware version. No device, input stream, or network is accessed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--xochitl', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
data = args.xochitl.read_bytes()
digest = hashlib.sha256(data).hexdigest()
assert digest == '43a9d5d0acc5b998264c16586e11b848f3b83d2d63b5fd322b09c0977d94d3d4', 'Unreviewed firmware ELF'
assert data[:6] == b'\x7fELF\x02\x01' and struct.unpack_from('<H', data, 18)[0] == 183

program_offset = struct.unpack_from('<Q', data, 32)[0]
section_offset = struct.unpack_from('<Q', data, 40)[0]
program_size, program_count, section_size, section_count = struct.unpack_from('<HHHH', data, 54)
segments = [struct.unpack_from('<IIQQQQQQ', data, program_offset + index * program_size)
            for index in range(program_count)]
sections = [struct.unpack_from('<IIQQQQIIQQ', data, section_offset + index * section_size)
            for index in range(section_count)]

def instruction(address):
    for kind, _, offset, virtual, _, file_size, _, _ in segments:
        if kind == 1 and virtual <= address < virtual + file_size:
            return struct.unpack_from('<I', data, offset + address - virtual)[0]
    raise AssertionError(f'No file-backed instruction at {address:x}')

imports = []
for section in sections:
    if section[1] != 11:  # SHT_DYNSYM
        continue
    strings = sections[section[6]]
    names = data[strings[4]:strings[4] + strings[5]]
    for offset in range(section[4], section[4] + section[5], section[9]):
        name, _, _, index, _, _ = struct.unpack_from('<IBBHQQ', data, offset)
        if index == 0 and name:
            imports.append(names[name:names.index(0, name)].decode())
mouse_imports = [name for name in imports if 'QWindowSystemInterface' in name and 'handleMouseEvent' in name]
tablet_imports = [name for name in imports if 'QWindowSystemInterface' in name and 'handleTabletEvent' in name]
assert len(mouse_imports) == 1 and not tablet_imports

expected = {
    0xa51928: 0x52800027,  # w7 = 1, button count
    0xa51934: 0x2a0703e6,  # w6 = w7, maximum point count
    0xa51938: 0x2a0703e5,  # w5 = w7, Position capability
    0xa51940: 0x52800084,  # w4 = 4, PointerType::Pen
    0xa51944: 0x52800203,  # w3 = 16, DeviceType::Stylus
    0xa51948: 0xd2952762,  # x2 = 43323, system id
    0xa51954: 0x97e8101b,  # call QPointingDevice constructor @plt 0x4559c0
    0xa51964: 0xf900f277,  # store created device at this+480
    0xa4b760: 0xf940f282,  # load same device from this+480 for mouse move
    0xa4b77c: 0x528000a7,  # QEvent::MouseMove = 5
    0xa4b788: 0xd2800000,  # window argument = nullptr
    0xa4b78c: 0x97e82691,  # call handleMouseEvent @plt 0x4551d0
    0xa4b7c4: 0xf940f282,  # load same device for press
    0xa4b7d4: 0xb9000bff,  # MouseEventNotSynthesized = 0, stack argument
    0xa4b7ec: 0x52800047,  # QEvent::MouseButtonPress = 2
    0xa4b7f0: 0xd2800000,  # window argument = nullptr
    0xa4b7f8: 0x97e82676,  # call handleMouseEvent
    0xa4c1c8: 0xf940f282,  # load same device for release
    0xa4c1d4: 0xb9000bff,  # MouseEventNotSynthesized = 0
    0xa4c1e4: 0x52800067,  # QEvent::MouseButtonRelease = 3
    0xa4c1f0: 0xd2800000,  # window argument = nullptr
    0xa4c214: 0x97e823ef,  # call handleMouseEvent
}
for address, opcode in expected.items():
    assert instruction(address) == opcode, f'Instruction mismatch at {address:x}'

def branch_target(address):
    immediate = instruction(address) & 0x3ffffff
    if immediate & 0x2000000:
        immediate -= 0x4000000
    return address + immediate * 4

assert branch_target(0xa51954) == 0x4559c0
assert all(branch_target(address) == 0x4551d0 for address in [0xa4b78c, 0xa4b7f8, 0xa4c214])
report = {
    'status': 'PASS', 'xochitl_sha256': digest,
    'method': 'Offline ELF imports and checked AArch64 call-site instructions',
    'device': {'type': 'Stylus', 'type_value': 16, 'pointer_type': 'Pen', 'pointer_type_value': 4,
               'capabilities': ['Position'], 'capabilities_value': 1, 'system_id': 43323,
               'maximum_points': 1, 'button_count': 1},
    'qt_delivery': {'api': 'QWindowSystemInterface::handleMouseEvent', 'window': None,
                    'source': 'MouseEventNotSynthesized',
                    'event_types': ['MouseButtonPress', 'MouseMove', 'MouseButtonRelease']},
    'mouse_imports': mouse_imports, 'tablet_imports': tablet_imports,
    'instruction_checks': [{'virtual_address': hex(address), 'opcode': hex(opcode)}
                           for address, opcode in expected.items()],
    'limits': ['This verifies code in the supplied firmware; it does not capture a physical pen event.',
               'The firmware mouse route exposes contact but no measured pressure.'],
}
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({key: report[key] for key in ['status', 'xochitl_sha256', 'device', 'qt_delivery']}, indent=2))
