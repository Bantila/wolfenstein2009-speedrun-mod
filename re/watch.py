"""Log live changes of pause-related flags in a running Wolf2.exe (for verifying offsets).
Usage: python watch.py <pid|0> <seconds> > watch.log   (0 = wait for Wolf2.exe to start)"""
import sys, time
from mem import Proc, modules

pid, secs = int(sys.argv[1]), float(sys.argv[2])
while True:  # wait for the game and its game DLL
    if pid == 0:
        import subprocess
        out = subprocess.run(['tasklist', '/fi', 'imagename eq Wolf2.exe', '/fo', 'csv', '/nh'], capture_output=True, text=True).stdout
        if 'Wolf2.exe' in out: pid = int(out.split('","')[1])
    if pid and 'gamex86.dll' in modules(pid): break
    time.sleep(1)
p = Proc(pid)
m = modules(pid)
gx = m['gamex86.dll']
gl = gx + 0x61f0b0
exe = 0x10000000

def snap():
    mc = p.u32(gx + 0x875e8c) or 0
    mp = p.u32(gl + 0x63e28)
    return {
        'cine': p.read(gl + 0x63bdc, 1),
        'camera': p.u32(gl + 0x63e74),
        'mcptr': mc,
        'mcobj': p.read(mc, 0x400) if mc else None,
        'gl_state': p.read(gl + 0x63bc0, 0x40),
        'map': (p.read(mp, 48) or b'').split(b'\0')[0] if mp else b'',
    }

prev = snap()
t0 = time.time()
print('start', {k: v for k, v in prev.items() if k not in ('mcobj', 'gl_state')}, flush=True)
while time.time() - t0 < secs:
    time.sleep(0.05)
    cur = snap()
    ts = '%7.2f' % (time.time() - t0)
    for k in ('cine', 'camera', 'mcptr', 'map'):
        if cur[k] != prev[k]:
            print(ts, k, prev[k], '->', cur[k], flush=True)
    for k, base in (('mcobj', 0), ('gl_state', 0x63bc0)):
        a, b = prev[k], cur[k]
        if a and b and len(a) == len(b) and cur['mcptr'] == prev['mcptr']:
            diffs = [(i, a[i], b[i]) for i in range(len(a)) if a[i] != b[i]]
            if 0 < len(diffs) <= 16:
                print(ts, k, ' '.join('+%x:%d->%d' % (base + i, x, y) for i, x, y in diffs), flush=True)
    prev = cur
print('end', flush=True)
