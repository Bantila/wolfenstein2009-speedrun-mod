"""Static RE helpers: string search, xrefs (push imm32 / mov imm32), disassembly."""
import sys, struct, re
import pefile, capstone

class PE:
    def __init__(self, path):
        self.pe = pefile.PE(path, fast_load=True)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.img = self.pe.get_memory_mapped_image()
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.text = [s for s in self.pe.sections if s.Characteristics & 0x20000000]

    def str_va(self, s, exact=True):
        pat = (b'\0' if exact else b'') + s.encode() + b'\0'
        out, i = [], 0
        while (i := self.img.find(pat, i)) != -1:
            out.append(self.base + i + (1 if exact else 0)); i += 1
        return out

    def xrefs(self, va):
        needle = struct.pack('<I', va)
        out = []
        for s in self.text:
            lo, hi = s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize
            i = lo
            while (i := self.img.find(needle, i, hi)) != -1:
                out.append(self.base + i); i += 1
        return out

    def dis(self, va, n=40, back=0):
        off = va - self.base - back
        for ins in self.md.disasm(self.img[off:off + n * 8], self.base + off):
            print(f'{ins.address:08x}: {ins.mnemonic} {ins.op_str}')
            n -= 1
            if n <= 0: break

    def u32(self, va):
        return struct.unpack_from('<I', self.img, va - self.base)[0]

    def cstr(self, va):
        off = va - self.base
        return self.img[off:self.img.index(b'\0', off)].decode('latin1')

def func_start(p, va):
    off = va - p.base
    while not (p.img[off-1] == 0xCC and p.img[off-2] == 0xCC) and not (p.img[off-1] == 0xC3 and p.img[off-2] in (0xCC, 0x5d, 0x5e, 0x5f, 0x5b)):
        off -= 1
    return p.base + off

def callers(p, tgt):
    out = []
    for t in p.text:
        lo, hi = t.VirtualAddress, t.VirtualAddress + t.Misc_VirtualSize
        for i in range(lo, hi - 5):
            if p.img[i] == 0xE8 and p.base + i + 5 + struct.unpack_from('<i', p.img, i + 1)[0] == tgt:
                out.append(p.base + i)
    return out
