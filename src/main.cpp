// ZeroZip: a native Win32 window drawn with Direct2D in the Windows 11 style,
// driving the engine on a worker thread.
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <exception>
#include <new>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "engine.h"
#include "gfx.h"
#include "resource.h"

namespace {

constexpr UINT WM_APP_ANALYSIS = WM_APP + 1;   // lParam: AnalysisResult*
constexpr UINT WM_APP_DONE = WM_APP + 2;       // lParam: RunResult*
constexpr UINT_PTR kTick = 1;                  // progress refresh while extracting
constexpr UINT_PTR kAnim = 2;                  // busy bar while a zip is read
constexpr double GiB = 1024.0 * 1024 * 1024;

// Segoe Fluent Icons / Segoe MDL2 Assets
constexpr wchar_t kGlyphDownload[] = L"\xE896", kGlyphFolder[] = L"\xE8B7", kGlyphBack[] = L"\xE72B",
                  kGlyphCheck[] = L"\xE73E", kGlyphError[] = L"\xEB90", kGlyphWarning[] = L"\xE7BA",
                  kGlyphPause[] = L"\xE769", kGlyphPlay[] = L"\xE768", kGlyphArrow[] = L"\xE72A";

enum class Screen { Home, Reading, Ready, Running, End };
enum class EndKind { Done, Stopped, Problem, CantOpen };
enum class Modal { None, Start, Stop };
enum Id { kNone, kBrowse, kBack, kChange, kCancel, kExtract, kPause, kStop, kOpen, kAnother, kResume, kRetry, kNo, kYes };
enum class Kind { Primary, Warn, Ghost, Icon };
enum class Tone { Good, Warn, Bad };

struct Hit { int id; D2D1_RECT_F r; bool enabled; };
struct AnalysisResult { unsigned seq; bool ok; Analysis a; std::wstring err; };
struct RunResult { bool ok, stopped; std::wstring err; };

HWND g_hwnd;
Gfx g_gfx;
Microsoft::WRL::ComPtr<ITaskbarList3> g_taskbar;

Screen g_screen = Screen::Home;
Modal g_modal = Modal::None;
EndKind g_end = EndKind::Done;
std::wstring g_endText, g_zip, g_zipDest;   // the zip being read, and the folder asked for
Analysis g_a;
std::atomic<int> g_reading{0};
unsigned g_readSeq = 0;                     // only the latest read's result is used
ULONGLONG g_animStart = 0;

Extractor g_ex;
std::thread g_worker;
bool g_running = false, g_paused = false, g_closing = false;
std::mutex g_mu;
Progress g_shared;                 // written by the worker
Progress g_prog;                   // the window's copy
std::deque<std::pair<ULONGLONG, uint64_t>> g_samples;
int g_titlePct = -1;

std::vector<Hit> g_hits;
int g_hover = kNone, g_press = kNone, g_focus = kNone;
bool g_keyboard = false;           // show the focus ring only after Tab
float g_scroll = 0, g_scrollMax = 0, g_view = 0;
bool g_dragging = false;           // dragging the scroll thumb
float g_dragFrom = 0, g_dragScroll = 0;

// ---- text ------------------------------------------------------------------------

std::wstring Fmt(double b, int decimals = -1) {
    bool neg = b < 0;
    b = std::fabs(b);
    struct Unit { const wchar_t* name; double size; } units[] = {{L"GB", GiB}, {L"MB", 1024.0 * 1024}, {L"KB", 1024.0}};
    wchar_t s[32];
    for (auto& u : units) {
        if (b < u.size * 0.95) continue;
        double v = b / u.size;
        swprintf_s(s, L"%s%.*f %s", neg ? L"−" : L"", decimals >= 0 ? decimals : (v >= 100 ? 0 : 1), v, u.name);
        return s;
    }
    swprintf_s(s, L"%s%.0f bytes", neg ? L"−" : L"", b);
    return s;
}

std::wstring Num(uint64_t n) {
    std::wstring s = std::to_wstring(n);
    for (int i = int(s.size()) - 3; i > 0; i -= 3) s.insert(size_t(i), 1, L',');
    return s;
}

std::wstring Words(uint64_t ms) {
    uint64_t s = (ms + 500) / 1000, m = s / 60, h = m / 60;
    wchar_t b[32];
    if (s < 60) swprintf_s(b, L"%llus", s);
    else if (!h) swprintf_s(b, L"%llum %llus", m, s % 60);
    else swprintf_s(b, L"%lluh %llum", h, m % 60);
    return b;
}

std::wstring Leaf(const std::wstring& name) {
    std::wstring n = name;
    while (!n.empty() && (n.back() == L'/' || n.back() == L'\\')) n.pop_back();
    size_t k = n.find_last_of(L"/\\");
    return k == std::wstring::npos ? n : n.substr(k + 1);
}

std::wstring Plural(size_t n, const wchar_t* one, const wchar_t* many) {
    return (n == 1 ? std::wstring(L"1 ") + one : Num(n) + L" " + many);
}

// A path cut down in the middle to fit about `width` DIPs of caption text.
std::wstring Compact(const std::wstring& path, float width) {
    wchar_t out[MAX_PATH];
    UINT chars = UINT(std::clamp(width / 6.6f, 12.f, float(MAX_PATH - 1)));
    if (path.size() < chars || !PathCompactPathExW(out, path.c_str(), chars, 0)) return path;
    return out;
}

std::wstring VolumeName() { return g_a.volume.empty() ? L"the drive" : g_a.volume; }

// ---- drawing pieces ----------------------------------------------------------------

D2D1_RECT_F R(float x, float y, float w, float h) { return D2D1::RectF(x, y, x + w, y + h); }

// A card: a faint outer shell holding a darker panel, like a plate sitting in a tray.
// Returns the inner panel.
D2D1_RECT_F Card(const D2D1_RECT_F& r) {
    const Theme& t = g_gfx.theme;
    g_gfx.Fill(r, t.shell, 20);
    g_gfx.Stroke(r, t.shellLine, 20);
    D2D1_RECT_F in = {r.left + 6, r.top + 6, r.right - 6, r.bottom - 6};
    g_gfx.Gradient(in, t.coreTop, t.coreBottom, 14, true);
    g_gfx.Line({in.left + 14, in.top + 1}, {in.right - 14, in.top + 1}, t.coreHighlight);
    return in;
}

constexpr float kButtonH = 44;

float ButtonWidth(const std::wstring& label, Kind kind, bool glyph = false) {
    float text = g_gfx.TextWidth(label, Font::BodyStrong);
    return kind == Kind::Primary || glyph ? 22 + text + 12 + 32 + 6 : 22 + text + 22;
}

// Pill buttons. A primary one, or one with a glyph, carries it in a small round tip on the right.
void Button(int id, float x, float y, const std::wstring& label, Kind kind, bool enabled = true,
            const wchar_t* glyph = nullptr) {
    const Theme& t = g_gfx.theme;
    bool tip = kind == Kind::Primary || kind == Kind::Warn || glyph;
    float w = kind == Kind::Icon ? kButtonH : ButtonWidth(label, kind, glyph != nullptr);
    D2D1_RECT_F r = R(x, y, w, kButtonH);
    float dy = g_gfx.OffsetY();
    g_hits.push_back({id, D2D1::RectF(r.left, r.top + dy, r.right, r.bottom + dy), enabled});
    bool hover = enabled && g_hover == id, press = hover && g_press == id;
    float radius = kButtonH / 2;
    if (press) r = D2D1::RectF(r.left + 1, r.top + 1, r.right - 1, r.bottom - 1);   // a slight press-in
    D2D1_COLOR_F ink = t.text1, tipBg = D2D1::ColorF(1, 1, 1, .06f);
    if (kind == Kind::Primary || kind == Kind::Warn) {
        D2D1_COLOR_F bg = kind == Kind::Warn ? t.warn : hover ? t.accentHover : t.accent;
        if (!enabled) bg.a = .3f;
        g_gfx.Fill(r, bg, radius);
        g_gfx.Line({r.left + radius, r.top + 1}, {r.right - radius, r.top + 1}, D2D1::ColorF(1, 1, 1, enabled ? .45f : .1f));
        ink = t.accentInk;
        tipBg = D2D1::ColorF(t.accentInk.r, t.accentInk.g, t.accentInk.b, .12f);
    } else {
        g_gfx.Fill(r, hover ? t.ghostHover : t.ghost, radius);
        g_gfx.Stroke(r, t.ghostLine, radius);
        if (!enabled) ink = t.textDisabled;
    }
    float h = r.bottom - r.top;
    if (kind == Kind::Icon) {
        g_gfx.Text(glyph, Font::Icon16, R(r.left, r.top + (h - 16) / 2, r.right - r.left, 18), ink, Align::Center);
    } else {
        g_gfx.Text(label, Font::BodyStrong, R(r.left + 22, r.top + (h - 20) / 2, r.right - r.left - 22, 20), ink);
        if (tip) {
            D2D1_POINT_2F c = {r.right - 6 - 16, r.top + h / 2};
            float nudge = hover ? 1.5f : 0;   // the tip leans forward on hover
            g_gfx.Circle({c.x + nudge, c.y}, 16, tipBg);
            g_gfx.Text(glyph ? glyph : kGlyphArrow, Font::Icon16, R(c.x - 9 + nudge, c.y - 8, 18, 18), ink, Align::Center);
        }
    }
    if (g_keyboard && g_focus == id)
        g_gfx.Stroke(D2D1::RectF(r.left - 3, r.top - 3, r.right + 3, r.bottom + 3), t.accentLine, radius + 3, 2);
}

// A coloured pill with a glyph, for the verdict and notices. Returns its height.
float Badge(float x, float y, Tone tone, const std::wstring& text) {
    const Theme& t = g_gfx.theme;
    D2D1_COLOR_F fg = t.accent, tint = t.accentTint, line = t.accentLine;
    const wchar_t* glyph = kGlyphCheck;
    if (tone == Tone::Warn) { fg = t.warn; tint = t.warnTint; line = t.warnLine; glyph = kGlyphWarning; }
    if (tone == Tone::Bad) { fg = t.danger; tint = t.dangerTint; line = t.dangerLine; glyph = kGlyphError; }
    float w = g_gfx.TextWidth(text, Font::BodyStrong) + 34 + 14;
    D2D1_RECT_F r = R(x, y, w, 30);
    g_gfx.Fill(r, tint, 15);
    g_gfx.Stroke(r, line, 15);
    g_gfx.Text(glyph, Font::Icon16, R(x + 11, y + 7, 18, 18), fg);
    g_gfx.Text(text, Font::BodyStrong, R(x + 34, y + 5, w - 34, 20), fg);
    return 30;
}

// A notice: a badge, and if there's more to say, a line of text under it.
float Notice(float x, float y, float w, Tone tone, const std::wstring& title, const std::wstring& text) {
    float h = Badge(x, y, tone, title);
    if (!text.empty()) h += 8 + g_gfx.Text(text, Font::Body, R(x + 4, y + h + 8, w - 4, 200), g_gfx.theme.text2, Align::Left, true);
    return h;
}

// A row of figures in a card, each with a small label under it.
void Figures(float x, float y, float w, const std::vector<std::pair<std::wstring, std::wstring>>& cells) {
    D2D1_RECT_F in = Card(R(x, y, w, 92));
    float cw = (in.right - in.left) / float(cells.size());
    for (size_t i = 0; i < cells.size(); i++) {
        float cx = in.left + cw * float(i);
        if (i) g_gfx.Line({cx, in.top + 16}, {cx, in.bottom - 16}, g_gfx.theme.hair);
        g_gfx.Text(cells[i].first, Font::MonoStrong, R(cx + 18, in.top + 16, cw - 28, 26), g_gfx.theme.text1);
        g_gfx.Text(cells[i].second, Font::Caption, R(cx + 18, in.top + 46, cw - 28, 16), g_gfx.theme.text3);
    }
}

// The ZeroZip mark: a mint tile with an opened zipper.
void Mark(float x, float y) {
    const Theme& t = g_gfx.theme;
    g_gfx.Gradient(R(x, y, 22, 22), t.accent2, t.accent, 6, true);
    g_gfx.Fill(R(x + 10, y + 3, 2, 8), t.accentInk, 1);
    g_gfx.Fill(R(x + 8.5f, y + 10, 5, 4), t.accentInk, 1.5f);
    g_gfx.Line({x + 11, y + 14}, {x + 6.5f, y + 20}, t.accentInk, 1.6f);
    g_gfx.Line({x + 11, y + 14}, {x + 15.5f, y + 20}, t.accentInk, 1.6f);
}

void Header(float W) {
    const Theme& t = g_gfx.theme;
    Mark(28, 21);
    g_gfx.Text(L"ZeroZip", Font::Brand, R(58, 21, 120, 22), t.text1);
    std::wstring status = L"Ready";
    D2D1_COLOR_F dot = t.text3;
    if (g_screen == Screen::Reading) status = L"Reading zip";
    if (g_screen == Screen::Running) {
        status = g_ex.Stopped() ? L"Stopping" : g_paused ? L"Paused" : L"Extracting · " + std::to_wstring(std::max(0, g_titlePct)) + L"%";
        dot = g_paused || g_ex.Stopped() ? t.warn : t.accent;
    }
    if (g_screen == Screen::End) {
        status = g_end == EndKind::Done ? L"Done" : g_end == EndKind::Stopped ? L"Stopped" : L"Needs attention";
        dot = g_end == EndKind::Done ? t.accent : g_end == EndKind::Stopped ? t.warn : t.danger;
    }
    float sw = g_gfx.TextWidth(status, Font::Caption) + 34;
    D2D1_RECT_F pill = R(W - 28 - sw, 18, sw, 28);
    g_gfx.Fill(pill, t.shell, 14);
    g_gfx.Stroke(pill, t.shellLine, 14);
    g_gfx.Circle({pill.left + 14, pill.top + 14}, 3.5f, dot);
    g_gfx.Text(status, Font::Caption, R(pill.left + 24, pill.top + 6, sw - 24, 16), g_running ? t.text1 : t.text2);
}

// ---- screens ---------------------------------------------------------------------------

bool CanExtract() { return g_a.problemCount == 0 && g_a.minFree >= int64_t(g_a.margin) && g_reading == 0; }

constexpr float kTop = 92;   // where page content starts, below the header

struct Column { float x, w; };

Column Content(float W) {
    float w = std::min(640.f, W - 64);
    return {std::floor((W - w) / 2), w};
}

float DrawHome(float W, float H) {
    const Theme& t = g_gfx.theme;
    float w = std::min(560.f, W - 64), h = 280, x = std::floor((W - w) / 2);
    float y = std::max(kTop, std::floor(64 + (H - 64 - h) / 2 - 16));
    D2D1_RECT_F in = Card(R(x, y, w, h));
    g_gfx.Stroke(D2D1::RectF(in.left + 10, in.top + 10, in.right - 10, in.bottom - 10), t.dashed, 10, 1, true);
    float cx = x + w / 2, ty = y + 54;
    D2D1_RECT_F tile = R(cx - 32, ty, 64, 64);
    g_gfx.Fill(tile, t.accentTint, 18);
    g_gfx.Stroke(tile, t.accentLine, 18);
    g_gfx.Text(kGlyphDownload, Font::Icon28, R(tile.left, ty + 17, 64, 32), t.accent, Align::Center);
    g_gfx.Text(L"Drop a zip here", Font::Subtitle, R(x, ty + 86, w, 28), t.text1, Align::Center);
    std::wstring choose = L"Choose zip";
    Button(kBrowse, cx - ButtonWidth(choose, Kind::Primary) / 2, ty + 132, choose, Kind::Primary);
    return y + h;
}

void BusyBar(float x, float y, float w) {
    const Theme& t = g_gfx.theme;
    g_gfx.Fill(R(x, y, w, 8), t.track, 4);
    float phase = float((GetTickCount64() - g_animStart) % 1600) / 1600.f;
    float seg = w * .32f, pos = -seg + phase * (w + seg);
    float a = std::max(x, x + pos), b = std::min(x + w, x + pos + seg);
    if (b > a) g_gfx.Gradient(D2D1::RectF(a, y, b, y + 8), t.accent, t.accent2, 4);
}

float DrawReading(float W) {
    auto [x, w] = Content(W);
    g_gfx.Text(Leaf(g_zip), Font::Title, R(x, kTop, w, 40), g_gfx.theme.text1);
    g_gfx.Text(L"Reading the zip…", Font::Body, R(x, kTop + 44, w, 20), g_gfx.theme.text2);
    BusyBar(x, kTop + 84, w);
    Button(kCancel, x, kTop + 116, L"Cancel", Kind::Ghost);
    return kTop + 160;
}

float DrawReady(float W) {
    const Theme& t = g_gfx.theme;
    const Analysis& a = g_a;
    auto [x, w] = Content(W);
    float y = kTop;
    Button(kBack, x - 4, y, L"", Kind::Icon, true, kGlyphBack);
    g_gfx.Text(Leaf(a.zip), Font::Title, R(x + 52, y - 2, w - 52, 40), t.text1);
    g_gfx.Text(Fmt(double(a.zipBytes)) + L" → " + Fmt(double(a.unpacked)) + L"  ·  " + Plural(a.files, L"file", L"files"),
               Font::Mono, R(x + 54, y + 38, w - 54, 18), t.text2);
    y += 80;

    std::wstring vol = VolumeName();
    if (a.problemCount) {
        std::wstring msg;
        for (size_t i = 0; i < std::min<size_t>(a.problems.size(), 3); i++)
            msg += (i ? L"\n" : L"") + Leaf(a.problems[i].name) + L": " + a.problems[i].reason;
        y += Notice(x, y, w, Tone::Bad, L"Can't extract this zip", msg) + 14;
    } else if (a.minFree >= int64_t(a.margin)) {
        y += Notice(x, y, w, Tone::Good, L"Fits on " + vol, L"") + 14;
    } else {
        y += Notice(x, y, w, Tone::Warn, L"Not enough space",
                    L"Free up " + Fmt(double(int64_t(a.margin) - a.minFree)) + L" more on " + vol + L".") + 14;
    }
    if (!a.lost.empty())
        y += Notice(x, y, w, Tone::Warn, Leaf(a.lost) + L" was lost last time and will be skipped", L"") + 14;
    else if (a.resuming)
        y += Notice(x, y, w, Tone::Good, L"Continuing where it stopped", L"") + 14;
    if (!a.sameVolume)
        y += Notice(x, y, w, Tone::Warn, L"The zip and the folder are on different drives",
                    L"The folder's drive needs room for every file.") + 14;
    if (a.punchFiles) {
        std::wstring names;
        for (auto& n : a.punchNames) names += (names.empty() ? L"" : L", ") + Leaf(n);
        if (a.punchFiles > a.punchNames.size()) names += L"…";
        y += Notice(x, y, w, Tone::Warn, Plural(a.punchFiles, L"file is", L"files are") + L" bigger than the free space", names) + 14;
    }
    y += 6;

    std::wstring change = L"Change";
    float cw = ButtonWidth(change, Kind::Ghost);
    D2D1_RECT_F in = Card(R(x, y, w, 92));
    g_gfx.Text(kGlyphFolder, Font::Icon20, R(in.left + 20, in.top + 29, 24, 24), t.text2);
    g_gfx.Text(L"Extract to", Font::Caption, R(in.left + 56, in.top + 18, w - 120 - cw, 16), t.text3);
    g_gfx.Text(Compact(a.dest, in.right - in.left - 100 - cw), Font::Mono, R(in.left + 56, in.top + 38, in.right - in.left - 90 - cw, 18), t.text1);
    Button(kChange, in.right - 18 - cw, in.top + 18, change, Kind::Ghost);
    y += 92 + 24;

    std::wstring extract = L"Extract";
    float ew = ButtonWidth(extract, Kind::Primary), nw = ButtonWidth(L"Cancel", Kind::Ghost);
    Button(kExtract, x + w - ew, y, extract, Kind::Primary, CanExtract());
    Button(kCancel, x + w - ew - 10 - nw, y, L"Cancel", Kind::Ghost);
    return y + kButtonH;
}

float DrawRunning(float W) {
    const Theme& t = g_gfx.theme;
    const Progress& p = g_prog;
    auto [x, w] = Content(W);
    bool stopping = g_ex.Stopped();
    float y = kTop;
    g_gfx.Text(stopping ? L"Stopping after this file" : g_paused ? L"Paused" : L"Extracting", Font::Title, R(x, y, w, 40), t.text1);
    g_gfx.Text(Leaf(g_a.zip), Font::Body, R(x, y + 42, w, 20), t.text2);
    y += 80;

    double frac = p.bytesTotal ? double(p.bytesDone) / double(p.bytesTotal) : p.total ? double(p.done) / double(p.total) : 0;
    frac = std::clamp(frac, 0.0, 1.0);
    wchar_t pct[16];
    double shown = frac * 100;
    if (shown >= 99.95 && p.done < p.total) shown = 99.9;
    swprintf_s(pct, shown < 10 ? L"%.1f" : L"%.0f", shown);
    D2D1_RECT_F in = Card(R(x, y, w, 196));
    float ix = in.left + 24, iw = in.right - in.left - 48;
    float pw = g_gfx.TextWidth(pct, Font::MonoLarge);
    g_gfx.Text(pct, Font::MonoLarge, R(ix - 4, in.top + 14, pw + 4, 84), t.text1);
    g_gfx.Text(L"%", Font::Subtitle, R(ix + pw + 2, in.top + 26, 30, 28), t.text2);
    D2D1_COLOR_F from = g_paused ? D2D1::ColorF(0.78f, 0.6f, 0.23f) : t.accent, to = g_paused ? t.warn : t.accent2;
    std::wstring state = g_paused ? L"Paused" : p.verifying ? L"Checking" : p.punching ? L"Freeing as it goes" : L"Extracting";
    float sw = g_gfx.TextWidth(state, Font::Caption) + 20;
    D2D1_RECT_F chip = R(ix, in.top + 108, sw, 22);
    g_gfx.Fill(chip, g_paused ? t.warnTint : t.accentTint, 11);
    g_gfx.Stroke(chip, g_paused ? t.warnLine : t.accentLine, 11);
    g_gfx.Text(state, Font::Caption, R(chip.left + 10, chip.top + 3, sw - 10, 16), g_paused ? t.warn : t.accent);
    g_gfx.Text(p.current.empty() ? L"Starting…" : Leaf(p.current), Font::Mono, R(ix + sw + 12, in.top + 110, iw - sw - 12, 18), t.text2);
    D2D1_RECT_F track = R(ix, in.top + 146, iw, 10);
    g_gfx.Fill(track, t.track, 5);
    if (frac > 0) g_gfx.Gradient(R(ix, track.top, std::max(10.f, iw * float(frac)), 10), from, to, 5);
    y += 196 + 12;

    std::wstring speed = L"–", left = L"–";
    if (g_samples.size() > 1) {
        auto& f = g_samples.front();
        auto& l = g_samples.back();
        double dt = double(l.first - f.first) / 1000.0;
        double rate = dt > .5 ? double(l.second - f.second) / dt : 0;
        if (rate > 0) {
            speed = Fmt(rate) + L"/s";
            left = Words(uint64_t(double(p.bytesTotal - std::min(p.bytesTotal, p.bytesDone)) / rate * 1000));
        }
    }
    if (g_paused) left = L"Paused";
    Figures(x, y, w, {{speed, L"Speed"}, {left, L"Time left"}, {Fmt(double(p.freeBytes)), L"Free on " + VolumeName()}});
    y += 92 + 24;

    std::wstring pause = g_paused ? L"Resume" : L"Pause";
    float sw2 = ButtonWidth(L"Stop", Kind::Ghost), pw2 = ButtonWidth(pause, Kind::Ghost, true);
    Button(kPause, x + w - sw2 - 10 - pw2, y, pause, Kind::Ghost, !stopping, g_paused ? kGlyphPlay : kGlyphPause);
    Button(kStop, x + w - sw2, y, L"Stop", Kind::Ghost, !stopping);
    return y + kButtonH;
}

float DrawEnd(float W, float H) {
    const Theme& t = g_gfx.theme;
    float w = std::min(520.f, W - 64), x = std::floor((W - w) / 2);
    D2D1_COLOR_F fg = t.accent, tint = t.accentTint, line = t.accentLine;
    const wchar_t* glyph = kGlyphCheck;
    std::wstring title;
    std::vector<std::pair<int, std::wstring>> buttons;
    switch (g_end) {
        case EndKind::Done:
            title = L"Done";
            buttons = {{kOpen, L"Open folder"}, {kAnother, L"Extract another"}};
            break;
        case EndKind::Stopped:
            fg = t.warn; tint = t.warnTint; line = t.warnLine; glyph = kGlyphPause;
            title = L"Stopped";
            buttons = {{kResume, L"Resume"}, {kAnother, L"Close"}};
            break;
        case EndKind::Problem:
            fg = t.danger; tint = t.dangerTint; line = t.dangerLine; glyph = kGlyphWarning;
            title = L"Stopped";
            buttons = {{kResume, L"Resume"}, {kAnother, L"Close"}};
            break;
        case EndKind::CantOpen:
            fg = t.danger; tint = t.dangerTint; line = t.dangerLine; glyph = kGlyphError;
            title = L"Can't open this zip";
            buttons = {{kRetry, L"Try again"}, {kAnother, L"Choose another zip"}};
            break;
    }
    float height = 76 + 28 + 44 + g_gfx.TextHeight(g_endText, Font::Body, w) + 32 + kButtonH;
    float y = std::max(kTop, std::floor(64 + (H - 64 - height) / 2 - 16));
    D2D1_POINT_2F c = {x + w / 2, y + 38};
    g_gfx.Glow(c, 90, D2D1::ColorF(fg.r, fg.g, fg.b, .12f));
    g_gfx.Circle(c, 38, tint);
    D2D1_RECT_F ring = D2D1::RectF(c.x - 38, c.y - 38, c.x + 38, c.y + 38);
    g_gfx.Stroke(ring, line, 38);
    g_gfx.Text(glyph, Font::Icon28, R(c.x - 20, c.y - 15, 40, 32), fg, Align::Center);
    y += 76 + 28;
    g_gfx.Text(title, Font::Title, R(x, y, w, 40), t.text1, Align::Center);
    y += 44;
    y += g_gfx.Text(g_endText, Font::Body, R(x, y, w, 200), t.text2, Align::Center, true) + 32;
    float total = 0;
    for (size_t i = 0; i < buttons.size(); i++) total += ButtonWidth(buttons[i].second, i == 0 ? Kind::Primary : Kind::Ghost) + 10;
    float bx = x + (w - total + 10) / 2;
    for (size_t i = 0; i < buttons.size(); i++) {
        Kind kind = i == 0 ? Kind::Primary : Kind::Ghost;
        Button(buttons[i].first, bx, y, buttons[i].second, kind);
        bx += ButtonWidth(buttons[i].second, kind) + 10;
    }
    return y + kButtonH;
}

void DrawModal(float W, float H) {
    const Theme& t = g_gfx.theme;
    g_hits.clear();   // only the dialog takes input
    g_gfx.Fill(D2D1::RectF(0, 0, W, H), t.smoke);
    float w = std::min(460.f, W - 48), x = std::floor((W - w) / 2), tw = w - 12 - 56;
    std::wstring title, text, yes, no;
    if (g_modal == Modal::Start) {
        title = L"Extract and delete the zip?";
        text = L"The zip is used up as it extracts and is gone when it's done.";
        if (g_a.punchFiles)
            text += L" If the PC loses power while one of the " + Plural(g_a.punchFiles, L"big file", L"big files") +
                    L" is being extracted, that file is lost.";
        yes = L"Extract";
        no = L"Cancel";
    } else {
        title = L"Stop extracting?";
        text = L"You can pick up where it stopped by opening the zip again.";
        if (g_prog.punching) text += L" The file being extracted now is finished first.";
        yes = L"Stop";
        no = L"Keep going";
    }
    float textH = g_gfx.TextHeight(text, Font::Body, tw);
    float h = 12 + 28 + 28 + 12 + textH + 28 + kButtonH + 28, y = std::floor((H - h) / 2);
    D2D1_RECT_F in = Card(R(x, y, w, h));
    float ix = in.left + 28;
    g_gfx.Text(title, Font::Subtitle, R(ix, in.top + 28, tw, 28), t.text1);
    g_gfx.Text(text, Font::Body, R(ix, in.top + 68, tw, textH + 4), t.text2, Align::Left, true);
    Kind kind = g_modal == Modal::Start ? Kind::Primary : Kind::Warn;
    float yw = ButtonWidth(yes, kind), nw = ButtonWidth(no, Kind::Ghost), by = in.bottom - 28 - kButtonH;
    Button(kYes, in.right - 28 - yw, by, yes, kind);
    Button(kNo, in.right - 28 - yw - 10 - nw, by, no, Kind::Ghost);
}

void Paint() {
    if (!g_gfx.Begin()) return;
    g_hits.clear();
    D2D1_SIZE_F size = g_gfx.Size();
    float W = size.width, H = size.height;
    g_view = H;
    if (g_scroll > g_scrollMax) g_scroll = g_scrollMax;
    const Theme& t = g_gfx.theme;
    g_gfx.Glow({W * .05f, -H * .15f}, std::max(W, H) * .55f, D2D1::ColorF(t.accent.r, t.accent.g, t.accent.b, .10f));
    g_gfx.Glow({W * 1.0f, H * 1.1f}, std::max(W, H) * .5f, D2D1::ColorF(t.accent2.r, t.accent2.g, t.accent2.b, .06f));

    g_gfx.Translate(-g_scroll);
    Header(W);
    float bottom = 0;
    switch (g_screen) {
        case Screen::Home: bottom = DrawHome(W, H); break;
        case Screen::Reading: bottom = DrawReading(W); break;
        case Screen::Ready: bottom = DrawReady(W); break;
        case Screen::Running: bottom = DrawRunning(W); break;
        case Screen::End: bottom = DrawEnd(W, H); break;
    }
    g_gfx.Translate(g_scroll);
    // Only the visible part of a scrolled button can be clicked.
    for (auto& h : g_hits) {
        h.r.top = std::max(h.r.top, 0.f);
        h.r.bottom = std::min(h.r.bottom, H);
    }
    g_hits.erase(std::remove_if(g_hits.begin(), g_hits.end(), [](const Hit& h) { return h.r.bottom <= h.r.top; }),
                 g_hits.end());

    float scrollMax = std::max(0.f, bottom + 28 - H);
    if (scrollMax != g_scrollMax) {
        g_scrollMax = scrollMax;
        if (g_scroll > g_scrollMax) InvalidateRect(g_hwnd, nullptr, FALSE);   // content got shorter: draw again in range
    }
    if (g_scrollMax > 0) {
        float thumb = std::max(32.f, H * H / (H + g_scrollMax));
        float top = (H - thumb) * (g_scroll / g_scrollMax);
        g_gfx.Fill(R(W - (g_dragging ? 7.f : 5.f), top + 2, g_dragging ? 4.f : 2.f, thumb - 4), t.text3, 2);
    }
    if (g_modal != Modal::None) DrawModal(W, H);
    g_gfx.End();
}

// ---- window chrome -------------------------------------------------------------------

// ZeroZip always looks the same, so the title bar is coloured to match the page.
void StyleTitleBar() {
    BOOL dark = TRUE;
    if (FAILED(DwmSetWindowAttribute(g_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof dark)))
        DwmSetWindowAttribute(g_hwnd, 19, &dark, sizeof dark);   // the same switch on Windows 10 before 20H1
    COLORREF caption = RGB(7, 8, 10), text = RGB(200, 203, 209), border = RGB(28, 30, 34);
    DwmSetWindowAttribute(g_hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof caption);   // Windows 11 only
    DwmSetWindowAttribute(g_hwnd, DWMWA_TEXT_COLOR, &text, sizeof text);
    DwmSetWindowAttribute(g_hwnd, DWMWA_BORDER_COLOR, &border, sizeof border);
}

// ---- navigation ------------------------------------------------------------------------

void SetTaskbar(TBPFLAG state, uint64_t done = 0, uint64_t total = 0) {
    if (!g_taskbar) return;
    g_taskbar->SetProgressState(g_hwnd, state);
    if (total && state != TBPF_NOPROGRESS) g_taskbar->SetProgressValue(g_hwnd, done, total);
}

void Go(Screen s) {
    if (s == Screen::Home) {
        g_readSeq++;   // a read still going is no longer wanted
        KillTimer(g_hwnd, kAnim);
        if (!g_running) SetTaskbar(TBPF_NOPROGRESS);
    }
    g_screen = s;
    g_scroll = 0;
    g_focus = kNone;
    g_hover = g_press = kNone;
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

void SetTitle(const std::wstring& suffix) {
    std::wstring t = L"ZeroZip";
    if (!suffix.empty()) t += L" – " + suffix;
    SetWindowTextW(g_hwnd, t.c_str());
}

void ShowEnd(EndKind kind, std::wstring text) {
    g_end = kind;
    g_endText = std::move(text);
    SetTitle(L"");
    Go(Screen::End);
}

void Read(std::wstring zip, std::wstring dest) {
    if (g_running) return;
    g_zip = zip;
    g_zipDest = dest;
    unsigned seq = ++g_readSeq;
    SetTaskbar(TBPF_NOPROGRESS);
    g_animStart = GetTickCount64();
    SetTimer(g_hwnd, kAnim, 33, nullptr);
    Go(Screen::Reading);
    g_reading++;
    std::thread([zip, dest, seq] {
        auto* r = new AnalysisResult{seq, false, {}, {}};
        try {
            r->ok = Analyze(zip, dest, r->a, r->err);
        } catch (const std::bad_alloc&) {
            r->err = L"The zip's file list is too large to read.";
        } catch (const std::exception&) {
            r->err = L"The zip couldn't be read.";
        }
        if (!PostMessageW(g_hwnd, WM_APP_ANALYSIS, 0, (LPARAM)r)) delete r;
        g_reading--;
    }).detach();
}

void OnAnalysis(AnalysisResult* r) {
    // Only the latest request counts, and never while a run is going.
    if (!g_running && r->seq == g_readSeq && g_screen == Screen::Reading) {
        KillTimer(g_hwnd, kAnim);
        if (r->ok) {
            g_a = std::move(r->a);
            Go(Screen::Ready);
        } else {
            ShowEnd(EndKind::CantOpen, r->err);
        }
    }
    delete r;
    InvalidateRect(g_hwnd, nullptr, FALSE);   // a finished read may enable Extract
}

void Pull() {
    {
        std::lock_guard<std::mutex> l(g_mu);
        g_prog = g_shared;
    }
    ULONGLONG now = GetTickCount64();
    if (!g_paused) g_samples.push_back({now, g_prog.bytesDone});
    while (g_samples.size() > 2 && now - g_samples.front().first > 5000) g_samples.pop_front();
    if (g_prog.bytesTotal) {
        SetTaskbar(g_paused ? TBPF_PAUSED : TBPF_NORMAL, g_prog.bytesDone, g_prog.bytesTotal);
        int pct = int(double(g_prog.bytesDone) * 100 / double(g_prog.bytesTotal));
        if (pct != g_titlePct || g_paused) {
            g_titlePct = pct;
            SetTitle(g_paused ? L"Paused" : std::to_wstring(pct) + L"%");
        }
    }
}

void StartRun() {
    if (g_running || g_reading > 0) return;
    g_running = true;
    g_paused = false;
    g_prog = Progress();
    g_samples.clear();
    g_titlePct = -1;
    {
        std::lock_guard<std::mutex> l(g_mu);
        g_shared = Progress();
    }
    g_ex.Reset();
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);   // don't let the PC sleep mid-run
    ShutdownBlockReasonCreate(g_hwnd, L"ZeroZip is extracting a zip");
    SetTimer(g_hwnd, kTick, 100, nullptr);
    Go(Screen::Running);
    std::wstring zip = g_a.zip, dest = g_a.dest;
    g_worker = std::thread([zip, dest] {
        auto* r = new RunResult{false, false, {}};
        try {
            r->ok = g_ex.Run(
                zip, dest,
                [](const Progress& p) {
                    std::lock_guard<std::mutex> l(g_mu);
                    g_shared = p;
                },
                [](const std::wstring&, uint64_t, FileResult) {},
                r->err);
        } catch (const std::bad_alloc&) {
            r->err = L"ZeroZip ran out of memory. Everything done so far is kept; open the zip again to resume.";
        } catch (const std::exception&) {
            r->err = L"Something went wrong. Everything done so far is kept; open the zip again to resume.";
        }
        r->stopped = !r->ok && r->err.empty();
        if (!PostMessageW(g_hwnd, WM_APP_DONE, 0, (LPARAM)r)) delete r;
    });
}

void OnDone(RunResult* r) {
    if (g_worker.joinable()) g_worker.join();
    KillTimer(g_hwnd, kTick);
    SetThreadExecutionState(ES_CONTINUOUS);
    ShutdownBlockReasonDestroy(g_hwnd);
    Pull();
    g_running = false;
    g_paused = false;
    g_modal = Modal::None;
    SetTaskbar(r->ok || r->stopped ? TBPF_NOPROGRESS : TBPF_ERROR, 1, 1);
    if (r->ok) {
        ShowEnd(EndKind::Done, L"Extracted to " + Compact(g_a.dest, 330));
    } else if (r->stopped) {
        ShowEnd(EndKind::Stopped, L"Open the zip again any time to continue.");
    } else {
        FlashWindow(g_hwnd, TRUE);
        ShowEnd(EndKind::Problem, r->err);
    }
    delete r;
    if (g_closing) DestroyWindow(g_hwnd);
}

// ---- dialogs ---------------------------------------------------------------------------

std::wstring Pick(bool folder, const std::wstring& start) {
    Microsoft::WRL::ComPtr<IFileOpenDialog> d;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return {};
    if (folder) {
        DWORD opts = 0;
        d->GetOptions(&opts);
        d->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        d->SetTitle(L"Extract to");
        std::wstring dir = start;
        while (!dir.empty() && GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) {
            size_t k = dir.find_last_of(L'\\');
            dir = k == std::wstring::npos ? L"" : dir.substr(0, k);
        }
        Microsoft::WRL::ComPtr<IShellItem> at;
        if (!dir.empty() && SUCCEEDED(SHCreateItemFromParsingName(dir.c_str(), nullptr, IID_PPV_ARGS(&at)))) d->SetFolder(at.Get());
    } else {
        COMDLG_FILTERSPEC spec[] = {{L"Zip archives", L"*.zip"}, {L"All files", L"*.*"}};
        d->SetFileTypes(2, spec);
        d->SetTitle(L"Choose a zip to extract");
    }
    Microsoft::WRL::ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(d->Show(g_hwnd)) || FAILED(d->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        return {};
    std::wstring s = path;
    CoTaskMemFree(path);
    return s;
}

// ---- input -----------------------------------------------------------------------------

void Click(int id) {
    switch (id) {
        case kBrowse: {
            std::wstring z = Pick(false, L"");
            if (!z.empty()) Read(z, L"");
            break;
        }
        case kBack:
        case kCancel:
        case kAnother: Go(Screen::Home); break;   // also abandons a read in progress
        case kChange: {
            std::wstring d = Pick(true, g_a.dest);
            if (!d.empty()) Read(g_a.zip, d);
            break;
        }
        case kExtract: g_modal = Modal::Start; g_focus = kNone; break;
        case kPause:
            g_paused = !g_paused;
            g_ex.Pause(g_paused);
            g_samples.clear();   // speed restarts from here
            g_titlePct = -1;
            Pull();
            break;
        case kStop: g_modal = Modal::Stop; g_focus = kNone; break;
        case kOpen: ShellExecuteW(g_hwnd, L"open", g_a.dest.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case kResume: Read(g_a.zip, g_a.dest); break;
        case kRetry: Read(g_zip, g_zipDest); break;
        case kYes:
            if (g_modal == Modal::Start) {
                if (!CanExtract()) break;   // a read is still finishing; the dialog stays up
                g_modal = Modal::None;
                StartRun();
            } else {
                g_modal = Modal::None;
                g_paused = false;
                g_ex.Stop();
            }
            break;
        case kNo: g_modal = Modal::None; break;
    }
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

int HitAt(LPARAM lp) {
    float s = g_gfx.Scale(), x = float(GET_X_LPARAM(lp)) / s, y = float(GET_Y_LPARAM(lp)) / s;
    for (auto it = g_hits.rbegin(); it != g_hits.rend(); ++it)
        if (x >= it->r.left && x < it->r.right && y >= it->r.top && y < it->r.bottom) return it->enabled ? it->id : kNone;
    return kNone;
}

void ScrollTo(float y) {
    g_scroll = std::clamp(y, 0.f, g_scrollMax);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

void RefreshHover() {
    POINT p;
    GetCursorPos(&p);
    ScreenToClient(g_hwnd, &p);
    g_hover = HitAt(MAKELPARAM(p.x, p.y));
}

bool OnScrollbar(LPARAM lp) {
    float s = g_gfx.Scale();
    return g_scrollMax > 0 && float(GET_X_LPARAM(lp)) / s >= g_gfx.Size().width - 14 && float(GET_Y_LPARAM(lp)) / s < g_view;
}

int DefaultButton() {
    if (g_modal != Modal::None) return kYes;
    for (auto& h : g_hits)
        if (h.enabled && (h.id == kBrowse || h.id == kExtract || h.id == kOpen || h.id == kResume || h.id == kRetry)) return h.id;
    return kNone;
}

void MoveFocus(bool back) {
    std::vector<int> ids;
    for (auto& h : g_hits)
        if (h.enabled) ids.push_back(h.id);
    if (ids.empty()) return;
    auto at = std::find(ids.begin(), ids.end(), g_focus);
    size_t i = at == ids.end() ? (back ? ids.size() - 1 : 0)
                               : (size_t(at - ids.begin()) + (back ? ids.size() - 1 : 1)) % ids.size();
    g_focus = ids[i];
    g_keyboard = true;
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ---- window ----------------------------------------------------------------------------

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            Paint();
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            g_gfx.Resize();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_GETMINMAXINFO: {
            UINT dpi = GetDpiForWindow(hwnd);
            MONITORINFO mi{sizeof mi};
            GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            mm->ptMinTrackSize = {std::min<LONG>(MulDiv(600, int(dpi), 96), mi.rcWork.right - mi.rcWork.left),
                                  std::min<LONG>(MulDiv(440, int(dpi), 96), mi.rcWork.bottom - mi.rcWork.top)};
            return 0;
        }
        case WM_DPICHANGED: {
            g_gfx.SetDpi(HIWORD(wp));
            auto* r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (g_dragging) {
                float travel = g_view - std::max(32.f, g_view * g_view / (g_view + g_scrollMax));
                if (travel > 0) ScrollTo(g_dragScroll + (float(GET_Y_LPARAM(lp)) / g_gfx.Scale() - g_dragFrom) * g_scrollMax / travel);
                return 0;
            }
            int id = HitAt(lp);
            if (id != g_hover) {
                g_hover = id;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            return 0;
        }
        case WM_MOUSELEAVE:
            g_hover = kNone;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            g_keyboard = false;
            if (g_modal == Modal::None && OnScrollbar(lp)) {
                g_dragging = true;
                g_dragFrom = float(GET_Y_LPARAM(lp)) / g_gfx.Scale();
                g_dragScroll = g_scroll;
                SetCapture(hwnd);
                return 0;
            }
            g_press = HitAt(lp);
            if (g_press) SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            if (g_dragging) {
                g_dragging = false;
                ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            int id = HitAt(lp), pressed = g_press;
            g_press = kNone;
            ReleaseCapture();
            InvalidateRect(hwnd, nullptr, FALSE);
            if (id && id == pressed) Click(id);
            return 0;
        }
        case WM_CAPTURECHANGED:
            g_press = kNone;
            g_dragging = false;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_MOUSEWHEEL:
            if (g_modal == Modal::None) {
                ScrollTo(g_scroll - float(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA * 64);
                RefreshHover();
            }
            return 0;
        case WM_KEYDOWN:
            if (lp & 0x40000000) {
                // held keys repeat; only scrolling should, never a button
                if (wp != VK_DOWN && wp != VK_UP && wp != VK_NEXT && wp != VK_PRIOR) return 0;
            }
            if (g_modal == Modal::None && (wp == VK_DOWN || wp == VK_UP || wp == VK_NEXT || wp == VK_PRIOR || wp == VK_HOME || wp == VK_END)) {
                float page = std::max(64.f, g_view - 64);
                ScrollTo(wp == VK_HOME ? 0 : wp == VK_END ? g_scrollMax
                         : g_scroll + (wp == VK_DOWN ? 48 : wp == VK_UP ? -48 : wp == VK_NEXT ? page : -page));
            } else if (wp == VK_TAB) {
                MoveFocus(GetKeyState(VK_SHIFT) < 0);
            } else if (wp == VK_RETURN || wp == VK_SPACE) {
                int id = g_keyboard && g_focus ? g_focus : wp == VK_RETURN ? DefaultButton() : int(kNone);
                if (id) Click(id);
            } else if (wp == VK_ESCAPE) {
                if (g_modal != Modal::None) Click(kNo);
                else if (g_screen == Screen::Ready || g_screen == Screen::End || g_screen == Screen::Reading) Go(Screen::Home);
            } else if (wp == 'O' && GetKeyState(VK_CONTROL) < 0 && !g_running && g_modal == Modal::None) {
                Click(kBrowse);
            }
            return 0;
        case WM_DROPFILES: {
            UINT n = DragQueryFileW(HDROP(wp), 0, nullptr, 0);
            std::wstring path(n, L'\0');
            if (!g_running && g_modal == Modal::None && n && DragQueryFileW(HDROP(wp), 0, path.data(), n + 1)) {
                SetForegroundWindow(hwnd);
                Read(path, L"");
            }
            DragFinish(HDROP(wp));
            return 0;
        }
        case WM_TIMER:
            if (wp == kTick && g_running) Pull();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_APP_ANALYSIS:
            OnAnalysis(reinterpret_cast<AnalysisResult*>(lp));
            return 0;
        case WM_APP_DONE:
            OnDone(reinterpret_cast<RunResult*>(lp));
            return 0;
        case WM_CLOSE:
            if (g_running) {
                if (g_closing) return 0;
                int r = MessageBoxW(hwnd,
                                    L"ZeroZip is still extracting.\n\nStop and close? Everything done so far is kept, "
                                    L"and opening the zip again picks up where it stopped.",
                                    L"ZeroZip", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2);
                if (r != IDOK) return 0;
                // The window closes once the run has stopped; a file being freed as it
                // goes is finished first, which can take a while.
                g_closing = true;
                g_modal = Modal::None;
                g_ex.Stop();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            DestroyWindow(hwnd);
            return 0;
        case WM_QUERYENDSESSION:
            if (!g_running) return TRUE;
            g_ex.Stop();   // Windows shows the block reason; stop so the shutdown can go ahead soon
            return FALSE;
        case WM_ENDSESSION:
            if (wp && g_running) {
                g_ex.Stop();
                if (g_worker.joinable()) g_worker.join();
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"ZeroZipWindow";
    RegisterClassExW(&wc);

    UINT dpi = GetDpiForSystem();
    int w = MulDiv(920, int(dpi), 96), h = MulDiv(640, int(dpi), 96);
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    w = std::min<int>(w, work.right - work.left);
    h = std::min<int>(h, work.bottom - work.top);
    g_hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"ZeroZip", WS_OVERLAPPEDWINDOW,
                             work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 2, w,
                             h, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd || !g_gfx.Init(g_hwnd, inst, IDR_FONT_SANS, IDR_FONT_MONO)) return 1;
    g_gfx.SetDpi(GetDpiForWindow(g_hwnd));
    StyleTitleBar();

    CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_taskbar));
    if (g_taskbar && FAILED(g_taskbar->HrInit())) g_taskbar.Reset();

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1) Read(argv[1], L"");
    LocalFree(argv);

    ShowWindow(g_hwnd, show);
    UpdateWindow(g_hwnd);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    g_taskbar.Reset();
    CoUninitialize();
    return 0;
}
