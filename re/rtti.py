import struct
from pe import PE
def vtables(p, cls):
    td = p.str_va('.?AV%s@@' % cls, exact=False)[0] - 8
    out = []
    for col in p.xrefs(td) if False else []: pass
    # COL lives in .rdata, search whole image
    needle = struct.pack('<I', td); i = 0
    while (i := p.img.find(needle, i)) != -1:
        col = p.base + i - 12; i += 1
        if p.u32(col) != 0: continue
        n = struct.pack('<I', col); j = 0
        while (j := p.img.find(n, j)) != -1:
            out.append((p.base + j + 4, p.u32(col + 4))); j += 1
    return out
