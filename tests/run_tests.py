"""End-to-end tests for the ZeroZip engine.

Needs the `zstandard` package. Run after `build.bat test`:
    python tests/run_tests.py
"""
import ctypes, hashlib, os, random, shutil, struct, subprocess, tempfile, threading, warnings, zipfile, zlib

import zstandard

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ZZ = os.path.join(ROOT, "build", "zztest.exe")


def dos_time(t=(2024, 5, 17, 13, 45, 30)):
    y, mo, d, h, mi, s = t
    return (h << 11) | (mi << 5) | (s // 2), ((y - 1980) << 9) | (mo << 5) | d


def write_zip(path, entries):
    """Minimal zip writer so tests can use Zstandard (method 93) and odd flags.
    entries: (name, data or None for a folder, method, flags[, overrides]) where
    overrides may replace the stored "crc" or "size" to make a broken entry."""
    tm, dt = dos_time()
    cd = b""
    with open(path, "wb") as f:
        for name, data, method, flags, *rest in entries:
            over = rest[0] if rest else {}
            raw = name.encode("utf-8")
            flags |= 0x800
            data = data or b""
            if method == 8:
                c = zlib.compressobj(6, zlib.DEFLATED, -15)
                comp = c.compress(data) + c.flush()
            elif method == 93:
                comp = zstandard.ZstdCompressor(level=3).compress(data)
            else:
                comp = data
            crc = over.get("crc", zlib.crc32(data))
            size = over.get("size", len(data))
            off = f.tell()
            f.write(struct.pack("<IHHHHHIIIHH", 0x04034B50, 20, flags, method, tm, dt, crc, len(comp), size, len(raw), 0))
            f.write(raw + comp)
            attrs = 0x10 if name.endswith("/") else 0x20
            cd += struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 20, 20, flags, method, tm, dt, crc, len(comp), size,
                              len(raw), 0, 0, 0, 0, attrs, off) + raw
        cdo = f.tell()
        f.write(cd)
        f.write(struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, len(entries), len(entries), len(cd), cdo, 0))


def blob(n, seed):
    r = random.Random(seed)
    words = [bytes(r.randrange(97, 123) for _ in range(r.randrange(3, 9))) for _ in range(200)]
    out = bytearray()
    while len(out) < n:
        out += r.choice(words) + b" "
        if r.random() < 0.01:
            out += r.randbytes(64)
    return bytes(out[:n])


def noise(n, seed):
    r = random.Random(seed)
    return b"".join(r.randbytes(min(16 << 20, n - i)) for i in range(0, n, 16 << 20))


def sha(b):
    return hashlib.sha256(b).hexdigest()


def run(*args, env=None):
    p = subprocess.run([ZZ, *args], capture_output=True, text=True, encoding="utf-8", env=env)
    return p.returncode, p.stdout


def punch_env(over):
    env = dict(os.environ)
    env["ZEROZIP_TEST_PUNCH_OVER"] = str(over)
    return env


def allocated(path):
    """Bytes a file really takes on disk (smaller than its size once it is sparse)."""
    f = ctypes.windll.kernel32.GetCompressedFileSizeW
    f.restype = ctypes.c_uint32
    high = ctypes.c_uint32(0)
    low = f(ctypes.c_wchar_p(path), ctypes.byref(high))
    return (high.value << 32) | low


def expect_tree(dest, expected):
    for name, data in expected.items():
        p = os.path.join(dest, *name.split("/"))
        if data is None:
            assert os.path.isdir(p), "missing dir " + name
        else:
            assert os.path.isfile(p), "missing file " + name
            with open(p, "rb") as f:
                got = f.read()
            assert sha(got) == sha(data), "content differs: %s (%d vs %d bytes)" % (name, len(got), len(data))
    leftovers = [os.path.join(d, n) for d, _, fs in os.walk(dest) for n in fs if n.endswith(".zzpart")]
    assert not leftovers, leftovers


def main():
    work = tempfile.mkdtemp(prefix="zztest-")
    big = blob(23 << 20, 1)          # several 4 MiB chunks
    rnd = noise(9 << 20, 2)
    files = {
        "Game/": None,
        "Game/readme.txt": b"hello zerozip\r\n",
        "Game/empty.dat": b"",
        "Game/data/big-zstd.bin": big,
        "Game/data/big-deflate.bin": big[::-1],
        "Game/data/random-stored.bin": rnd,
        "Game/data/sub/": None,
        "Game/data/sub/ünïcödé ファイル.txt": "ünïcödé".encode("utf-8") * 1000,
        "Game/CON.txt": b"reserved name",
    }
    methods = {"Game/data/big-deflate.bin": 8, "Game/data/random-stored.bin": 0, "Game/readme.txt": 8, "Game/empty.dat": 8}
    entries = [(n, d, methods.get(n, 93), 0) for n, d in files.items()]
    expected = dict(files)
    expected["Game/_CON.txt"] = expected.pop("Game/CON.txt")
    passed = 0

    def fresh(name, ents):
        p = os.path.join(work, name)
        write_zip(p, ents)
        return p

    def ok(msg):
        nonlocal passed
        passed += 1
        print("ok  " + msg)

    # full run
    z = fresh("mixed.zip", entries)
    d = os.path.join(work, "out1")
    code, out = run("plan", z, d)
    assert code == 0 and "problems=0" in out, out
    code, out = run("run", z, d)
    assert code == 0 and out.strip().endswith("done"), out
    expect_tree(d, expected)
    assert not os.path.exists(z) and not os.path.exists(z + ".zerozip")
    ok("full run, mixed methods")

    # stop part way, check the zip shrank, then resume; the folder is remembered
    z = fresh("resume.zip", entries)
    size0 = os.path.getsize(z)
    d = os.path.join(work, "out2")
    code, out = run("run", z, d, "--stop-after", "3")
    assert code == 3 and "stopped" in out, out
    assert os.path.getsize(z) < size0 and os.path.exists(z + ".zerozip")
    code, out = run("plan", z)
    assert "resuming=1" in out and "problems=0" in out and "dest=%s\n" % d in out, out
    code, out = run("run", z, d)
    assert code == 0 and "done" in out, out
    expect_tree(d, expected)
    assert not os.path.exists(z)
    ok("stop and resume, folder remembered")

    # some files already extracted by another tool
    z = fresh("partial.zip", entries)
    d = os.path.join(work, "out3")
    for n in ("Game/readme.txt", "Game/data/random-stored.bin"):
        p = os.path.join(d, *n.split("/"))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, "wb").write(files[n])
    half = os.path.join(d, "Game", "data", "big-zstd.bin")       # a half-written file
    open(half, "wb").write(big[: len(big) // 2])
    code, out = run("plan", z, d)
    assert "present=2" in out, out
    code, out = run("run", z, d)
    assert code == 0 and out.count("ok   ") == 2, out
    expect_tree(d, expected)
    ok("skips files already extracted, redoes half-written ones")

    # a same-size copy with different content is replaced, never trusted
    z = fresh("corrupt.zip", entries)
    d = os.path.join(work, "out4")
    p = os.path.join(d, "Game", "readme.txt")
    os.makedirs(os.path.dirname(p), exist_ok=True)
    open(p, "wb").write(b"HELLO ZEROZIP\r\n")
    code, out = run("run", z, d)
    assert code == 0 and "wrote Game/readme.txt" in out, out
    expect_tree(d, expected)
    ok("same-size file with wrong content is replaced")

    # entries that would fail partway block the run and leave the zip alone
    for name, ents, why in (
        ("evil.zip", [("../evil.txt", b"x", 0, 0)], "unsafe path"),
        ("locked.zip", [("a.txt", b"x", 0, 1)], "password protected"),
        ("lzma.zip", [("a.txt", b"x", 14, 0)], "LZMA"),
        ("long.zip", [("d/" + "n" * 252 + ".txt", b"x", 0, 0)], "name too long"),
        ("clash.zip", [("a", b"file", 0, 0), ("a/b.txt", b"x", 0, 0)], "same name"),
        ("temp.zip", [("x.zzpart", b"x", 0, 0)], "temporary files"),
    ):
        z = fresh(name, ents)
        size0 = os.path.getsize(z)
        code, out = run("plan", z, os.path.join(work, "out5"))
        assert "problems=1" in out and why in out, out
        code, out = run("run", z, os.path.join(work, "out5"))
        assert code == 1 and os.path.getsize(z) == size0 and not os.path.exists(z + ".zerozip"), out
        assert not os.path.exists(os.path.join(work, "evil.txt"))
    ok("unsafe / unsupported / clashing names are refused untouched")

    # zip made by Python's zipfile with ZIP64 extras and a duplicate name
    z = os.path.join(work, "py64.zip")
    with zipfile.ZipFile(z, "w", zipfile.ZIP_DEFLATED) as zf:
        for n, data in files.items():
            if data is None:
                continue
            with zf.open(n, "w", force_zip64=True) as w:
                w.write(data)
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            zf.writestr("Game/readme.txt", b"second copy wins")
    d = os.path.join(work, "out6")
    code, out = run("run", z, d)
    assert code == 0 and "done" in out, out
    exp = {k: v for k, v in expected.items() if v is not None}
    exp["Game/readme.txt"] = b"second copy wins"
    expect_tree(d, exp)
    ok("zip64 extras + duplicate names (last wins)")

    # names that differ only in case both survive
    z = fresh("case.zip", [("src/README", b"upper", 0, 0), ("src/readme", b"lower", 8, 0)])
    d = os.path.join(work, "out7")
    code, out = run("run", z, d)
    assert code == 0, out
    expect_tree(d, {"src/README": b"upper", "src/readme (2)": b"lower"})
    ok("case-only name clash gets a numbered name")

    # default destination: one top folder -> next to the zip
    z = fresh("smart.zip", entries)
    code, out = run("plan", z)
    assert "dest=%s\n" % work in out and "singleRoot=1" in out, out
    z2 = fresh("loose.zip", [("a.txt", b"a", 0, 0), ("b.txt", b"b", 8, 0)])
    code, out = run("plan", z2)
    assert "dest=%s\n" % os.path.join(work, "loose") in out, out
    ok("default destination")

    # the zip is locked by another program
    z = fresh("busy.zip", entries)
    with open(z, "rb"):
        code, out = run("run", z, os.path.join(work, "out8"))
    assert code == 1 and "Another app has the zip open" in out, out
    ok("refuses while another app has the zip open")

    # resume data left by a different zip with the same name is ignored
    z = fresh("same-name.zip", entries)
    d = os.path.join(work, "out9")
    code, out = run("run", z, d, "--stop-after", "2")
    assert code == 3 and os.path.exists(z + ".zerozip"), out
    os.remove(z)
    other = {"Game/new.bin": blob(3 << 20, 9), "Game/readme.txt": b"version two\r\n"}
    write_zip(z, [(n, v, 93, 0) for n, v in other.items()])
    code, out = run("plan", z, d)
    assert "resuming=0" in out, out
    code, out = run("run", z, d)
    assert code == 0 and "done" in out, out
    expect_tree(d, other)
    ok("stale resume data from another zip is ignored")

    # a finished temp file whose rename was lost is recovered, not deleted
    z = fresh("rename.zip", entries[:-1])     # ends with a plainly named file
    d = os.path.join(work, "out10")
    code, out = run("run", z, d, "--stop-after", "1")
    last = out.split("\n")[0].split(" ", 1)[1]
    p = os.path.join(d, *last.split("/"))
    os.replace(p, p + ".zzpart")
    code, out = run("run", z, d)
    assert code == 0 and ("ok    " + last) in out, out
    expect_tree(d, {k: v for k, v in files.items() if k != "Game/CON.txt"})
    ok("temp file left by a crash is recovered")

    # an entry that decompresses to more than it claims is stopped early
    z = fresh("liar.zip", [("a.bin", blob(5 << 20, 3), 93, 0, {"size": 1 << 20})])
    size0 = os.path.getsize(z)
    code, out = run("run", z, os.path.join(work, "out11"))
    assert code == 1 and "more data than the zip says" in out and os.path.getsize(z) == size0, out
    ok("output is capped at the stored size")

    # hole-punch mode: forced for files over 1 MB, zip space freed mid-file
    huge = noise(640 << 20, 4)
    pents = entries + [("Game/data/huge.bin", huge, 0, 0)]
    pexp = dict(expected, **{"Game/data/huge.bin": huge})
    z = fresh("punch.zip", pents)
    d = os.path.join(work, "out12")
    env = punch_env(1 << 20)
    code, out = run("plan", z, d, env=env)
    assert "sparse=1" in out and "punchFiles=4" in out, out
    lowest = [os.path.getsize(z)]
    free0 = shutil.disk_usage(work).free
    lowest_free = [free0]
    done = threading.Event()

    def watch():
        while not done.is_set():
            try:
                lowest[0] = min(lowest[0], allocated(z))
            except OSError:
                pass
            lowest_free[0] = min(lowest_free[0], shutil.disk_usage(work).free)
            done.wait(0.02)
    t = threading.Thread(target=watch)
    t.start()
    code, out = run("run", z, d, env=env)
    done.set()
    t.join()
    assert code == 0 and "done" in out, out
    expect_tree(d, pexp)
    assert lowest[0] < 500 << 20, "zip space was never freed mid-file (lowest %d)" % lowest[0]
    dip = free0 - lowest_free[0]
    assert dip < 320 << 20, "free space fell by %d MB extracting a 640 MB hole-punched file" % (dip >> 20)
    ok("hole-punch mode frees zip space during a file (zip down to %d MB on disk, free space dipped %d MB)"
       % (lowest[0] >> 20, dip >> 20))

    # a stop during a hole-punched file waits for that file to finish
    z = fresh("punch-stop.zip", pents)
    d = os.path.join(work, "out13")
    code, out = run("run", z, d, "--stop-while-punching", env=env)
    assert code == 3 and "stop requested while punching" in out, out
    assert "wrote Game/data/huge.bin" in out, out
    with open(os.path.join(d, "Game", "data", "huge.bin"), "rb") as f:
        assert sha(f.read()) == sha(huge)
    code, out = run("run", z, d, env=env)
    assert code == 0, out
    expect_tree(d, pexp)
    ok("stop during a hole-punched file finishes that file first")

    # a hole-punched file that fails its CRC is reported lost, then skipped
    bad = noise(300 << 20, 5)
    z = fresh("punch-bad.zip", entries + [("Game/data/bad.bin", bad, 0, 0, {"crc": zlib.crc32(bad) ^ 1})])
    d = os.path.join(work, "out14")
    code, out = run("run", z, d, env=env)
    assert code == 1 and "was lost" in out and os.path.exists(z + ".zerozip-lost"), out
    code, out = run("plan", z, d, env=env)
    assert "lost=Game/data/bad.bin" in out and "problems=0" in out, out
    code, out = run("run", z, d, env=env)
    assert code == 0 and "lost  Game/data/bad.bin" in out, out
    expect_tree(d, expected)
    assert not os.path.exists(os.path.join(d, "Game", "data", "bad.bin")) and not os.path.exists(z + ".zerozip-lost")
    ok("damaged hole-punched file is reported lost and skipped on resume")

    shutil.rmtree(work, ignore_errors=True)
    print("\n%d groups passed" % passed)


if __name__ == "__main__":
    main()
