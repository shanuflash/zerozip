// ZeroZip engine: extracts a zip from its last entry to its first and cuts each
// entry's data off the end of the archive once its file is written, CRC-checked
// and flushed, so the archive gives its space back while it is being extracted.
//
// A file too big for the free space is extracted in hole-punch mode instead: its
// compressed data is freed from the (sparse) zip as it is decompressed.
#include "engine.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

#include <winioctl.h>

#include "../third_party/miniz/miniz.h"
#define ZSTD_STATIC_LINKING_ONLY   // for ZSTD_WINDOWLOG_MAX
#include "../third_party/zstd/zstd.h"

namespace {

constexpr uint64_t kMargin = 1ull << 30;   // never let the drive go below this
constexpr uint64_t kWindow = 64ull << 20;  // hole-punch mode frees zip data in steps this big
constexpr size_t kChunk = 4u << 20;
constexpr char kSidecarMagic[8] = {'Z', 'E', 'R', 'O', 'Z', 'I', 'P', '2'};
constexpr wchar_t kTemp[] = L".zzpart";
constexpr size_t kTempLen = 7;

// ---- small helpers ---------------------------------------------------------

uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
uint64_t rd64(const uint8_t* p) { uint64_t v; memcpy(&v, p, 8); return v; }

uint32_t g_crc[8][256];
struct CrcTables {
    CrcTables() {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            g_crc[0][i] = c;
        }
        for (int t = 1; t < 8; t++)
            for (int i = 0; i < 256; i++)
                g_crc[t][i] = (g_crc[t - 1][i] >> 8) ^ g_crc[0][g_crc[t - 1][i] & 0xFF];
    }
} g_crcTables;

uint32_t Crc32(uint32_t crc, const uint8_t* p, size_t n) {
    crc = ~crc;
    while (n >= 8) {
        uint32_t a = rd32(p) ^ crc, b = rd32(p + 4);
        crc = g_crc[7][a & 0xFF] ^ g_crc[6][(a >> 8) & 0xFF] ^ g_crc[5][(a >> 16) & 0xFF] ^
              g_crc[4][a >> 24] ^ g_crc[3][b & 0xFF] ^ g_crc[2][(b >> 8) & 0xFF] ^
              g_crc[1][(b >> 16) & 0xFF] ^ g_crc[0][b >> 24];
        p += 8;
        n -= 8;
    }
    while (n--) crc = g_crc[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE v) : h(v) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { Close(); }
    void Close() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
    }
    explicit operator bool() const { return h != INVALID_HANDLE_VALUE; }
};

std::wstring ErrText(DWORD code) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, (LPWSTR)&msg, 0, nullptr);
    std::wstring s = msg ? msg : L"Unknown error";
    LocalFree(msg);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L'.')) s.pop_back();
    return s;
}

bool ReadAt(HANDLE h, uint64_t off, void* buf, size_t n) {
    auto* p = static_cast<uint8_t*>(buf);
    while (n) {
        OVERLAPPED ov{};
        ov.Offset = DWORD(off);
        ov.OffsetHigh = DWORD(off >> 32);
        DWORD want = DWORD(std::min<size_t>(n, kChunk)), got = 0;
        if (!ReadFile(h, p, want, &got, &ov) || got == 0) return false;
        p += got;
        off += got;
        n -= got;
    }
    return true;
}

bool WriteAll(HANDLE h, const void* buf, size_t n) {
    auto* p = static_cast<const uint8_t*>(buf);
    while (n) {
        DWORD put = 0;
        if (!WriteFile(h, p, DWORD(std::min<size_t>(n, kChunk)), &put, nullptr) || put == 0) return false;
        p += put;
        n -= put;
    }
    return true;
}

uint64_t FileSize(HANDLE h) {
    LARGE_INTEGER li{};
    GetFileSizeEx(h, &li);
    return uint64_t(li.QuadPart);
}

std::wstring Decode(const uint8_t* p, size_t n, bool utf8) {
    if (!n) return {};
    UINT cp = utf8 ? CP_UTF8 : CP_OEMCP;
    int len = MultiByteToWideChar(cp, 0, (LPCCH)p, int(n), nullptr, 0);
    std::wstring s(size_t(len), L'\0');
    MultiByteToWideChar(cp, 0, (LPCCH)p, int(n), s.data(), len);
    return s;
}

std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), DWORD(s.size()));
    return s;
}

std::wstring FullPath(const std::wstring& p) {
    DWORD n = GetFullPathNameW(p.c_str(), 0, nullptr, nullptr);
    if (!n) return p;
    std::wstring s(n, L'\0');
    n = GetFullPathNameW(p.c_str(), n, s.data(), nullptr);
    s.resize(n);
    while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    return s;
}

// \\?\ form, so paths past MAX_PATH work.
std::wstring Extended(std::wstring full) {
    if (full.size() > 3 && full.back() == L'\\') full.pop_back();
    if (full.size() == 3 && full[1] == L':') full.pop_back();   // "C:\" -> "C:", parts get "\" prefixed
    if (full.rfind(L"\\\\?\\", 0) == 0) return full;
    if (full.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + full.substr(2);
    return L"\\\\?\\" + full;
}

// Parent folder; a drive root comes back as "C:\" so it isn't read as "current folder on C:".
std::wstring ParentOf(const std::wstring& p) {
    size_t k = p.find_last_of(L'\\');
    if (k == std::wstring::npos) return {};
    std::wstring up = p.substr(0, k);
    bool drive = (up.size() == 2 && up[1] == L':') || (up.size() == 6 && up.rfind(L"\\\\?\\", 0) == 0 && up[5] == L':');
    if (drive) up += L'\\';
    return up;
}

// Closest folder on the path that already exists, for free-space and volume lookups.
std::wstring ExistingAncestor(std::wstring p) {
    while (!p.empty()) {
        DWORD a = GetFileAttributesW(p.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) return p;
        std::wstring up = ParentOf(p);
        if (up == p) break;
        p = up;
    }
    return p;
}

std::wstring VolumeOf(const std::wstring& p) {
    wchar_t v[MAX_PATH + 1] = {};
    if (!GetVolumePathNameW(p.c_str(), v, MAX_PATH)) return {};
    std::wstring s = v;
    if (s.rfind(L"\\\\?\\", 0) == 0) s = s.substr(4);
    while (s.size() > 2 && s.back() == L'\\') s.pop_back();
    return s;
}

uint64_t FreeOn(const std::wstring& dir) {
    ULARGE_INTEGER avail{};
    GetDiskFreeSpaceExW(ExistingAncestor(dir).c_str(), &avail, nullptr, nullptr);
    return avail.QuadPart;
}

struct VolumeInfo {
    uint64_t cluster = 4096;
    bool sparse = false;   // can free ranges inside a file
    bool fat = false;      // FAT/FAT32: no file over 4 GB
};

VolumeInfo InfoOf(const std::wstring& dir) {
    VolumeInfo v;
    std::wstring root = VolumeOf(ExistingAncestor(dir));
    if (root.empty()) return v;
    root += L'\\';
    DWORD spc = 0, bps = 0, f = 0, t = 0;
    if (GetDiskFreeSpaceW(root.c_str(), &spc, &bps, &f, &t) && spc && bps) v.cluster = uint64_t(spc) * bps;
    DWORD flags = 0;
    wchar_t fs[32] = {};
    if (GetVolumeInformationW(root.c_str(), nullptr, 0, nullptr, nullptr, &flags, fs, 32)) {
        v.sparse = (flags & FILE_SUPPORTS_SPARSE_FILES) != 0;
        v.fat = Lower(fs).rfind(L"fat", 0) == 0;
    }
    return v;
}

bool SameVolume(const std::wstring& a, const std::wstring& b) {
    std::wstring va = VolumeOf(ExistingAncestor(a)), vb = VolumeOf(ExistingAncestor(b));
    return !va.empty() && Lower(va) == Lower(vb);
}

bool MakeDirs(const std::wstring& path) {
    if (path.empty()) return true;
    DWORD a = GetFileAttributesW(path.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    std::wstring up = ParentOf(path);
    if (!up.empty() && up != path && up.size() > 7 && !MakeDirs(up)) return false;
    return CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

// Makes a rename inside `dir` durable before the zip data it replaces is cut.
void FlushDir(const std::wstring& dir) {
    Handle h(CreateFileW(dir.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (h) FlushFileBuffers(h.h);
}

bool SizeOnDisk(const std::wstring& path, uint64_t& size) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d) || (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return false;
    size = (uint64_t(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
    return true;
}

bool RemoveFile(const std::wstring& path) {
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    return DeleteFileW(path.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
}

// Test hook: files at least this big use hole-punch mode even when they'd fit.
uint64_t PunchOverride() {
    wchar_t v[32];
    DWORD n = GetEnvironmentVariableW(L"ZEROZIP_TEST_PUNCH_OVER", v, 32);
    return n && n < 32 ? _wcstoui64(v, nullptr, 10) : UINT64_MAX;
}

// One path component made safe for Windows. Returns false for "..".
bool CleanPart(std::wstring& s) {
    if (s == L"..") return false;
    for (auto& c : s)
        if (c < 32 || wcschr(L"<>:\"|?*", c)) c = L'_';
    if (!s.empty() && (s.back() == L'.' || s.back() == L' ')) s.back() = L'_';
    std::wstring stem = s.substr(0, s.find(L'.'));
    for (auto& c : stem) c = wchar_t(towupper(c));
    static const wchar_t* kReserved[] = {L"CON", L"PRN", L"AUX", L"NUL", L"COM1", L"COM2", L"COM3", L"COM4",
                                         L"COM5", L"COM6", L"COM7", L"COM8", L"COM9", L"LPT1", L"LPT2",
                                         L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9"};
    for (auto* r : kReserved)
        if (stem == r) s = L"_" + s;
    return true;
}

std::vector<std::wstring> SplitName(const std::wstring& name) {
    std::vector<std::wstring> parts;
    std::wstring cur;
    for (wchar_t c : name) {
        if (c == L'/' || c == L'\\') {
            if (!cur.empty() && cur != L".") parts.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty() && cur != L".") parts.push_back(cur);
    return parts;
}

bool DosAttrs(const ZipEntry& e) {
    int host = e.madeBy >> 8;
    return host == 0 || host == 10 || host == 11 || host == 14;
}

// ---- reading the zip's index ------------------------------------------------

const wchar_t kDamaged[] = L"The zip's file list is damaged or this is not a zip file.";

bool ParseTail(const uint8_t* buf, size_t len, uint64_t base, ZipIndex& idx, std::wstring& err) {
    if (len < 22) { err = kDamaged; return false; }
    size_t eocd = SIZE_MAX;
    for (size_t i = len - 22 + 1; i-- > 0 && len - i <= 22 + 65535;) {
        if (rd32(buf + i) == 0x06054b50 && i + 22 + rd16(buf + i + 20) <= len) { eocd = i; break; }
    }
    if (eocd == SIZE_MAX) { err = kDamaged; return false; }
    const uint8_t* e = buf + eocd;
    uint64_t disk = rd16(e + 4), cdDisk = rd16(e + 6), count = rd16(e + 10), cdOff = rd32(e + 16);
    if (eocd >= 20 && rd32(e - 20) == 0x07064b50) {
        uint64_t z64 = rd64(e - 20 + 8);
        if (rd32(e - 20 + 16) > 1) { err = L"Split (multi-part) zips aren't supported."; return false; }
        if (z64 < base || z64 - base + 56 > len || rd32(buf + (z64 - base)) != 0x06064b50) { err = kDamaged; return false; }
        const uint8_t* z = buf + (z64 - base);
        disk = rd32(z + 16);
        cdDisk = rd32(z + 20);
        count = rd64(z + 32);
        cdOff = rd64(z + 48);
    }
    if (disk || cdDisk) { err = L"Split (multi-part) zips aren't supported."; return false; }
    if (cdOff != base) {
        err = L"This zip has extra data around its contents (a self-extractor?) and isn't supported.";
        return false;
    }
    if (count > len / 46) { err = kDamaged; return false; }
    size_t p = 0;
    idx.entries.clear();
    idx.entries.reserve(size_t(count));
    for (uint64_t k = 0; k < count; k++) {
        if (p + 46 > len || rd32(buf + p) != 0x02014b50) { err = kDamaged; return false; }
        const uint8_t* c = buf + p;
        ZipEntry z;
        z.madeBy = rd16(c + 4);
        z.flags = rd16(c + 8);
        z.method = rd16(c + 10);
        z.dosTime = rd16(c + 12);
        z.dosDate = rd16(c + 14);
        z.crc = rd32(c + 16);
        uint64_t cs = rd32(c + 20), us = rd32(c + 24), lho = rd32(c + 42);
        size_t nlen = rd16(c + 28), xlen = rd16(c + 30), clen = rd16(c + 32);
        z.attrs = rd32(c + 38);
        if (p + 46 + nlen + xlen + clen > len) { err = kDamaged; return false; }
        const uint8_t* name = c + 46;
        const uint8_t* x = name + nlen;
        z.name = Decode(name, nlen, (z.flags & 0x800) != 0);
        for (size_t q = 0; q + 4 <= xlen;) {
            uint16_t id = rd16(x + q), sz = rd16(x + q + 2);
            const uint8_t* d = x + q + 4;
            if (q + 4 + sz > xlen) break;
            if (id == 0x0001) {
                size_t r = 0;
                if (us == 0xFFFFFFFF && r + 8 <= sz) { us = rd64(d + r); r += 8; }
                if (cs == 0xFFFFFFFF && r + 8 <= sz) { cs = rd64(d + r); r += 8; }
                if (lho == 0xFFFFFFFF && r + 8 <= sz) { lho = rd64(d + r); r += 8; }
            } else if (id == 0x7075 && sz >= 5 && !(z.flags & 0x800)) {
                if (rd32(d + 1) == Crc32(0, name, nlen)) z.name = Decode(d + 5, sz - 5u, true);
            }
            q += 4u + sz;
        }
        z.size = us;
        z.csize = cs;
        z.offset = lho;
        z.isDir = (!z.name.empty() && (z.name.back() == L'/' || z.name.back() == L'\\')) ||
                  (DosAttrs(z) && (z.attrs & FILE_ATTRIBUTE_DIRECTORY) && z.size == 0);
        if (z.offset >= base) { err = kDamaged; return false; }
        idx.entries.push_back(std::move(z));
        p += 46 + nlen + xlen + clen;
    }
    std::sort(idx.entries.begin(), idx.entries.end(),
              [](const ZipEntry& a, const ZipEntry& b) { return a.offset < b.offset; });
    idx.tail.assign(buf, buf + len);
    idx.tailOffset = base;
    return true;
}

bool ReadIndexFromZip(HANDLE h, ZipIndex& idx, std::wstring& err) {
    uint64_t size = FileSize(h);
    size_t look = size_t(std::min<uint64_t>(size, 22 + 65535 + 20 + 56));
    std::vector<uint8_t> end(look);
    if (!ReadAt(h, size - look, end.data(), look)) { err = L"Couldn't read the zip: " + ErrText(GetLastError()); return false; }
    // Find where the central directory starts, then read it through end of file.
    uint64_t cdOff = UINT64_MAX, cdSize = 0;
    for (size_t i = look >= 22 ? look - 22 + 1 : 0; i-- > 0;) {
        const uint8_t* e = end.data() + i;
        if (rd32(e) != 0x06054b50 || i + 22 + rd16(e + 20) > look) continue;
        cdOff = rd32(e + 16);
        cdSize = rd32(e + 12);
        if (i >= 20 && rd32(e - 20) == 0x07064b50) {
            uint64_t z64 = rd64(e - 20 + 8);
            uint8_t rec[56];
            if (z64 + 56 <= size && ReadAt(h, z64, rec, 56) && rd32(rec) == 0x06064b50) {
                cdOff = rd64(rec + 48);
                cdSize = rd64(rec + 40);
            }
        }
        break;
    }
    // After the directory come only the end records and a comment, so anything
    // much bigger means the offsets are garbage.
    if (cdOff == UINT64_MAX || cdOff >= size || cdSize > size - cdOff || size - cdOff - cdSize > (1u << 20)) {
        err = L"This doesn't look like a zip file, or its end is missing (still downloading?).";
        return false;
    }
    std::vector<uint8_t> tail(size_t(size - cdOff));
    if (!ReadAt(h, cdOff, tail.data(), tail.size())) { err = L"Couldn't read the zip: " + ErrText(GetLastError()); return false; }
    return ParseTail(tail.data(), tail.size(), cdOff, idx, err);
}

// Reads the local header of every entry still in the zip to find where its data
// starts, and checks that no entry's data runs into the next one: cutting the
// zip at an entry must never damage another.
bool ReadLocalHeaders(HANDLE h, uint64_t zipSize, ZipIndex& idx, std::wstring& err) {
    for (size_t i = 0; i < idx.entries.size(); i++) {
        ZipEntry& e = idx.entries[i];
        if (e.offset >= zipSize) continue;   // already extracted and cut off
        uint8_t lh[30];
        if (e.offset + 30 > zipSize || !ReadAt(h, e.offset, lh, 30) || rd32(lh) != 0x04034b50) {
            err = L"The zip is damaged near \"" + e.name + L"\".";
            return false;
        }
        e.data = e.offset + 30 + rd16(lh + 26) + rd16(lh + 28);
        uint64_t next = i + 1 < idx.entries.size() ? idx.entries[i + 1].offset : idx.tailOffset;
        if (e.data + e.csize > next) {
            err = L"Entries in this zip overlap each other, so it can't be extracted in place.";
            return false;
        }
    }
    return true;
}

struct Identity {
    uint32_t volume = 0;
    uint64_t file = 0;
};

Identity IdentityOf(HANDLE h) {
    BY_HANDLE_FILE_INFORMATION info{};
    GetFileInformationByHandle(h, &info);
    return {info.dwVolumeSerialNumber, (uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow};
}

// The sidecar holds a copy of the central directory, which is cut off with the
// last entry, plus what it takes to resume: which file it belongs to and where
// the files were going.
//   magic[8] tailOffset:u64 originalSize:u64 volume:u32 destLen:u32 file:u64 dest[destLen] tail
constexpr size_t kSidecarHead = 40;

enum class Sidecar { None, Stale, Ok, Bad };

Sidecar ReadSidecar(const std::wstring& zip, HANDLE zh, ZipIndex& idx, std::wstring& err) {
    Handle h(CreateFileW(SidecarPath(zip).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
    if (!h) {
        DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND) return Sidecar::None;
        err = L"Couldn't read the resume file next to the zip: " + ErrText(code);
        return Sidecar::Bad;
    }
    uint64_t size = FileSize(h.h);
    uint8_t head[kSidecarHead];
    if (size < kSidecarHead || size > (1ull << 31) || !ReadAt(h.h, 0, head, kSidecarHead) ||
        memcmp(head, kSidecarMagic, 8) != 0)
        return Sidecar::Stale;
    uint64_t base = rd64(head + 8), original = rd64(head + 16);
    Identity id{rd32(head + 24), rd64(head + 32)};
    uint32_t destLen = rd32(head + 28);
    Identity now = IdentityOf(zh);
    if (id.volume != now.volume || id.file != now.file) return Sidecar::Stale;   // left over from another zip
    if (kSidecarHead + uint64_t(destLen) * 2 > size) return Sidecar::Stale;
    std::vector<uint8_t> b(static_cast<size_t>(size - kSidecarHead));
    if (!ReadAt(h.h, kSidecarHead, b.data(), b.size())) { err = L"Couldn't read the resume file next to the zip."; return Sidecar::Bad; }
    idx.dest.assign(reinterpret_cast<const wchar_t*>(b.data()), destLen);
    size_t skip = size_t(destLen) * 2;
    if (!ParseTail(b.data() + skip, b.size() - skip, base, idx, err)) return Sidecar::Bad;
    // The zip may only ever have been cut at an entry boundary.
    uint64_t zipSize = FileSize(zh);
    bool atBoundary = zipSize == original;
    for (auto& e : idx.entries) atBoundary = atBoundary || e.offset == zipSize;
    if (!atBoundary) {
        err = L"The zip has changed since ZeroZip last worked on it, so it can't be resumed safely.";
        return Sidecar::Bad;
    }
    return Sidecar::Ok;
}

bool WriteSidecar(const std::wstring& zip, HANDLE zh, const ZipIndex& idx, const std::wstring& dest, std::wstring& err) {
    std::wstring path = SidecarPath(zip), tmp = path + L".tmp";
    uint8_t head[kSidecarHead] = {};
    uint64_t original = FileSize(zh);
    Identity id = IdentityOf(zh);
    uint32_t destLen = uint32_t(dest.size());
    memcpy(head, kSidecarMagic, 8);
    memcpy(head + 8, &idx.tailOffset, 8);
    memcpy(head + 16, &original, 8);
    memcpy(head + 24, &id.volume, 4);
    memcpy(head + 28, &destLen, 4);
    memcpy(head + 32, &id.file, 8);
    {
        Handle h(CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr));
        if (!h || !WriteAll(h.h, head, kSidecarHead) || !WriteAll(h.h, dest.data(), dest.size() * 2) ||
            !WriteAll(h.h, idx.tail.data(), idx.tail.size()) || !FlushFileBuffers(h.h)) {
            err = L"Couldn't save resume data next to the zip: " + ErrText(GetLastError());
            return false;
        }
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        err = L"Couldn't save resume data next to the zip: " + ErrText(GetLastError());
        return false;
    }
    FlushDir(ParentOf(path));
    return true;
}

// Written before a file's zip data starts being freed and removed once the file
// is done. If it is still there on the next run, that file was cut off midway.
bool ReadLostMarker(const std::wstring& zip, uint64_t& offset) {
    Handle h(CreateFileW(LostMarkerPath(zip).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
    uint8_t b[8];
    if (!h || !ReadAt(h.h, 0, b, 8)) return false;
    offset = rd64(b);
    return true;
}

bool WriteLostMarker(const std::wstring& zip, uint64_t offset) {
    Handle h(CreateFileW(LostMarkerPath(zip).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN,
                         nullptr));
    return h && WriteAll(h.h, &offset, 8) && FlushFileBuffers(h.h);
}

// Loads the index from the sidecar when resuming, otherwise from the zip.
bool LoadIndex(const std::wstring& zip, HANDLE zh, ZipIndex& idx, bool& resuming, std::wstring& err) {
    Sidecar s = ReadSidecar(zip, zh, idx, err);
    if (s == Sidecar::Bad) return false;
    resuming = s == Sidecar::Ok;
    if (!resuming && !ReadIndexFromZip(zh, idx, err)) return false;
    return ReadLocalHeaders(zh, FileSize(zh), idx, err);
}

const wchar_t* MethodProblem(const ZipEntry& e) {
    if (e.flags & 1) return L"password protected";
    switch (e.method) {
        case 0: case 8: case 93: return nullptr;
        case 9: return L"Deflate64 compression";
        case 12: return L"BZip2 compression";
        case 14: return L"LZMA compression";
        case 95: return L"XZ compression";
        case 98: return L"PPMd compression";
        case 99: return L"AES encrypted";
        default: return L"unknown compression";
    }
}

struct Problems {
    std::vector<Problem>& list;
    size_t& count;
    void Add(const std::wstring& name, const std::wstring& reason) {
        if (list.size() < 6) list.push_back({name, reason});
        count++;
    }
};

// Fills in each entry's output path and finds entries that would fail partway
// through a run, so they block it before anything is touched.
void MapPaths(ZipIndex& idx, const std::wstring& destFull, const std::wstring& zipFull, Problems bad, bool& singleRoot) {
    std::wstring base = Extended(destFull);
    VolumeInfo vol = InfoOf(destFull);
    const std::unordered_set<std::wstring> reserved = {Lower(Extended(zipFull)), Lower(Extended(SidecarPath(zipFull))),
                                                       Lower(Extended(LostMarkerPath(zipFull)))};
    std::unordered_map<std::wstring, size_t> lastByName;
    for (size_t i = 0; i < idx.entries.size(); i++)
        if (!idx.entries[i].isDir) lastByName[idx.entries[i].name] = i;

    std::unordered_set<std::wstring> files, dirs;
    std::wstring root;
    singleRoot = !idx.entries.empty();
    for (size_t i = 0; i < idx.entries.size(); i++) {
        ZipEntry& e = idx.entries[i];
        e.path.clear();
        e.shadowed = !e.isDir && lastByName[e.name] != i;
        if (e.shadowed) continue;
        auto parts = SplitName(e.name);
        bool safe = !parts.empty();
        for (auto& p : parts) safe = CleanPart(p) && safe;
        if (!safe) { bad.Add(e.name, parts.empty() ? L"empty name" : L"unsafe path (\"..\")"); continue; }
        if (!e.isDir) {
            if (const wchar_t* m = MethodProblem(e)) { bad.Add(e.name, m); continue; }
            if (vol.fat && e.size > 0xFFFFFFFFull) { bad.Add(e.name, L"bigger than a FAT32 drive allows (4 GB)"); continue; }
        }
        bool tooLong = false;
        for (auto& p : parts) tooLong = tooLong || p.size() > 255;
        if (!e.isDir && parts.back().size() + kTempLen > 255) tooLong = true;
        if (tooLong) { bad.Add(e.name, L"name too long for Windows"); continue; }
        if (!e.isDir && Lower(parts.back()).size() > kTempLen &&
            Lower(parts.back()).compare(parts.back().size() - kTempLen, kTempLen, kTemp) == 0) {
            bad.Add(e.name, L"uses the name ZeroZip gives its temporary files");
            continue;
        }

        std::wstring first = Lower(parts[0]);
        if (root.empty()) root = first;
        if (first != root || (parts.size() == 1 && !e.isDir)) singleRoot = false;

        std::wstring dir = base;
        for (size_t k = 0; k + 1 < parts.size(); k++) {
            dir += L"\\" + parts[k];
            dirs.insert(Lower(dir));
        }
        if (e.isDir) {
            e.path = dir + L"\\" + parts.back();
            dirs.insert(Lower(e.path));
            continue;
        }
        // Names that only differ in case or in characters Windows can't store
        // would land on the same file; give the later ones a numbered name.
        std::wstring leaf = parts.back(), path = dir + L"\\" + leaf;
        for (int n = 2; files.count(Lower(path)); n++) {
            size_t dot = leaf.find_last_of(L'.');
            std::wstring stem = dot == std::wstring::npos || dot == 0 ? leaf : leaf.substr(0, dot);
            std::wstring ext = dot == std::wstring::npos || dot == 0 ? L"" : leaf.substr(dot);
            path = dir + L"\\" + stem + L" (" + std::to_wstring(n) + L")" + ext;
        }
        if (reserved.count(Lower(path))) { bad.Add(e.name, L"would overwrite the zip or ZeroZip's resume data"); continue; }
        files.insert(Lower(path));
        e.path = path;
    }
    for (auto& e : idx.entries)
        if (!e.isDir && !e.path.empty() && dirs.count(Lower(e.path))) {
            bad.Add(e.name, L"a folder in the zip has the same name");
            e.path.clear();
        }
}

}  // namespace

std::wstring SidecarPath(const std::wstring& zip) { return zip + L".zerozip"; }
std::wstring LostMarkerPath(const std::wstring& zip) { return zip + L".zerozip-lost"; }

bool Analyze(const std::wstring& zipIn, const std::wstring& destIn, Analysis& a, std::wstring& err) {
    a = Analysis();
    a.zip = FullPath(zipIn);
    Handle h(CreateFileW(a.zip.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, 0, nullptr));
    if (!h) { err = L"Couldn't open the zip: " + ErrText(GetLastError()); return false; }
    a.zipBytes = FileSize(h.h);
    ZipIndex idx;
    if (!LoadIndex(a.zip, h.h, idx, a.resuming, err)) return false;
    h.Close();

    std::wstring zipDir = ParentOf(a.zip);
    std::vector<Problem> ignored;
    size_t ignoredCount = 0;
    if (!destIn.empty()) {
        a.dest = FullPath(destIn);
    } else if (a.resuming && !idx.dest.empty()) {
        a.dest = idx.dest;
    } else {
        // Like "smart extract" elsewhere: a zip with one top folder goes next to
        // the zip, anything else into a folder named after it.
        MapPaths(idx, zipDir, a.zip, {ignored, ignoredCount}, a.singleRoot);
        std::wstring stem = a.zip.substr(a.zip.find_last_of(L'\\') + 1);
        size_t dot = stem.find_last_of(L'.');
        if (dot != std::wstring::npos && dot > 0) stem.resize(dot);
        a.dest = a.singleRoot ? zipDir : FullPath(zipDir + L"\\" + stem);
    }
    MapPaths(idx, a.dest, a.zip, {a.problems, a.problemCount}, a.singleRoot);

    a.volume = VolumeOf(ExistingAncestor(a.dest));
    a.sameVolume = SameVolume(a.zip, a.dest);
    VolumeInfo vol = InfoOf(a.dest);
    a.sparse = a.sameVolume && vol.sparse;
    a.freeBytes = FreeOn(a.dest);
    a.margin = kMargin;
    uint64_t lostAt = UINT64_MAX, forced = PunchOverride();
    if (a.resuming) ReadLostMarker(a.zip, lostAt);
    auto onDisk = [&](uint64_t n) { return (n + vol.cluster - 1) / vol.cluster * vol.cluster; };

    Problems bad{a.problems, a.problemCount};
    int64_t f = int64_t(a.freeBytes);
    if (!a.resuming && a.sameVolume) f -= int64_t(onDisk(idx.tail.size() + kSidecarHead + a.dest.size() * 2));
    int64_t worst = f;
    uint64_t prevEnd = a.zipBytes;
    double total = double(std::max<uint64_t>(a.zipBytes, 1));
    std::vector<std::pair<double, int64_t>> pts;
    pts.push_back({0.0, f});
    for (size_t i = idx.entries.size(); i-- > 0;) {
        const ZipEntry& e = idx.entries[i];
        bool gone = e.offset >= a.zipBytes;
        int64_t credit = 0;   // zip data a hole-punched file frees before the zip is cut
        if (e.isDir) {
            a.dirs++;
        } else if (!e.path.empty()) {
            a.files++;
            a.unpacked += e.size;
            uint64_t have = 0;
            bool exists = SizeOnDisk(e.path, have);
            if (exists && have == e.size) {
                a.present++;
            } else if (e.offset == lostAt) {
                a.lost = e.name;
            } else if (gone) {
                bad.Add(e.name, L"already removed from the zip but missing from the folder");
            } else {
                if (exists) f += int64_t(onDisk(have));   // the wrong-size copy is deleted first
                int64_t need = int64_t(onDisk(e.size));
                if (a.sparse && (f - need < int64_t(kMargin) || e.size >= forced)) {
                    credit = int64_t(e.csize > kWindow ? e.csize - kWindow : 0);
                    a.punchFiles++;
                    if (a.punchNames.size() < 3) a.punchNames.push_back(e.name);
                }
                f += credit - need;
                a.toWrite += e.size;
                a.normalNeed += uint64_t(need);
                worst = std::min(worst, f);
            }
        }
        double x = (total - double(std::min(e.offset, a.zipBytes))) / total;
        pts.push_back({x, f});
        if (!gone && a.sameVolume) f += int64_t(prevEnd - e.offset) - credit;
        if (!gone) prevEnd = e.offset;
        pts.push_back({x, f});
    }
    if (a.sameVolume) f += int64_t(prevEnd);   // whatever is left of the zip is deleted at the end
    pts.push_back({1.0, f});
    a.minFree = worst;
    a.endFree = f;

    // Keep the chart light: at most ~240 points, each bucket keeping its lowest value.
    const size_t kBuckets = 240;
    if (pts.size() <= kBuckets) {
        a.curve = std::move(pts);
    } else {
        std::vector<std::pair<double, int64_t>> out;
        size_t per = (pts.size() + kBuckets - 1) / kBuckets;
        for (size_t i = 0; i < pts.size(); i += per) {
            auto best = pts[i];
            for (size_t j = i; j < std::min(pts.size(), i + per); j++)
                if (pts[j].second < best.second) best = pts[j];
            best.first = pts[std::min(pts.size(), i + per) - 1].first;
            out.push_back(best);
        }
        out.front().first = 0.0;
        out.push_back(pts.back());
        a.curve = std::move(out);
    }
    return true;
}

// ---- extraction ---------------------------------------------------------------

Extractor::Extractor() : resume_(CreateEventW(nullptr, TRUE, TRUE, nullptr)) {}

Extractor::~Extractor() {
    if (resume_) CloseHandle(resume_);
}

void Extractor::Reset() {
    stop_ = false;
    Pause(false);
}

void Extractor::Pause(bool on) {
    paused_ = on;
    if (!resume_) return;
    if (on) ResetEvent(resume_);
    else SetEvent(resume_);
}

void Extractor::Stop() {
    stop_ = true;
    Pause(false);
}

bool Extractor::WaitIfPaused() {
    while (paused_ && !stop_) {
        if (resume_) WaitForSingleObject(resume_, 250);
        else Sleep(50);
    }
    return !stop_;
}

namespace {

struct Job {
    HANDLE zip;
    uint64_t zipSize;
    std::wstring zipPath, dest;
    uint64_t freeCheckedAt = 0;
    uint64_t punchUnit = 64 << 10;     // freed ranges of a sparse file are rounded to this
    bool zipSparse = false;
    uint64_t markerAt = UINT64_MAX;    // entry the lost-file marker points at
    std::vector<uint8_t> in = std::vector<uint8_t>(kChunk), out = std::vector<uint8_t>(kChunk);
    ZSTD_DCtx* zstd = nullptr;
    Progress prog;
    Extractor::ProgressFn onProgress;
    std::function<bool()> keepGoing;

    Job(HANDLE z, std::wstring zp, std::wstring d)
        : zip(z), zipSize(FileSize(z)), zipPath(std::move(zp)), dest(std::move(d)) {}
    ~Job() { if (zstd) ZSTD_freeDCtx(zstd); }

    void Tick(uint64_t bytes) {
        prog.bytesDone += bytes;
        if (prog.bytesDone - freeCheckedAt >= (256ull << 20)) {
            freeCheckedAt = prog.bytesDone;
            prog.freeBytes = FreeOn(dest);
        }
        onProgress(prog);
    }

    enum class Check { Ok, Missing, Bad, Stopped };

    // Whether `path` already holds this entry, by size and CRC.
    Check Matches(const std::wstring& path, const ZipEntry& e) {
        uint64_t have = 0;
        if (!SizeOnDisk(path, have)) return Check::Missing;
        if (have != e.size) return Check::Bad;
        Handle h(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (!h) return Check::Bad;
        uint64_t before = prog.bytesDone;
        prog.verifying = true;
        uint32_t crc = 0;
        Check result = Check::Bad;
        for (;;) {
            if (!keepGoing()) { result = Check::Stopped; break; }
            DWORD got = 0;
            if (!ReadFile(h.h, in.data(), DWORD(kChunk), &got, nullptr)) break;
            if (!got) { result = crc == e.crc ? Check::Ok : Check::Bad; break; }
            crc = Crc32(crc, in.data(), got);
            Tick(got);
        }
        prog.verifying = false;
        if (result != Check::Ok) prog.bytesDone = before;
        return result;
    }

    std::wstring Lost(const ZipEntry& e, const std::wstring& why) {
        return L"\"" + e.name + L"\" was lost: " + why +
               L" after part of its zip data had already been freed. Open the zip again to carry on with the other files.";
    }

    // Decompresses one entry into `<path>.zzpart`, checks it, then moves it into place.
    // With `punch`, the entry's zip data is freed as it is used, for files too big
    // for the free space.
    bool Extract(const ZipEntry& e, bool punch, std::wstring& err) {
        if (e.data + e.csize > zipSize) { err = L"The zip is damaged near \"" + e.name + L"\"."; return false; }
        if (punch) {
            DWORD ret = 0;
            FILE_SET_SPARSE_BUFFER sb{TRUE};
            if (!zipSparse && !DeviceIoControl(zip, FSCTL_SET_SPARSE, &sb, sizeof sb, nullptr, 0, &ret, nullptr)) {
                err = L"Couldn't prepare the zip for freeing space partway through a file: " + ErrText(GetLastError());
                return false;
            }
            zipSparse = true;
            if (!WriteLostMarker(zipPath, e.offset)) {
                err = L"Couldn't save resume data next to the zip: " + ErrText(GetLastError());
                return false;
            }
            markerAt = e.offset;
        }

        std::wstring tmp = e.path + kTemp;
        Handle file(CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (!file) { err = L"Couldn't create \"" + e.name + L"\": " + ErrText(GetLastError()); return false; }
        if (!punch) {   // reserving the whole file up front would take the space punching is meant to free
            FILE_ALLOCATION_INFO alloc{};
            alloc.AllocationSize.QuadPart = LONGLONG(e.size);
            SetFileInformationByHandle(file.h, FileAllocationInfo, &alloc, sizeof alloc);
        }

        uint64_t pos = e.data, left = e.csize, written = 0;
        uint64_t punchFrom = (pos + punchUnit - 1) / punchUnit * punchUnit, punched = 0;
        uint32_t crc = 0;
        bool ok = true, stopped = false;
        std::wstring why;
        prog.punching = punch;
        // In punch mode a stop waits for the end of the file: its freed data can't be restored.
        auto alive = [&] { return keepGoing() || punch; };
        auto emit = [&](const uint8_t* p, size_t n) {
            if (!n) return true;
            if (written + n > e.size) { why = L"it holds more data than the zip says"; return false; }
            crc = Crc32(crc, p, n);
            written += n;
            if (!WriteAll(file.h, p, n)) {
                DWORD code = GetLastError();
                why = code == ERROR_DISK_FULL ? L"the drive is full" : L"couldn't write it: " + ErrText(code);
                return false;
            }
            Tick(n);
            return true;
        };
        // Frees zip data that has been fully decompressed, once its output is on disk.
        auto release = [&] {
            uint64_t to = pos / punchUnit * punchUnit;
            if (!punch || to < punchFrom + kWindow) return true;
            FILE_ZERO_DATA_INFORMATION z{};
            z.FileOffset.QuadPart = LONGLONG(punchFrom);
            z.BeyondFinalZero.QuadPart = LONGLONG(to);
            DWORD ret = 0;
            if (!FlushFileBuffers(file.h) || !DeviceIoControl(zip, FSCTL_SET_ZERO_DATA, &z, sizeof z, nullptr, 0, &ret, nullptr)) {
                why = L"couldn't free its zip data: " + ErrText(GetLastError());
                return false;
            }
            punched += to - punchFrom;
            punchFrom = to;
            prog.zipBytes = zipSize - punched;
            return true;
        };
        auto readChunk = [&](size_t& n) {
            if (!release()) return false;
            n = size_t(std::min<uint64_t>(left, kChunk));
            if (n && !ReadAt(zip, pos, in.data(), n)) { why = L"couldn't read the zip: " + ErrText(GetLastError()); return false; }
            pos += n;
            left -= n;
            return true;
        };

        if (e.size == 0) {
            // nothing to decompress; an empty deflate stream may still take a couple of bytes
        } else if (e.method == 0) {
            while (ok && left) {
                if (!alive()) { stopped = true; break; }
                size_t n;
                ok = readChunk(n) && emit(in.data(), n);
            }
        } else if (e.method == 8) {
            mz_stream s{};
            ok = mz_inflateInit2(&s, -MZ_DEFAULT_WINDOW_BITS) == MZ_OK;
            for (bool done = false; ok && !done;) {
                if (!alive()) { stopped = true; break; }
                if (s.avail_in == 0 && left) {
                    size_t n;
                    if (!readChunk(n)) { ok = false; break; }
                    s.next_in = in.data();
                    s.avail_in = unsigned(n);
                }
                s.next_out = out.data();
                s.avail_out = unsigned(kChunk);
                int r = mz_inflate(&s, MZ_NO_FLUSH);
                size_t made = kChunk - s.avail_out;
                if (!emit(out.data(), made)) { ok = false; break; }
                if (r == MZ_STREAM_END) done = true;
                else if (!left && !s.avail_in && !made) ok = false;   // ran out of input
                else if (r != MZ_OK && r != MZ_BUF_ERROR) ok = false;
            }
            mz_inflateEnd(&s);
        } else {   // 93: Zstandard
            if (!zstd && !(zstd = ZSTD_createDCtx())) { err = L"Out of memory."; return false; }
            ZSTD_DCtx_reset(zstd, ZSTD_reset_session_only);
            ZSTD_DCtx_setParameter(zstd, ZSTD_d_windowLogMax, ZSTD_WINDOWLOG_MAX);   // allow --long archives
            while (ok && left) {
                if (!alive()) { stopped = true; break; }
                size_t n;
                if (!readChunk(n)) { ok = false; break; }
                ZSTD_inBuffer ib{in.data(), n, 0};
                while (ok && ib.pos < ib.size) {
                    ZSTD_outBuffer ob{out.data(), kChunk, 0};
                    size_t r = ZSTD_decompressStream(zstd, &ob, &ib);
                    ok = !ZSTD_isError(r) && emit(out.data(), ob.pos);
                }
            }
            for (bool more = ok && !stopped; more;) {   // drain anything still buffered
                ZSTD_inBuffer ib{nullptr, 0, 0};
                ZSTD_outBuffer ob{out.data(), kChunk, 0};
                size_t r = ZSTD_decompressStream(zstd, &ob, &ib);
                ok = !ZSTD_isError(r) && emit(out.data(), ob.pos);
                more = ok && ob.pos == kChunk;
            }
        }
        prog.punching = false;

        if (stopped || !ok || written != e.size || crc != e.crc) {
            file.Close();
            DeleteFileW(tmp.c_str());
            if (stopped) return false;
            if (why.empty()) why = L"it failed its CRC check, so the zip may be damaged";
            if (punched) {
                err = Lost(e, why + L",");
            } else {
                if (punch) { DeleteFileW(LostMarkerPath(zipPath).c_str()); markerAt = UINT64_MAX; }
                why[0] = wchar_t(towupper(why[0]));
                err = L"\"" + e.name + L"\": " + why + L". Nothing was removed for this file.";
            }
            return false;
        }
        if (!FlushFileBuffers(file.h)) {
            err = (punched ? Lost(e, L"it couldn't be saved,") : L"Couldn't save \"" + e.name + L"\": ") + ErrText(GetLastError());
            return false;
        }
        FILETIME local, ft;
        if (DosDateTimeToFileTime(e.dosDate, e.dosTime, &local) && LocalFileTimeToFileTime(&local, &ft))
            SetFileTime(file.h, &ft, nullptr, &ft);
        file.Close();
        return Place(e, tmp, err);
    }

    // Moves a finished temp file over the entry's path and makes that stick.
    bool Place(const ZipEntry& e, const std::wstring& tmp, std::wstring& err) {
        SetFileAttributesW(e.path.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (!MoveFileExW(tmp.c_str(), e.path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            err = L"Couldn't save \"" + e.name + L"\": " + ErrText(GetLastError());
            return false;
        }
        FlushDir(ParentOf(e.path));
        DWORD keep = e.attrs & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
        if (keep && DosAttrs(e)) SetFileAttributesW(e.path.c_str(), keep);
        return true;
    }

    bool Truncate(uint64_t at, std::wstring& err) {
        if (at >= zipSize) return true;
        FILE_END_OF_FILE_INFO eof{};
        eof.EndOfFile.QuadPart = LONGLONG(at);
        if (!SetFileInformationByHandle(zip, FileEndOfFileInfo, &eof, sizeof eof) || !FlushFileBuffers(zip)) {
            err = L"Couldn't shrink the zip: " + ErrText(GetLastError());
            return false;
        }
        // Moving the end alone keeps the clusters reserved until the handle closes;
        // shrinking the allocation too gives them back to the drive right away. Only
        // ever shrink it: a sparse zip with freed ranges has less allocated than `at`.
        FILE_STANDARD_INFO info{};
        if (GetFileInformationByHandleEx(zip, FileStandardInfo, &info, sizeof info) &&
            uint64_t(info.AllocationSize.QuadPart) > at) {
            FILE_ALLOCATION_INFO alloc{};
            alloc.AllocationSize.QuadPart = LONGLONG(at);
            SetFileInformationByHandle(zip, FileAllocationInfo, &alloc, sizeof alloc);
        }
        zipSize = at;
        prog.zipBytes = at;
        return true;
    }
};

}  // namespace

bool Extractor::Run(const std::wstring& zipIn, const std::wstring& destIn, ProgressFn onProgress, LogFn onFile,
                    std::wstring& err) {
    err.clear();
    std::wstring zip = FullPath(zipIn), dest = FullPath(destIn);
    Handle zh(CreateFileW(zip.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr));
    if (!zh) {
        DWORD code = GetLastError();
        err = code == ERROR_SHARING_VIOLATION
                  ? L"Another app has the zip open. Close WinRAR, 7-Zip or anything else using it, then try again."
                  : L"Couldn't open the zip: " + ErrText(code);
        return false;
    }
    ZipIndex idx;
    bool resuming = false;
    if (!LoadIndex(zip, zh.h, idx, resuming, err)) return false;
    std::vector<Problem> bad;
    size_t badCount = 0;
    bool singleRoot;
    MapPaths(idx, dest, zip, {bad, badCount}, singleRoot);
    if (badCount) {
        err = L"\"" + bad[0].name + L"\" can't be extracted: " + bad[0].reason + L".";
        return false;
    }
    if (ExistingAncestor(dest) != dest && !MakeDirs(Extended(dest))) {
        err = L"Couldn't create the destination folder: " + ErrText(GetLastError());
        return false;
    }

    Job r(zh.h, zip, dest);
    if (resuming) {
        ReadLostMarker(zip, r.markerAt);
    } else {
        DeleteFileW(LostMarkerPath(zip).c_str());
        if (!WriteSidecar(zip, zh.h, idx, dest, err)) return false;
    }
    VolumeInfo zipVol = InfoOf(ParentOf(zip));
    r.punchUnit = std::max<uint64_t>(64 << 10, zipVol.cluster * 16);
    bool sparse = SameVolume(zip, dest) && zipVol.sparse;
    uint64_t forced = PunchOverride();
    r.onProgress = std::move(onProgress);
    r.keepGoing = [this] { return WaitIfPaused(); };
    for (auto& e : idx.entries) {
        r.prog.total++;
        if (!e.isDir && !e.path.empty()) r.prog.bytesTotal += e.size;
    }
    r.prog.zipBytes = r.zipSize;
    r.prog.freeBytes = FreeOn(dest);
    r.onProgress(r.prog);

    for (size_t i = idx.entries.size(); i-- > 0;) {
        const ZipEntry& e = idx.entries[i];
        if (!WaitIfPaused()) return false;
        r.prog.current = e.name;
        if (e.isDir) {
            if (!MakeDirs(e.path)) { err = L"Couldn't create folder \"" + e.name + L"\": " + ErrText(GetLastError()); return false; }
        } else if (!e.shadowed) {
            if (!MakeDirs(ParentOf(e.path))) { err = L"Couldn't create folder for \"" + e.name + L"\": " + ErrText(GetLastError()); return false; }
            std::wstring tmp = e.path + kTemp;
            bool gone = e.offset >= r.zipSize;
            bool lost = e.offset == r.markerAt;
            auto st = r.Matches(e.path, e);
            if (st != Job::Check::Ok && st != Job::Check::Stopped) {
                // A finished temp file whose rename didn't survive a crash is still good.
                if (r.Matches(tmp, e) == Job::Check::Ok) {
                    if (!r.Place(e, tmp, err)) return false;
                    st = Job::Check::Ok;
                }
            }
            if (st == Job::Check::Stopped) return false;
            DeleteFileW(tmp.c_str());
            if (st == Job::Check::Ok) {
                onFile(e.name, e.size, FileResult::Verified);
            } else if (lost) {
                r.Tick(e.size);
                onFile(e.name, e.size, FileResult::Lost);
            } else if (gone) {
                err = L"\"" + e.name + L"\" is no longer in the zip, and the copy in the folder is missing or damaged.";
                return false;
            } else {
                if (st == Job::Check::Bad && !RemoveFile(e.path)) {
                    err = L"Couldn't replace \"" + e.name + L"\": " + ErrText(GetLastError());
                    return false;
                }
                uint64_t freeNow = FreeOn(dest);
                bool fits = freeNow >= e.size + kMargin;
                bool punch = sparse && (!fits || e.size >= forced);
                if (!fits && (!punch || freeNow + e.csize < e.size + kMargin + kWindow)) {
                    err = L"Not enough free space for \"" + e.name + L"\". Free up a little space, then press Resume.";
                    return false;
                }
                if (!r.Extract(e, punch, err)) return false;
                onFile(e.name, e.size, FileResult::Written);
            }
        }
        if (!r.Truncate(e.offset, err)) return false;
        if (r.markerAt == e.offset) {
            DeleteFileW(LostMarkerPath(zip).c_str());
            r.markerAt = UINT64_MAX;
        }
        r.prog.done++;
        r.prog.freeBytes = FreeOn(dest);
        r.onProgress(r.prog);
    }
    zh.Close();
    if (!DeleteFileW(zip.c_str())) { err = L"Everything was extracted, but the empty zip couldn't be deleted: " + ErrText(GetLastError()); return false; }
    DeleteFileW(SidecarPath(zip).c_str());
    DeleteFileW(LostMarkerPath(zip).c_str());
    return true;
}
