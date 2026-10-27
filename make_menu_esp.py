#!/usr/bin/env python3
"""Gera papyrus/FreeClimbMenu.esp: a quest do menu (MCM) do FreeClimb LE e 4 registos SOUN que embrulham
os descritores de som FC_* do FreeClimb.esp. O formato copia o SoftTarget.esp (Skyrim LE, form version 43).

Mestres:  0 = Skyrim.esm   1 = FreeClimb.esp   -> os registos novos usam o indice 2 (0x02......).
"""
import struct, sys

def sub(tag, data): return tag + struct.pack('<H', len(data)) + data
def z(s): return s.encode('ascii') + b'\0'
def record(tag, formid, subs, flags=0):
    body = b''.join(subs)
    return tag + struct.pack('<IIIIHH', len(body), flags, formid, 0, 43, 0) + body
def group(label, children):
    body = b''.join(children)
    return b'GRUP' + struct.pack('<I', 24 + len(body)) + label + struct.pack('<IHHI', 0, 0, 0, 43) + body

SCRIPT = b'FreeClimbMCM'
vmad = (struct.pack('<HHH', 5, 2, 1) + struct.pack('<H', len(SCRIPT)) + SCRIPT + b'\0' + struct.pack('<H', 0)
        + b'\x02' + struct.pack('<HHH', 0, 0, 0))
quest = record(b'QUST', 0x02000800, [
    sub(b'EDID', z('FreeClimbMCMQuest')),
    sub(b'VMAD', vmad),
    sub(b'FULL', z('FreeClimb')),
    sub(b'DNAM', bytes.fromhex('110100120000000000000000')),
    sub(b'NEXT', b''),
    sub(b'ANAM', struct.pack('<I', 0)),
])

sounds = []
for i, (name, sndr) in enumerate([('FC_MenuStep', 0x01000801), ('FC_MenuGrip', 0x01000802), ('FC_MenuPush', 0x01000803), ('FC_MenuTop', 0x01000804)]):
    sounds.append(record(b'SOUN', 0x02000801 + i, [
        sub(b'EDID', z(name)),
        sub(b'OBND', struct.pack('<6h', 0, 0, 0, 0, 0, 0)),
        sub(b'SDSC', struct.pack('<I', sndr)),
    ]))

groups = [group(b'QUST', [quest]), group(b'SOUN', sounds)]
n_records = 2 + 1 + len(sounds)      # 2 grupos + quest + sons  (o SoftTarget conta grupo + registo)
n_records = len(groups) + 1 + len(sounds)

tes4 = record(b'TES4', 0, [
    sub(b'HEDR', struct.pack('<fII', 0.94, n_records, 0x805)),
    sub(b'CNAM', z('FreeClimbLE')),
    sub(b'SNAM', z('FreeClimb LE - in-game menu and sounds')),
    sub(b'MAST', z('Skyrim.esm')), sub(b'DATA', b'\0' * 8),
    sub(b'MAST', z('FreeClimb.esp')), sub(b'DATA', b'\0' * 8),
    sub(b'INTV', struct.pack('<I', 1)),
])
out = sys.argv[1] if len(sys.argv) > 1 else 'FreeClimbMenu.esp'
open(out, 'wb').write(tes4 + b''.join(groups))
print('escrito', out)
