"""Find where Sekiro 1.06 keeps its lists of characters (ChrIns), starting from WorldChrMan.

A pointer is a ChrIns when [[p+0x1FF8]+0x68]+0x8 == p (its physics module points back at it).
This walks WorldChrMan's fields looking for arrays of such pointers, directly or one struct deeper,
and prints each candidate list with the characters' positions, HP and posture.

Usage: python chr_scan.py
"""
import struct
import sys

import sekiro_peek as sp

WCM_STATIC = 0x143D7A1E0  # sekiro.exe+0x3D7A1E0 (image base 0x140000000)


def is_chr(p, proc, cache={}):
    if p in cache:
        return cache[p]
    ok = False
    if p and 0x10000 < p < 0x7FFFFFFFFFFF and p % 8 == 0:
        modules = proc.ptr(p + 0x1FF8)
        physics = proc.ptr(modules + 0x68) if modules else None
        owner = proc.ptr(physics + 0x8) if physics else None
        ok = owner == p
    cache[p] = ok
    return ok


def describe(p, proc):
    modules = proc.ptr(p + 0x1FF8)
    physics = proc.ptr(modules + 0x68)
    data = proc.ptr(modules + 0x18)
    x, y, z = struct.unpack("<3f", proc.read(physics + 0x80, 12))
    hp, maxhp = struct.unpack("<2i", proc.read(data + 0x130, 8))
    post, maxpost = struct.unpack("<2i", proc.read(data + 0x148, 8))
    handle = struct.unpack("<I", proc.read(p + 0x8, 4))[0]
    chr_id = struct.unpack("<I", proc.read(p + 0x68, 4))[0]
    team = proc.read(p + 0x74, 1)[0]
    vtable = proc.ptr(p)
    return f"chr {p:#x} vt {vtable:#x} handle {handle:#010x} id {chr_id} team {team} pos ({x:8.2f},{y:7.2f},{z:8.2f}) hp {hp}/{maxhp} posture {post}/{maxpost}"


def array_chrs(base, proc, max_len=512):
    """Count consecutive ChrIns pointers at base (allowing a few null holes)."""
    found, holes = [], 0
    raw = proc.read(base, max_len * 8)
    if not raw:
        return found
    for i in range(max_len):
        v = struct.unpack_from("<Q", raw, i * 8)[0]
        if v == 0:
            holes += 1
            if holes > 8:
                break
            continue
        if is_chr(v, proc):
            found.append((i, v))
        else:
            # Also allow arrays of {ChrIns*, something} pairs: tolerate non-chr entries if we've seen many.
            if len(found) == 0 and i > 2:
                break
    return found


def main():
    pid = sp.find_pid("sekiro.exe")
    proc = sp.Proc(pid)
    wcm = proc.ptr(WCM_STATIC)
    hero = proc.ptr(wcm + 0x88)
    print(f"WorldChrMan {wcm:#x}, hero {hero:#x} ({'ok' if is_chr(hero, proc) else 'NOT a chr?'})")
    if hero:
        print("hero:", describe(hero, proc))
    seen = {}
    blob = proc.read(wcm, 0x4000) or b""
    for off in range(0, len(blob), 8):
        p = struct.unpack_from("<Q", blob, off)[0]
        if not (0x10000 < p < 0x7FFFFFFFFFFF):
            continue
        # direct array
        hits = array_chrs(p, proc)
        if len(hits) >= 1:
            seen.setdefault(f"[WCM+{off:#x}][i]", hits)
        # one level deeper: struct at p, pointer fields pointing to arrays
        inner = proc.read(p, 0x80) or b""
        for ioff in range(0, len(inner), 8):
            q = struct.unpack_from("<Q", inner, ioff)[0]
            if 0x10000 < q < 0x7FFFFFFFFFFF and q != p:
                hits = array_chrs(q, proc)
                if len(hits) >= 2:
                    seen.setdefault(f"[[WCM+{off:#x}]+{ioff:#x}][i]", hits)
        # inline array right at WCM+off is a pointer list itself
    for where, hits in sorted(seen.items(), key=lambda kv: -len(kv[1])):
        print(f"\n{where}: {len(hits)} characters (indices {hits[0][0]}..{hits[-1][0]})")
        for i, c in hits[:6]:
            print(f"   [{i}] {describe(c, proc)}")


if __name__ == "__main__":
    sys.exit(main())
