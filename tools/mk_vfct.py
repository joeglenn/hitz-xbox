#!/usr/bin/env python3
"""Build a guest VFCT ACPI table around the 8060S VBIOS for QEMU -acpitable.

The Windows AMD driver on APUs takes the VBIOS from ACPI VFCT, matched by the
PCI bus/device/function in the image header. Point it at the GPU's address in
the guest (00:05.0 in the dockur VM) instead of the host's (bc:00.0).
usage: mk_vfct.py vbios.rom out.bin [bus dev fn]

No functions: a short, one-shot script run by tools/win11-gpu.sh's comments
(see there) to produce vfct_guest.bin ahead of time. VEN/DID/SSVID/SSID/REV
are the PCI vendor/device/subsystem/revision ids QEMU reports for the
emulated GPU device, and must match what the guest driver expects to find
in the VFCT entry for the VBIOS to be accepted.
"""
import struct, sys

rom = open(sys.argv[1], "rb").read()
out = sys.argv[2]
bus, dev, fn = (int(x, 0) for x in sys.argv[3:6]) if len(sys.argv) >= 6 else (0, 5, 0)
assert rom[:2] == b"\x55\xaa", "not a PCI option ROM"

VEN, DID, SSVID, SSID, REV = 0x1002, 0x1586, 0x1F4C, 0xB026, 0xC1
# VFCT "image" header: identifies which PCI device this VBIOS blob belongs
# to, followed immediately by the raw VBIOS bytes themselves (rom).
img_hdr = struct.pack("<IIIHHHHII", bus, dev, fn, VEN, DID, SSVID, SSID, REV, len(rom))
body = bytes(16) + struct.pack("<II", 0x4C, 0) + bytes(16)       # UUID, VBIOSImageOffset, Lib1, reserved
assert 36 + len(body) == 0x4C
payload = body + img_hdr + rom
length = 36 + len(payload)
# Generic ACPI table header (signature, length, revision, checksum byte,
# OEM id/table id/revision, creator id/revision); checksum is patched below.
hdr = bytearray(struct.pack("<4sIBB6s8sI4sI", b"VFCT", length, 1, 0, b"AMD   ", b"HITZVM  ", 1, b"QEMU", 1))
table = hdr + payload
# ACPI tables must sum to 0 mod 256 over all bytes. Byte 9 is the checksum
# field (the second "B" in the pack format above, packed as 0 so far); set
# it to the value that makes the whole table's byte sum land on 0.
table[9] = (-sum(table)) & 0xFF
assert sum(table) & 0xFF == 0
open(out, "wb").write(table)
print("VFCT %d bytes for %02x:%02x.%x" % (len(table), bus, dev, fn))
