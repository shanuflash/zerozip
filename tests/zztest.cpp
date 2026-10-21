// Command-line driver for the engine, used by tests/run_tests.py.
//   zztest plan ZIP [DEST]
//   zztest run ZIP DEST [--stop-after N] [--stop-while-punching]
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../src/engine.h"

static std::string U8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) return 2;
    std::wstring mode = argv[1], zip = argv[2], dest = argc > 3 ? argv[3] : L"";
    std::wstring err;
    if (mode == L"plan") {
        Analysis a;
        if (!Analyze(zip, dest, a, err)) { printf("error: %s\n", U8(err).c_str()); return 1; }
        printf("dest=%s\nfiles=%zu dirs=%zu present=%zu unpacked=%llu toWrite=%llu\n", U8(a.dest).c_str(), a.files,
               a.dirs, a.present, a.unpacked, a.toWrite);
        printf("free=%llu minFree=%lld endFree=%lld normalNeed=%llu sameVolume=%d resuming=%d singleRoot=%d\n",
               a.freeBytes, a.minFree, a.endFree, a.normalNeed, a.sameVolume, a.resuming, a.singleRoot);
        printf("problems=%zu punchFiles=%zu sparse=%d lost=%s\n", a.problemCount, a.punchFiles, a.sparse,
               U8(a.lost).c_str());
        for (auto& p : a.problems) printf("  %s: %s\n", U8(p.name).c_str(), U8(p.reason).c_str());
        return 0;
    }
    int stopAfter = -1;
    bool stopPunching = false;
    for (int i = 4; i < argc; i++) {
        std::wstring o = argv[i];
        if (o == L"--stop-after" && i + 1 < argc) stopAfter = _wtoi(argv[++i]);
        if (o == L"--stop-while-punching") stopPunching = true;
    }
    Extractor ex;
    int files = 0;
    bool ok = ex.Run(
        zip, dest,
        [&](const Progress& p) {
            if (stopPunching && p.punching && !ex.Stopped()) {
                printf("stop requested while punching %s\n", U8(p.current).c_str());
                ex.Stop();
            }
        },
        [&](const std::wstring& name, uint64_t, FileResult r) {
            const char* tag = r == FileResult::Verified ? "ok   " : r == FileResult::Lost ? "lost " : "wrote";
            printf("%s %s\n", tag, U8(name).c_str());
            if (++files == stopAfter) ex.Stop();
        },
        err);
    if (ok) { printf("done\n"); return 0; }
    if (err.empty()) { printf("stopped\n"); return 3; }
    printf("error: %s\n", U8(err).c_str());
    return 1;
}
