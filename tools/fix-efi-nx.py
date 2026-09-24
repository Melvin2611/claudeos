#!/usr/bin/env python3
"""Work around a GRUB (2.12+) bug that crashes multiboot2 boots on UEFI firmware with
memory protection (e.g. current OVMF), patching the ISO in place:

1. GRUB marks the .text section of loaded modules read-only/executable through the EFI
   memory attribute protocol, but relocator.mod stores the register values for the
   final jump (grub_relocator32_eax, ...) inside its own .text section -> #PF.
   We set SHF_WRITE on the executable sections of every copy of relocator.mod, so GRUB
   maps them RWX.
2. Clear the NX_COMPAT flag of the embedded EFI images (belt and braces).
"""
import struct, sys

ISO = sys.argv[1]


def patch_elf_exec_writable(mod):
    d = bytearray(mod)
    if d[:4] != b"\x7fELF":
        return None
    is64 = d[4] == 2
    if is64:
        shoff = struct.unpack_from("<Q", d, 0x28)[0]
        shentsize, shnum = struct.unpack_from("<HH", d, 0x3A)
    else:
        shoff = struct.unpack_from("<I", d, 0x20)[0]
        shentsize, shnum = struct.unpack_from("<HH", d, 0x2E)
    for i in range(shnum):
        off = shoff + i * shentsize + 8
        if is64:
            flags = struct.unpack_from("<Q", d, off)[0]
            if flags & 4:   # SHF_EXECINSTR
                struct.pack_into("<Q", d, off, flags | 1)   # | SHF_WRITE
        else:
            flags = struct.unpack_from("<I", d, off)[0]
            if flags & 4:
                struct.pack_into("<I", d, off, flags | 1)
    return bytes(d)


with open(ISO, "rb") as f:
    data = bytearray(f.read())

count_mod = 0
for plat in ("x86_64-efi", "i386-efi"):
    try:
        orig = open("/usr/lib/grub/%s/relocator.mod" % plat, "rb").read()
    except OSError:
        continue
    new = patch_elf_exec_writable(orig)
    pos = 0
    while True:
        pos = data.find(orig, pos)
        if pos < 0:
            break
        data[pos:pos + len(orig)] = new
        count_mod += 1
        pos += len(orig)

count_nx = 0
pos = 0
while True:
    pos = data.find(b"MZ", pos)
    if pos < 0:
        break
    try:
        pe = struct.unpack_from("<I", data, pos + 0x3C)[0]
        if 0 < pe < 0x1000 and data[pos + pe:pos + pe + 4] == b"PE\0\0":
            machine = struct.unpack_from("<H", data, pos + pe + 4)[0]
            opt = pos + pe + 24
            magic = struct.unpack_from("<H", data, opt)[0]
            subsystem = struct.unpack_from("<H", data, opt + 68)[0]
            if machine in (0x8664, 0x14C) and magic in (0x10B, 0x20B) and subsystem == 10:
                dllc = struct.unpack_from("<H", data, opt + 70)[0]
                if dllc & 0x100:
                    struct.pack_into("<H", data, opt + 70, dllc & ~0x100)
                    count_nx += 1
    except struct.error:
        pass
    pos += 2

with open(ISO, "r+b") as f:
    f.write(data)
print("fix-efi: patched %d relocator module(s), %d EFI image(s)" % (count_mod, count_nx))
