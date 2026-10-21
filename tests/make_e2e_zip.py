"""Builds a game-like zip for the end-to-end run and records what it should contain.
    python tests/make_e2e_zip.py WORKDIR"""
import hashlib, json, os, random, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run_tests import blob, write_zip

work = sys.argv[1]
big = "--big" in sys.argv
os.makedirs(work, exist_ok=True)
r = random.Random(7)
entries, expect = [("Demo Game/", None, 0, 0)], {}
if big:   # ~2 GB of incompressible data, so the run lasts long enough to pause and stop
    for i in range(8):
        data = b"".join(r.randbytes(16 << 20) for _ in range(16))
        name = "Demo Game/data/big-%d.bin" % i
        entries.append((name, data, 0, 0))
        expect[name] = hashlib.sha256(data).hexdigest()
for i in range(40):
    n = r.choice([64 << 10, 1 << 20, 6 << 20, 24 << 20])
    data = blob(n, 100 + i) if i % 3 else r.randbytes(n)
    name = "Demo Game/data/pack-%03d.bin" % i
    entries.append((name, data, (0, 93, 8)[i % 3], 0))
    expect[name] = hashlib.sha256(data).hexdigest()
for name, data in (("Demo Game/Demo.exe", b"MZ" + bytes(2000)), ("Demo Game/readme.txt", b"thanks for playing\r\n")):
    entries.append((name, data, 8, 0))
    expect[name] = hashlib.sha256(data).hexdigest()
write_zip(os.path.join(work, "Demo-Game.zip"), entries)
json.dump(expect, open(os.path.join(work, "expect.json"), "w"))
print(os.path.getsize(os.path.join(work, "Demo-Game.zip")), "bytes,", len(expect), "files")
