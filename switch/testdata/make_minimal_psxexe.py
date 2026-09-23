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

    # MMIO smoke: issue a blue 16x12 FillRect through GP0 at 0x1F801810.
    0x3C0F1F80, # lui   t7, 0x1F80
    0x35EF1810, # ori   t7, t7, 0x1810
    0x3C1802FF, # lui   t8, 0x02FF
    0x37180000, # ori   t8, t8, 0x0000 => 0x02FF0000 (blue)
    0xADF80000, # sw    t8, 0(t7) GP0 command
    0x3C1900DC, # lui   t9, 0x00DC
    0x37390168, # ori   t9, t9, 0x0168 => y=220,x=360
    0xADF90000, # sw    t9, 0(t7)
    0x3C08000C, # lui   t0, 0x000C
    0x35080010, # ori   t0, t0, 0x0010 => h=12,w=16
    0xADE80000, # sw    t0, 0(t7)
    0x03E00008, # jr    ra
    0x00000000, # nop

    # DMA2 MMIO smoke: RAM commands -> DMA2 registers -> GPU.
    0x3C098000, # lui   t1, 0x8000
    0x35293000, # ori   t1, t1, 0x3000
    0x3C0A02FF, # lui   t2, 0x02FF
    0x354A00FF, # ori   t2, t2, 0x00FF => magenta FillRect
    0xAD2A0000, # sw    t2, 0(t1)
    0x3C0B0104, # lui   t3, 0x0104
    0x356B01A4, # ori   t3, t3, 0x01A4 => y=260,x=420
    0xAD2B0004, # sw    t3, 4(t1)
    0x3C0C0014, # lui   t4, 0x0014
    0x358C0018, # ori   t4, t4, 0x0018 => h=20,w=24
    0xAD2C0008, # sw    t4, 8(t1)
    0x3C0D1F80, # lui   t5, 0x1F80
    0x35AD10A0, # ori   t5, t5, 0x10A0 => DMA2 MADR
    0x240E3000, # addiu t6, zero, 0x3000
    0xADAE0000, # sw    t6, 0(t5) MADR
    0x240F0003, # addiu t7, zero, 3
    0xADAF0004, # sw    t7, 4(t5) BCR
    0x3C181100, # lui   t8, 0x1100
    0x37180001, # ori   t8, t8, 1 => fromRAM + start + manual trigger
    0xADB80008, # sw    t8, 8(t5) CHCR; transfer executes synchronously
    0x03E00008, # jr    ra
    0x00000000, # nop
]
header = bytearray(0x800)
header[0:8] = b"PS-X EXE"
struct.pack_into("<I", header, 0x10, 0x80010000) # initial PC
struct.pack_into("<I", header, 0x14, 0x80018000) # initial GP
struct.pack_into("<I", header, 0x18, 0x80010000) # load address
struct.pack_into("<I", header, 0x1C, len(words) * 4)
struct.pack_into("<I", header, 0x30, 0x801FFF00) # initial SP base
struct.pack_into("<I", header, 0x34, 0x00000040) # initial SP offset
out = Path(__file__).with_name("minimal.psx.exe")
out.write_bytes(header + b"".join(struct.pack("<I", w) for w in words))
print(out)
