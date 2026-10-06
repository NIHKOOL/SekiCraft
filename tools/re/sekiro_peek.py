"""Read Sekiro 1.06's player state from outside the game (reverse-engineering helper).

Pointer paths are from the community Sekiro Practice cheat table (ElaDiDu/Sekiro-Practice-CT):
  WorldChrMan        static sekiro.exe+0x3D7A1E0, AOB "48 8B C6 48 89 05 ?? ?? ?? ?? 48 85 C0" (+3: mov [rip+x], rax)
  player physics     [[[[WorldChrMan]+0x88]+0x1FF8]+0x68]
  position           physics +0x80 / +0x84 / +0x88 (floats)

Usage: python sekiro_peek.py [--watch]
"""
import ctypes
import ctypes.wintypes as wt
import struct
import sys
import time

PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
TH32CS_SNAPPROCESS = 0x2
TH32CS_SNAPMODULE = 0x8 | 0x10

k32 = ctypes.WinDLL("kernel32", use_last_error=True)


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("th32DefaultHeapID", ctypes.c_void_p), ("th32ModuleID", wt.DWORD), ("cntThreads", wt.DWORD),
                ("th32ParentProcessID", wt.DWORD), ("pcPriClassBase", ctypes.c_long), ("dwFlags", wt.DWORD),
                ("szExeFile", ctypes.c_wchar * 260)]


class MODULEENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("th32ModuleID", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("GlblcntUsage", wt.DWORD), ("ProccntUsage", wt.DWORD), ("modBaseAddr", ctypes.c_void_p),
                ("modBaseSize", wt.DWORD), ("hModule", wt.HMODULE), ("szModule", ctypes.c_wchar * 256),
                ("szExePath", ctypes.c_wchar * 260)]


k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
k32.OpenProcess.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]


def find_pid(name):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    e = PROCESSENTRY32W(dwSize=ctypes.sizeof(PROCESSENTRY32W))
    ok = k32.Process32FirstW(snap, ctypes.byref(e))
    while ok:
        if e.szExeFile.lower() == name:
            k32.CloseHandle(snap)
            return e.th32ProcessID
        ok = k32.Process32NextW(snap, ctypes.byref(e))
    k32.CloseHandle(snap)
    return None


def module_base(pid, name):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid)
    e = MODULEENTRY32W(dwSize=ctypes.sizeof(MODULEENTRY32W))
    ok = k32.Module32FirstW(snap, ctypes.byref(e))
    while ok:
        if e.szModule.lower() == name:
            k32.CloseHandle(snap)
            return e.modBaseAddr, e.modBaseSize
        ok = k32.Module32NextW(snap, ctypes.byref(e))
    k32.CloseHandle(snap)
    return None, None


class Proc:
    def __init__(self, pid):
        self.h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
        if not self.h:
            raise OSError(f"OpenProcess failed: {ctypes.get_last_error()}")

    def read(self, addr, size):
        buf = ctypes.create_string_buffer(size)
        n = ctypes.c_size_t()
        if not k32.ReadProcessMemory(self.h, ctypes.c_void_p(addr), buf, size, ctypes.byref(n)) or n.value != size:
            return None
        return buf.raw

    def ptr(self, addr):
        b = self.read(addr, 8)
        return struct.unpack("<Q", b)[0] if b else None

    def chain(self, base, *offsets):
        """[[[base]+o0]+o1]... ; returns the final address (not dereferenced) or None."""
        p = self.ptr(base)
        for o in offsets[:-1]:
            if not p:
                return None
            p = self.ptr(p + o)
        return p + offsets[-1] if p else None


def aob_find(proc, base, size, pattern):
    """First match of an IDA-style pattern in the module image."""
    want = [None if t == "??" else int(t, 16) for t in pattern.split()]
    image = bytearray()
    chunk = 1 << 20
    for off in range(0, size, chunk):
        part = proc.read(base + off, min(chunk, size - off))
        image += part if part else bytes(min(chunk, size - off))
    first = want[0]
    i = image.find(bytes([first]))
    while i != -1:
        if all(w is None or image[i + k] == w for k, w in enumerate(want)):
            return i
        i = image.find(bytes([first]), i + 1)
    return None


def main():
    pid = find_pid("sekiro.exe")
    if not pid:
        print("sekiro.exe is not running")
        return 1
    base, size = module_base(pid, "sekiro.exe")
    proc = Proc(pid)
    print(f"sekiro.exe pid {pid} base {base:#x} size {size:#x}")

    # Locate WorldChrMan by AOB (works on any version where the code is unchanged) and compare
    # with the 1.06 static offset.
    hit = aob_find(proc, base, size, "48 8B C6 48 89 05 ?? ?? ?? ?? 48 85 C0")
    if hit is None:
        print("WorldChrMan AOB not found")
        return 1
    insn = base + hit + 3                      # mov [rip+rel32], rax (7 bytes)
    rel = struct.unpack("<i", proc.read(insn + 3, 4))[0]
    wcm_static = insn + 7 + rel
    print(f"WorldChrMan static: {wcm_static:#x} (sekiro.exe+{wcm_static - base:#x}; table says +0x3d7a1e0)")

    while True:
        slot = proc.chain(wcm_static, 0x88, 0x1FF8, 0x68)  # address of the physics module pointer
        phys = proc.ptr(slot) if slot else None
        pos = proc.read(phys + 0x80, 12) if phys else None
        if pos:
            x, y, z = struct.unpack("<3f", pos)
            print(f"player pos  x {x:9.3f}  y {y:9.3f}  z {z:9.3f}   (physics {phys:#x})")
        else:
            print("no player yet (load a save)")
        if "--watch" not in sys.argv:
            return 0
        time.sleep(0.5)


if __name__ == "__main__":
    sys.exit(main())
