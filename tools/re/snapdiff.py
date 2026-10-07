"""Find a game setting by diffing memory: snapshot, change the setting in game, snapshot, compare.

Snapshots Sekiro 1.06's GameDataMan and GameMan objects and every object they point to (one level
down, 0x800 bytes each), then lists bytes that differ between two snapshots.

Usage: python snapdiff.py save NAME     -> NAME.snap
       python snapdiff.py diff A B [C]  -> bytes that differ A->B (and back again B->C, if given)
"""
import pickle
import struct
import sys

import sekiro_peek as sp

ROOTS = {"GameDataMan": 0x143D5AAC0, "GameMan": 0x143D5C0D0, "FieldArea": 0x143D5C0A0}
SIZE = 0x800


def snapshot(proc):
    out = {}
    for name, static in ROOTS.items():
        obj = proc.ptr(static)
        if not obj:
            continue
        blob = proc.read(obj, SIZE)
        if not blob:
            continue
        out[(name, "")] = (obj, blob)
        for off in range(0, SIZE, 8):
            p = struct.unpack_from("<Q", blob, off)[0]
            if 0x10000 < p < 0x7FFFFFFFFFFF and p % 8 == 0:
                sub = proc.read(p, SIZE)
                if sub:
                    out[(name, f"+{off:#x}")] = (p, sub)
    return out


def main():
    if sys.argv[1] == "save":
        proc = sp.Proc(sp.find_pid("sekiro.exe"))
        snap = snapshot(proc)
        with open(sys.argv[2] + ".snap", "wb") as f:
            pickle.dump(snap, f)
        print(f"saved {len(snap)} objects to {sys.argv[2]}.snap")
        return
    snaps = [pickle.load(open(n + ".snap", "rb")) for n in sys.argv[2:]]
    a, b = snaps[0], snaps[1]
    c = snaps[2] if len(snaps) > 2 else None
    for key in a:
        if key not in b:
            continue
        (pa, ba), (pb, bb) = a[key], b[key]
        if pa != pb:
            continue
        for i in range(min(len(ba), len(bb))):
            if ba[i] == bb[i]:
                continue
            if c is not None:
                if key not in c or c[key][0] != pa or c[key][1][i] != ba[i]:
                    continue  # must change back when the setting goes back
            name, sub = key
            print(f"[{name}]{sub} +{i:#05x}: {ba[i]:#04x} -> {bb[i]:#04x}" + (" -> back" if c is not None else ""))


if __name__ == "__main__":
    main()
