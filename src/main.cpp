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
                  kGlyphCheck[] = L"\xE73E", kGlyphDone[] = L"\xEC61", kGlyphError[] = L"\xEB90",
                  kGlyphWarning[] = L"\xE7BA", kGlyphInfo[] = L"\xE946", kGlyphPause[] = L"\xE769",
                  kGlyphPlay[] = L"\xE768";

enum class Screen { Home, Reading, Ready, Running, End };
enum class EndKind { Done, Stopped, Problem, CantOpen };
enum class Modal { None, Start, Stop };
enum Id { kNone, kBrowse, kBack, kChange, kCancel, kExtract, kPause, kStop, kOpen, kAnother, kResume, kRetry, kNo, kYes };
enum class Kind { Standard, Accent, Subtle };
enum class Bar { Info, Success, Caution, Critical };

struct Hit { int id; D2D1_RECT_F r; bool enabled; };
struct AnalysisResult { unsigned seq; bool ok; Analysis a; std::wstring err; };
struct RunResult { bool ok, stopped; std::wstring err; };

HWND g_hwnd;
Gfx g_gfx;
Microsoft::WRL::ComPtr<ITaskbarList3> g_taskbar;
bool g_mica = false;

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

void Card(const D2D1_RECT_F& r) {
    g_gfx.Fill(r, g_gfx.theme.card, 8);
    g_gfx.Stroke(r, g_gfx.theme.cardStroke, 8);
}

float ButtonWidth(const std::wstring& label, float min = 120) { return std::max(min, g_gfx.TextWidth(label, Font::Body) + 24); }

void Button(int id, const D2D1_RECT_F& r, const std::wstring& label, Kind kind, bool enabled = true,
            const wchar_t* glyph = nullptr) {
    float dy = g_gfx.OffsetY();
    g_hits.push_back({id, D2D1::RectF(r.left, r.top + dy, r.right, r.bottom + dy), enabled});
    bool hover = enabled && g_hover == id, press = hover && g_press == id;
    const Theme& t = g_gfx.theme;
    D2D1_COLOR_F text = t.text1;
    if (kind == Kind::Accent) {
        D2D1_COLOR_F bg = t.accent;
        if (!enabled) bg = t.accentDisabled;
        else if (press) bg.a *= .8f;
        else if (hover) bg.a *= .9f;
        g_gfx.Fill(r, bg, 4);
        if (enabled) g_gfx.Stroke(r, D2D1::ColorF(0, 0, 0, .1f), 4);
        text = enabled ? t.onAccent : t.onAccentDisabled;
    } else if (kind == Kind::Standard) {
        g_gfx.Fill(r, !enabled ? t.ctrlDisabled : press ? t.ctrlPress : hover ? t.ctrlHover : t.ctrl, 4);
        g_gfx.Stroke(r, t.ctrlStroke, 4);
        g_gfx.Line({r.left + 4, r.bottom - .5f}, {r.right - 4, r.bottom - .5f}, t.ctrlStrokeBottom);
        text = !enabled ? t.textDisabled : press ? t.text2 : t.text1;
    } else {
        if (hover) g_gfx.Fill(r, press ? t.subtlePress : t.subtleHover, 4);
        text = enabled ? (press ? t.text2 : t.text1) : t.textDisabled;
    }
    float h = r.bottom - r.top;
    if (label.empty() && glyph) {
        g_gfx.Text(glyph, Font::Icon16, R(r.left, r.top + (h - 16) / 2, r.right - r.left, 18), text, Align::Center);
    } else {
        float tw = g_gfx.TextWidth(label, Font::Body), gw = glyph ? 24.f : 0;
        float x = r.left + (r.right - r.left - tw - gw) / 2;
        if (glyph) g_gfx.Text(glyph, Font::Icon16, R(x, r.top + (h - 16) / 2, 16, 18), text);
        g_gfx.Text(label, Font::Body, R(x + gw, r.top + (h - 20) / 2, tw + 2, 20), text);
    }
    if (g_keyboard && g_focus == id)
        g_gfx.Stroke(D2D1::RectF(r.left - 3, r.top - 3, r.right + 3, r.bottom + 3), t.text1, 7, 2);
}

float InfoBar(float x, float y, float w, Bar kind, const std::wstring& title, const std::wstring& msg) {
    const Theme& t = g_gfx.theme;
    D2D1_COLOR_F bg = t.infoBg, fg = t.info;
    const wchar_t* glyph = kGlyphInfo;
    if (kind == Bar::Success) { bg = t.successBg; fg = t.success; glyph = kGlyphDone; }
    if (kind == Bar::Caution) { bg = t.cautionBg; fg = t.caution; glyph = kGlyphWarning; }
    if (kind == Bar::Critical) { bg = t.criticalBg; fg = t.critical; glyph = kGlyphError; }
    float tx = x + 48, tw = w - 48 - 16;
    float th = g_gfx.TextHeight(title, Font::BodyStrong, tw), mh = msg.empty() ? 0 : g_gfx.TextHeight(msg, Font::Body, tw);
    float h = 14 + th + (msg.empty() ? 0 : 2 + mh) + 14;
    D2D1_RECT_F r = R(x, y, w, h);
    g_gfx.Fill(r, bg, 6);
    g_gfx.Stroke(r, t.cardStroke, 6);
    g_gfx.Text(glyph, Font::Icon16, R(x + 18, y + 16, 18, 18), fg);
    g_gfx.Text(title, Font::BodyStrong, R(tx, y + 14, tw, th), t.text1, Align::Left, true);
    if (!msg.empty()) g_gfx.Text(msg, Font::Body, R(tx, y + 16 + th, tw, mh), t.text1, Align::Left, true);
    return h;
}

// A row of figures in a card, each with a small label under it.
void Figures(float x, float y, float w, const std::vector<std::pair<std::wstring, std::wstring>>& cells) {
    float h = 76, cw = w / float(cells.size());
    Card(R(x, y, w, h));
    for (size_t i = 0; i < cells.size(); i++) {
        float cx = x + cw * float(i);
        if (i) g_gfx.Line({cx, y + 14}, {cx, y + h - 14}, g_gfx.theme.divider);
        g_gfx.Text(cells[i].first, Font::Subtitle, R(cx + 20, y + 14, cw - 32, 28), g_gfx.theme.text1);
        g_gfx.Text(cells[i].second, Font::Caption, R(cx + 20, y + 44, cw - 32, 16), g_gfx.theme.text2);
    }
}

void Footer(float W, float H) {
    g_gfx.Fill(D2D1::RectF(0, H - 64, W, H), g_gfx.theme.footer);
    g_gfx.Line({0, H - 64 + .5f}, {W, H - 64 + .5f}, g_gfx.theme.divider);
}

// ---- screens ---------------------------------------------------------------------------

bool CanExtract() { return g_a.problemCount == 0 && g_a.minFree >= int64_t(g_a.margin) && g_reading == 0; }

struct Column { float x, w; };

Column Content(float W) {
    float w = std::min(720.f, W - 64);
    return {std::floor((W - w) / 2), w};
}

float DrawHome(float W, float H) {
    const Theme& t = g_gfx.theme;
    float w = std::min(560.f, W - 64), h = 240, x = std::floor((W - w) / 2), y = std::max(32.f, std::floor((H - h) / 2 - 12));
    D2D1_RECT_F drop = R(x, y, w, h);
    g_gfx.Fill(drop, t.card, 8);
    g_gfx.Stroke(drop, t.dashed, 8, 1, true);
    g_gfx.Text(kGlyphDownload, Font::Icon40, R(x, y + 48, w, 44), t.accent, Align::Center);
    g_gfx.Text(L"Drop a zip here", Font::Subtitle, R(x, y + 108, w, 28), t.text1, Align::Center);
    std::wstring browse = L"Browse…";
    float bw = ButtonWidth(browse);
    Button(kBrowse, R(x + (w - bw) / 2, y + 160, bw, 32), browse, Kind::Accent);
    return y + h;
}

void BusyBar(float x, float y, float w) {
    g_gfx.Line({x, y + 1.5f}, {x + w, y + 1.5f}, g_gfx.theme.text3);
    float phase = float((GetTickCount64() - g_animStart) % 1800) / 1800.f;
    float seg = w * .3f, pos = -seg + phase * (w + seg);
    float a = std::max(x, x + pos), b = std::min(x + w, x + pos + seg);
    if (b > a) g_gfx.Fill(D2D1::RectF(a, y, b, y + 3), g_gfx.theme.accent, 1.5f);
}

float DrawReading(float W) {
    auto [x, w] = Content(W);
    g_gfx.Text(Leaf(g_zip), Font::Title, R(x, 28, w, 40), g_gfx.theme.text1);
    BusyBar(x, 88, w);
    Button(kCancel, R(x, 112, 120, 32), L"Cancel", Kind::Standard);
    return 152;
}

float DrawReady(float W) {
    const Theme& t = g_gfx.theme;
    const Analysis& a = g_a;
    auto [x, w] = Content(W);
    float y = 28;
    Button(kBack, R(x, y + 4, 32, 32), L"", Kind::Subtle, true, kGlyphBack);
    g_gfx.Text(Leaf(a.zip), Font::Title, R(x + 44, y, w - 44, 40), t.text1);
    g_gfx.Text(Fmt(double(a.zipBytes)) + L" zip · " + Fmt(double(a.unpacked)) + L" unpacked · " +
                   Plural(a.files, L"file", L"files"),
               Font::Caption, R(x + 44, y + 40, w - 44, 16), t.text2);
    y += 76;

    std::wstring vol = VolumeName();
    if (a.problemCount) {
        std::wstring msg;
        for (size_t i = 0; i < std::min<size_t>(a.problems.size(), 3); i++)
            msg += (i ? L"\n" : L"") + Leaf(a.problems[i].name) + L": " + a.problems[i].reason;
        y += InfoBar(x, y, w, Bar::Critical, L"This zip can't be extracted", msg) + 8;
    } else if (a.minFree >= int64_t(a.margin)) {
        y += InfoBar(x, y, w, Bar::Success, L"Fits on " + vol, L"") + 8;
    } else {
        y += InfoBar(x, y, w, Bar::Caution, L"Not enough space",
                     L"Free up " + Fmt(double(int64_t(a.margin) - a.minFree)) + L" more on " + vol + L".") + 8;
    }
    if (!a.lost.empty())
        y += InfoBar(x, y, w, Bar::Caution, Leaf(a.lost) + L" was lost last time and will be skipped", L"") + 8;
    else if (a.resuming)
        y += InfoBar(x, y, w, Bar::Info, L"Continuing where it stopped", L"") + 8;
    if (!a.sameVolume)
        y += InfoBar(x, y, w, Bar::Caution, L"The zip and the folder are on different drives",
                     L"The folder's drive needs room for every file.") + 8;
    if (a.punchFiles) {
        std::wstring names;
        for (auto& n : a.punchNames) names += (names.empty() ? L"" : L", ") + Leaf(n);
        if (a.punchFiles > a.punchNames.size()) names += L"…";
        y += InfoBar(x, y, w, Bar::Caution, Plural(a.punchFiles, L"file is", L"files are") + L" bigger than the free space",
                     names) + 8;
    }
    y += 4;

    std::wstring change = L"Change…";
    float cw = ButtonWidth(change, 96);
    Card(R(x, y, w, 68));
    g_gfx.Text(kGlyphFolder, Font::Icon20, R(x + 20, y + 24, 24, 22), t.text1);
    g_gfx.Text(L"Extract to", Font::Body, R(x + 60, y + 14, w - 100 - cw, 20), t.text1);
    g_gfx.Text(Compact(a.dest, w - 100 - cw), Font::Caption, R(x + 60, y + 36, w - 100 - cw, 16), t.text2);
    Button(kChange, R(x + w - 20 - cw, y + 18, cw, 32), change, Kind::Standard);
    return y + 68;
}

void ReadyFooter(float W, float H) {
    auto [x, w] = Content(W);
    Footer(W, H);
    Button(kExtract, R(x + w - 120, H - 48, 120, 32), L"Extract", Kind::Accent, CanExtract());
    Button(kCancel, R(x + w - 248, H - 48, 120, 32), L"Cancel", Kind::Standard);
}

float DrawRunning(float W) {
    const Theme& t = g_gfx.theme;
    const Progress& p = g_prog;
    auto [x, w] = Content(W);
    bool stopping = g_ex.Stopped();
    float y = 28;
    g_gfx.Text(stopping ? L"Stopping after this file" : g_paused ? L"Paused" : L"Extracting", Font::Title, R(x, y, w, 40),
               t.text1);
    g_gfx.Text(Leaf(g_a.zip), Font::Caption, R(x, y + 40, w, 16), t.text2);
    y += 76;

    double frac = p.bytesTotal ? double(p.bytesDone) / double(p.bytesTotal) : p.total ? double(p.done) / double(p.total) : 0;
    frac = std::clamp(frac, 0.0, 1.0);
    wchar_t pct[16];
    double shown = frac * 100;
    if (shown >= 99.95 && p.done < p.total) shown = 99.9;
    swprintf_s(pct, shown < 10 ? L"%.1f%%" : L"%.0f%%", shown);
    Card(R(x, y, w, 120));
    g_gfx.Text(pct, Font::Large, R(x + 20, y + 12, 240, 52), t.text1);
    float bx = x + 20, bw = w - 40, by = y + 72;
    g_gfx.Line({bx, by + 1.5f}, {bx + bw, by + 1.5f}, t.text3);
    if (frac > 0) g_gfx.Fill(R(bx, by, std::max(3.f, bw * float(frac)), 3), g_paused ? t.caution : t.accent, 1.5f);
    g_gfx.Text(p.current.empty() ? L"Starting…" : Leaf(p.current), Font::Caption, R(bx, y + 88, bw, 16), t.text2);
    y += 128;

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
    return y + 76;
}

void RunningFooter(float W, float H) {
    auto [x, w] = Content(W);
    bool stopping = g_ex.Stopped();
    Footer(W, H);
    Button(kStop, R(x + w - 120, H - 48, 120, 32), L"Stop", Kind::Standard, !stopping);
    Button(kPause, R(x + w - 248, H - 48, 120, 32), g_paused ? L"Resume" : L"Pause", Kind::Standard, !stopping,
           g_paused ? kGlyphPlay : kGlyphPause);
}

float DrawEnd(float W, float H) {
    const Theme& t = g_gfx.theme;
    float w = std::min(520.f, W - 64), x = std::floor((W - w) / 2);
    D2D1_COLOR_F fg = t.success, bg = t.successBg;
    const wchar_t* glyph = kGlyphCheck;
    std::wstring title;
    std::vector<std::pair<int, std::wstring>> buttons;
    switch (g_end) {
        case EndKind::Done:
            title = L"Done";
            buttons = {{kOpen, L"Open folder"}, {kAnother, L"Extract another"}};
            break;
        case EndKind::Stopped:
            fg = t.caution; bg = t.cautionBg; glyph = kGlyphPause;
            title = L"Stopped";
            buttons = {{kResume, L"Resume"}, {kAnother, L"Close"}};
            break;
        case EndKind::Problem:
            fg = t.critical; bg = t.criticalBg; glyph = kGlyphWarning;
            title = L"Stopped";
            buttons = {{kResume, L"Resume"}, {kAnother, L"Close"}};
            break;
        case EndKind::CantOpen:
            fg = t.critical; bg = t.criticalBg; glyph = kGlyphError;
            title = L"Can't open this zip";
            buttons = {{kRetry, L"Try again"}, {kAnother, L"Choose another zip"}};
            break;
    }
    float height = 96 + 48 + g_gfx.TextHeight(g_endText, Font::Body, w) + 28 + 32;
    float y = std::max(32.f, std::floor((H - height) / 2 - 12));
    g_gfx.Circle({x + w / 2, y + 36}, 36, bg);
    g_gfx.Text(glyph, Font::Icon40, R(x, y + 16, w, 44), fg, Align::Center);
    y += 96;
    g_gfx.Text(title, Font::Title, R(x, y, w, 40), t.text1, Align::Center);
    y += 48;
    y += g_gfx.Text(g_endText, Font::Body, R(x, y, w, 200), t.text2, Align::Center, true) + 28;
    float total = 0;
    for (auto& b : buttons) total += ButtonWidth(b.second) + 8;
    float bx = x + (w - total + 8) / 2;
    for (size_t i = 0; i < buttons.size(); i++) {
        float bw = ButtonWidth(buttons[i].second);
        Button(buttons[i].first, R(bx, y, bw, 32), buttons[i].second, i == 0 ? Kind::Accent : Kind::Standard);
        bx += bw + 8;
    }
    return y + 56;
}

void DrawModal(float W, float H) {
    const Theme& t = g_gfx.theme;
    g_hits.clear();   // only the dialog takes input
    g_gfx.Fill(D2D1::RectF(0, 0, W, H), t.smoke);
    float w = std::min(440.f, W - 48), x = std::floor((W - w) / 2), tw = w - 48;
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
    float h = 24 + 28 + 12 + textH + 24 + 80, y = std::floor((H - h) / 2);
    D2D1_RECT_F box = R(x, y, w, h);
    g_gfx.Fill(D2D1::RectF(x - 1, y + 2, x + w + 1, y + h + 6), D2D1::ColorF(0, 0, 0, .25f), 9);
    g_gfx.Fill(box, t.dialog, 8);
    g_gfx.PushClip(D2D1::RectF(x, y + h - 80, x + w, y + h));
    g_gfx.Fill(box, t.dialogFooter, 8);
    g_gfx.PopClip();
    g_gfx.Line({x, y + h - 80 + .5f}, {x + w, y + h - 80 + .5f}, t.divider);
    g_gfx.Stroke(box, t.cardStroke, 8);
    g_gfx.Text(title, Font::Subtitle, R(x + 24, y + 24, tw, 28), t.text1);
    g_gfx.Text(text, Font::Body, R(x + 24, y + 64, tw, textH + 4), t.text1, Align::Left, true);
    float bw = (w - 48 - 8) / 2;
    Button(kYes, R(x + 24, y + h - 56, bw, 32), yes, Kind::Accent);
    Button(kNo, R(x + 24 + bw + 8, y + h - 56, bw, 32), no, Kind::Standard);
}

void Paint() {
    if (!g_gfx.Begin()) return;
    g_hits.clear();
    D2D1_SIZE_F size = g_gfx.Size();
    float W = size.width, H = size.height;
    bool footer = g_screen == Screen::Ready || g_screen == Screen::Running;
    float view = footer ? H - 64 : H;
    g_view = view;
    if (g_scroll > g_scrollMax) g_scroll = g_scrollMax;

    g_gfx.PushClip(D2D1::RectF(0, 0, W, view));
    g_gfx.Translate(-g_scroll);
    float bottom = 0;
    switch (g_screen) {
        case Screen::Home: bottom = DrawHome(W, H); break;
        case Screen::Reading: bottom = DrawReading(W); break;
        case Screen::Ready: bottom = DrawReady(W); break;
        case Screen::Running: bottom = DrawRunning(W); break;
        case Screen::End: bottom = DrawEnd(W, H); break;
    }
    g_gfx.Translate(g_scroll);
    g_gfx.PopClip();
    // Only the visible part of a scrolled button can be clicked.
    for (auto& h : g_hits) {
        h.r.top = std::max(h.r.top, 0.f);
        h.r.bottom = std::min(h.r.bottom, view);
    }
    g_hits.erase(std::remove_if(g_hits.begin(), g_hits.end(), [](const Hit& h) { return h.r.bottom <= h.r.top; }),
                 g_hits.end());
    if (g_screen == Screen::Ready) ReadyFooter(W, H);
    if (g_screen == Screen::Running) RunningFooter(W, H);

    float scrollMax = std::max(0.f, bottom + 24 - view);
    if (scrollMax != g_scrollMax) {
        g_scrollMax = scrollMax;
        if (g_scroll > g_scrollMax) InvalidateRect(g_hwnd, nullptr, FALSE);   // content got shorter: draw again in range
    }
    if (g_scrollMax > 0) {
        float thumb = std::max(32.f, view * view / (view + g_scrollMax));
        float top = (view - thumb) * (g_scroll / g_scrollMax);
        g_gfx.Fill(R(W - (g_dragging ? 7.f : 5.f), top + 2, g_dragging ? 4.f : 2.f, thumb - 4), g_gfx.theme.text3, 2);
    }
    if (g_modal != Modal::None) DrawModal(W, H);
    g_gfx.End();
}

// ---- theme -------------------------------------------------------------------------

D2D1_COLOR_F Hex(const BYTE* p) { return D2D1::ColorF(p[0] / 255.f, p[1] / 255.f, p[2] / 255.f); }

void ApplyTheme() {
    DWORD light = 1, n = sizeof light;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &n);
    bool dark = light == 0;
    wchar_t forced[8] = {};
    DWORD fl = GetEnvironmentVariableW(L"ZEROZIP_TEST_THEME", forced, 8);   // test hook
    if (fl && fl < 8) dark = wcscmp(forced, L"light") != 0;
    // Accent shades the way WinUI picks them: Light2 on dark backgrounds, Dark1 on light ones.
    D2D1_COLOR_F onDark = D2D1::ColorF(0x4cc2ff), onLight = D2D1::ColorF(0x005fb8);
    BYTE pal[32];
    n = sizeof pal;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                     L"AccentPalette", RRF_RT_REG_BINARY, nullptr, pal, &n) == ERROR_SUCCESS && n >= 32) {
        onDark = Hex(pal + 4);
        onLight = Hex(pal + 16);
    }
    g_gfx.SetTheme(dark, g_mica, onDark, onLight);
    BOOL on = dark;
    if (FAILED(DwmSetWindowAttribute(g_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &on, sizeof on)))
        DwmSetWindowAttribute(g_hwnd, 19, &on, sizeof on);   // the same switch on Windows 10 before 20H1
    InvalidateRect(g_hwnd, nullptr, FALSE);
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
        ShowEnd(EndKind::Done, L"Extracted to " + g_a.dest);
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
        case WM_SETTINGCHANGE:
            if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0) ApplyTheme();
            return 0;
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
    int w = MulDiv(880, int(dpi), 96), h = MulDiv(600, int(dpi), 96);
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    w = std::min<int>(w, work.right - work.left);
    h = std::min<int>(h, work.bottom - work.top);
    g_hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"ZeroZip", WS_OVERLAPPEDWINDOW,
                             work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 2, w,
                             h, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd || !g_gfx.Init(g_hwnd)) return 1;
    g_gfx.SetDpi(GetDpiForWindow(g_hwnd));

    int backdrop = DWMSBT_MAINWINDOW;   // Mica, on Windows 11 22H2 and later
    bool noMica = GetEnvironmentVariableW(L"ZEROZIP_TEST_NO_MICA", nullptr, 0) > 0;   // test hook: the Windows 10 look
    g_mica = !noMica && SUCCEEDED(DwmSetWindowAttribute(g_hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof backdrop));
    if (g_mica) {
        MARGINS all{-1, -1, -1, -1};
        DwmExtendFrameIntoClientArea(g_hwnd, &all);
    }
    ApplyTheme();

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
