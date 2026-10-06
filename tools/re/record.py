"""Log Wolf's physics position and nearby candidate rotation floats at 10 Hz to a CSV (RE helper)."""
import struct, sys, time, sekiro_peek as sp

pid = sp.find_pid("sekiro.exe"); p = sp.Proc(pid)
wcm = 0x143d7a1e0  # WorldChrMan static, 1.06
out = open(sys.argv[1] if len(sys.argv) > 1 else "record.csv", "w", buffering=1)
out.write("t,x,y,z,r70,r74,r78,r7c,rb0,rb4,rb8,rbc\n")
t0 = time.time()
end = t0 + float(sys.argv[2] if len(sys.argv) > 2 else 600)
while time.time() < end:
    slot = p.chain(wcm, 0x88, 0x1FF8, 0x68)
    phys = p.ptr(slot) if slot else None
    b = p.read(phys + 0x70, 0x50) if phys else None
    if b:
        r = struct.unpack_from("<4f", b, 0x00); pos = struct.unpack_from("<3f", b, 0x10); q = struct.unpack_from("<4f", b, 0x40)
        out.write(f"{time.time()-t0:.2f}," + ",".join(f"{v:.4f}" for v in pos + r + q) + "\n")
    time.sleep(0.1)
