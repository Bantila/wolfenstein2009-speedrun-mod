import ctypes, struct
from ctypes import wintypes as W
k = ctypes.WinDLL('kernel32', use_last_error=True)
k.OpenProcess.restype = W.HANDLE
k.ReadProcessMemory.argtypes = [W.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
class Proc:
    def __init__(self, pid):
        self.h = k.OpenProcess(0x0410, False, pid)
    def read(self, a, n):
        b = ctypes.create_string_buffer(n); r = ctypes.c_size_t()
        if not k.ReadProcessMemory(self.h, a, b, n, ctypes.byref(r)): return None
        return b.raw
    def u32(self, a):
        d = self.read(a, 4); return struct.unpack('<I', d)[0] if d else None
    def f3(self, a):
        d = self.read(a, 12); return struct.unpack('<3f', d) if d else None

def modules(pid):
    ps = ctypes.WinDLL('psapi')
    h = k.OpenProcess(0x0410, False, pid)
    arr = (ctypes.c_void_p * 1024)(); need = W.DWORD()
    ps.EnumProcessModulesEx(h, arr, ctypes.sizeof(arr), ctypes.byref(need), 3)
    out = {}
    for m in arr[:need.value // ctypes.sizeof(ctypes.c_void_p)]:
        name = ctypes.create_unicode_buffer(260)
        ps.GetModuleBaseNameW(h, ctypes.c_void_p(m), name, 260)
        out[name.value.lower()] = m
    return out
