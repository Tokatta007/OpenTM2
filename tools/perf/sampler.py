"""poor man's sampling profiler: python sampler.py PID SECONDS OUT.txt
suspends each thread of the process ~1000 times a second, reads RIP; writes 'tid rva' lines (rva: from the exe's base)"""
import ctypes, ctypes.wintypes as W, sys, time

k = ctypes.WinDLL('kernel32', use_last_error=True)
pid, secs, out = int(sys.argv[1]), float(sys.argv[2]), sys.argv[3]
ALL = len(sys.argv) > 4
TH32CS_SNAPTHREAD, TH32CS_SNAPMODULE = 4, 8


class THREADENTRY32(ctypes.Structure):
    _fields_ = [('dwSize', W.DWORD), ('cntUsage', W.DWORD), ('th32ThreadID', W.DWORD), ('th32OwnerProcessID', W.DWORD),
                ('tpBasePri', W.LONG), ('tpDeltaPri', W.LONG), ('dwFlags', W.DWORD)]


class MODULEENTRY32(ctypes.Structure):
    _fields_ = [('dwSize', W.DWORD), ('th32ModuleID', W.DWORD), ('th32ProcessID', W.DWORD), ('GlblcntUsage', W.DWORD),
                ('ProccntUsage', W.DWORD), ('modBaseAddr', ctypes.c_void_p), ('modBaseSize', W.DWORD),
                ('hModule', W.HMODULE), ('szModule', ctypes.c_char * 256), ('szExePath', ctypes.c_char * 260)]


k.CreateToolhelp32Snapshot.restype = W.HANDLE
snap = k.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid)
me = MODULEENTRY32(); me.dwSize = ctypes.sizeof(me)
k.Module32First(snap, ctypes.byref(me))
mods = []
while True:
    mods.append((me.modBaseAddr, me.modBaseSize, me.szModule.decode()))
    if not k.Module32Next(snap, ctypes.byref(me)):
        break
base = mods[0][0]

snap = k.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
te = THREADENTRY32(); te.dwSize = ctypes.sizeof(te)
tids = []
k.Thread32First(snap, ctypes.byref(te))
while True:
    if te.th32OwnerProcessID == pid:
        tids.append(te.th32ThreadID)
    if not k.Thread32Next(snap, ctypes.byref(te)):
        break
k.OpenThread.restype = W.HANDLE
k.OpenProcess.restype = W.HANDLE
proc = k.OpenProcess(0x0010 | 0x0400, False, pid)
hs = [(t, k.OpenThread(0x0002 | 0x0008 | 0x0040, False, t)) for t in tids]
buf = ctypes.create_string_buffer(1232 + 16)
addr = (ctypes.addressof(buf) + 15) & ~15
ctx = (ctypes.c_char * 1232).from_address(addr)
f = open(out, 'w')
end = time.time() + secs
n = 0
while time.time() < end:
    for t, h in hs:
        if k.SuspendThread(h) == 0xFFFFFFFF:
            continue
        ctypes.memset(addr, 0, 1232)
        ctypes.c_uint32.from_address(addr + 0x30).value = 0x100001
        if k.GetThreadContext(h, ctypes.c_void_p(addr)):
            rip = ctypes.c_uint64.from_address(addr + 0xF8).value
            name = next((m[2] for m in mods if m[0] <= rip < m[0] + m[1]), '?')
            ret = ''
            if name.lower() == 'ucrtbase.dll' or ALL:
                rsp = ctypes.c_uint64.from_address(addr + 0x98).value
                stk = (ctypes.c_uint64 * 128)()
                got = ctypes.c_size_t()
                if k.ReadProcessMemory(proc, ctypes.c_void_p(rsp), stk, 1024, ctypes.byref(got)):
                    ret = ' ' + ','.join(('e%x' % (x - base)) if base <= x < base + mods[0][1] else ('m%x' % x) for x in stk)
            f.write('%d %s %x%s\n' % (t, name, rip - base if name == mods[0][2] else rip, ret))
        k.ResumeThread(h)
    n += 1
    time.sleep(0.001)
print('rounds', n, 'threads', len(hs), 'base %x' % base)
for m in mods: print('module %x %x %s' % m)
