#!/usr/bin/env python3
import struct
from pathlib import Path

# Same guest program previously represented manually in recompiled_minimal.cpp.
words = [
    0x24080028, # addiu t0, zero, 40
    0x24090002, # addiu t1, zero, 2
    0x01091021, # addu  v0, t0, t1
    0x3C0A1234, # lui   t2, 0x1234
    0x354A5678, # ori   t2, t2, 0x5678
    0x3C0B8000, # lui   t3, 0x8000
    0x356B2000, # ori   t3, t3, 0x2000
    0xAD6A0000, # sw    t2, 0(t3)
    0x8D6C0000, # lw    t4, 0(t3)
    0x118A0002, # beq   t4, t2, +2 -> 0x80010034
    0x240D0007, # delay slot: addiu t5, zero, 7
    0x24020000, # fail path: addiu v0, zero, 0
    0x0C004010, # jal   0x80010040
    0x240E0009, # delay slot: addiu t6, zero, 9
    0x03E00008, # jr    ra
    0x00000000, # nop
    0x24420001, # helper: addiu v0, v0, 1
    0x03E00008, # jr    ra
    0x00000000, # nop
]
header = bytearray(0x800)
header[0:8] = b"PS-X EXE"
struct.pack_into("<I", header, 0x10, 0x80010000) # initial PC
struct.pack_into("<I", header, 0x18, 0x80010000) # load address
struct.pack_into("<I", header, 0x1C, len(words) * 4)
out = Path(__file__).with_name("minimal.psx.exe")
out.write_bytes(header + b"".join(struct.pack("<I", w) for w in words))
print(out)
