#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct ZipEntry {
    std::wstring name;      // as stored in the zip, '/' separated
    std::wstring path;      // absolute output path (\\?\ form), empty if it can't be extracted
    uint64_t offset = 0;    // local header
    uint64_t data = 0;      // compressed data, found from the local header
    uint64_t csize = 0;
    uint64_t size = 0;
    uint32_t crc = 0;
    uint32_t attrs = 0;     // external attributes
    uint16_t method = 0;
    uint16_t flags = 0;
    uint16_t dosTime = 0;
    uint16_t dosDate = 0;
    uint16_t madeBy = 0;
    bool isDir = false;
    bool shadowed = false;  // an entry with the same name comes later in the zip, and wins
};

struct ZipIndex {
    std::vector<ZipEntry> entries;   // sorted by offset, ascending
    std::vector<uint8_t> tail;       // central directory through end of file, for the sidecar
    uint64_t tailOffset = 0;         // where `tail` starts in the original zip
    std::wstring dest;               // from the sidecar: where the stopped run was extracting to
};

struct Problem {
    std::wstring name;
    std::wstring reason;
};

struct Analysis {
    std::wstring zip, dest;
    uint64_t zipBytes = 0;        // current size on disk
    uint64_t unpacked = 0;        // total of all files
    uint64_t toWrite = 0;         // bytes that still have to be written
    uint64_t freeBytes = 0;       // free on the destination drive now
    uint64_t normalNeed = 0;      // free space a normal extractor would need
    int64_t minFree = 0;          // lowest free space during a ZeroZip run
    int64_t endFree = 0;          // free space when done
    uint64_t margin = 0;
    size_t files = 0, dirs = 0, present = 0;
    bool sameVolume = false;
    bool sparse = false;          // the drive can free parts of a file (NTFS, ReFS)
    bool resuming = false;        // a previous run on this zip was stopped
    bool singleRoot = false;      // everything sits under one top-level folder
    std::wstring volume;          // e.g. "C:"
    std::vector<std::pair<double, int64_t>> curve;   // progress 0..1 -> free bytes
    std::vector<Problem> problems;                  // first few entries that block the run
    size_t problemCount = 0;
    // Files too big for the free space. Their zip data is freed while they are
    // extracted, so a damaged one can't be recovered from the zip afterwards.
    size_t punchFiles = 0;
    std::vector<std::wstring> punchNames;
    std::wstring lost;            // a file a previous run lost partway through; it is skipped
};

struct Progress {
    size_t done = 0, total = 0;
    uint64_t bytesDone = 0, bytesTotal = 0;   // uncompressed bytes handled, written or checked
    uint64_t zipBytes = 0;                    // zip data still on disk
    uint64_t freeBytes = 0;
    std::wstring current;
    bool verifying = false;
    bool punching = false;
};

enum class FileResult { Written, Verified, Lost };

std::wstring SidecarPath(const std::wstring& zip);
std::wstring LostMarkerPath(const std::wstring& zip);

// An empty dest picks one: where a stopped run was going, else next to the zip
// if it has a single top folder, else a folder named after the zip.
bool Analyze(const std::wstring& zip, const std::wstring& dest, Analysis& a, std::wstring& err);

class Extractor {
public:
    using ProgressFn = std::function<void(const Progress&)>;
    using LogFn = std::function<void(const std::wstring& name, uint64_t size, FileResult result)>;

    Extractor();
    ~Extractor();
    Extractor(const Extractor&) = delete;
    Extractor& operator=(const Extractor&) = delete;

    // Clears a previous stop or pause. Call it before handing Run to another thread.
    void Reset();

    // Runs on the calling thread until finished, stopped or failed. Returns true
    // when the whole zip was extracted and removed; false with an empty `err`
    // means it was stopped.
    bool Run(const std::wstring& zip, const std::wstring& dest, ProgressFn onProgress, LogFn onFile,
             std::wstring& err);

    void Pause(bool on);
    // A file being extracted in hole-punch mode can't be stopped halfway, so a
    // stop then takes effect once that file is finished.
    void Stop();
    bool Stopped() const { return stop_; }
    bool WaitIfPaused();

private:
    std::atomic<bool> stop_{false};
    std::atomic<bool> paused_{false};
    HANDLE resume_;
};
