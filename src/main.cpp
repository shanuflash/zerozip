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
constexpr UINT WM_APP_FIT = WM_APP + 3;        // wParam: client height wanted, in pixels
constexpr UINT_PTR kTick = 1;                  // frames while extracting
constexpr UINT_PTR kAnim = 2;                  // busy bar while a zip is read
constexpr double GiB = 1024.0 * 1024 * 1024;

// Segoe Fluent Icons / Segoe MDL2 Assets
constexpr wchar_t kGlyphDownload[] = L"\xE896", kGlyphFolder[] = L"\xE8B7", kGlyphDrive[] = L"\xEDA2",
                  kGlyphCheck[] = L"\xE73E", kGlyphError[] = L"\xEB90", kGlyphWarning[] = L"\xE7BA",
                  kGlyphPause[] = L"\xE769", kGlyphPlay[] = L"\xE768", kGlyphArrow[] = L"\xE72A",
                  kGlyphClose[] = L"\xE711";

enum class Screen { Home, Reading, Ready, Running, End };
enum class EndKind { Done, Stopped, Problem, CantOpen };
enum Id { kNone, kBrowse, kChange, kCancel, kExtract, kPause, kStop, kOpen, kAnother, kResume, kRetry };
enum class Kind { Primary, Ghost, Icon };
enum class Tone { Good, Warn, Bad };

struct Hit { int id; D2D1_RECT_F r; bool enabled; };
struct AnalysisResult { unsigned seq; bool ok; Analysis a; std::wstring err; };
struct RunResult { bool ok, stopped; std::wstring err; };

HWND g_hwnd;
Gfx g_gfx;
Microsoft::WRL::ComPtr<ITaskbarList3> g_taskbar;

Screen g_screen = Screen::Home;
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
// The speed graph runs on its own clock and shows the last half minute sliding left at a steady
// pace, so the curve only ever moves sideways. The clock stops while paused.
struct Sample { double t, rate; uint64_t bytes; };
std::deque<Sample> g_samples;                      // one every quarter second of run time
double g_clock = 0;                                // seconds extracting, not counting pauses
ULONGLONG g_clockAt = 0;
double g_rate = 0;                                 // a slower average, for the time remaining
double g_shownRate = 0;                            // the speed in the graph's corner
double g_top = 0;                                  // top of the graph, easing towards the visible peak
constexpr double kStep = .25, kSpan = 30;
constexpr double kDelay = 2 * kStep;               // the tip trails two samples behind, so it never has to guess
int g_titlePct = -1;

std::vector<Hit> g_hits;
int g_hover = kNone, g_press = kNone, g_focus = kNone;
bool g_keyboard = false;           // show the focus ring only after Tab
int g_fitAsked = 0;
struct Drive { std::wstring name; uint64_t free, total; };
std::vector<Drive> g_drives;       // fixed drives, shown on the home screen                // last height asked for, so a clamped window isn't asked again

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

// "C:" reads better as "C Drive".
std::wstring DriveName(const std::wstring& vol) {
    if (vol.size() >= 2 && vol[1] == L':') return std::wstring(1, wchar_t(towupper(vol[0]))) + L" Drive";
    return vol.empty() ? L"the drive" : vol;
}

std::wstring VolumeName() { return DriveName(g_a.volume); }

// ---- drawing pieces ----------------------------------------------------------------

constexpr float kWidth = 520;   // the window's fixed width; its height follows what's on screen
constexpr float kPad = 24;
constexpr float kButtonH = 36;

D2D1_RECT_F R(float x, float y, float w, float h) { return D2D1::RectF(x, y, x + w, y + h); }

D2D1_RECT_F Card(const D2D1_RECT_F& r) {
    g_gfx.Fill(r, g_gfx.theme.card, 12);
    g_gfx.Stroke(r, g_gfx.theme.cardLine, 12);
    return r;
}

float ButtonWidth(const std::wstring& label, Kind kind) {
    if (kind == Kind::Icon) return kButtonH;
    float text = g_gfx.TextWidth(label, Font::BodyStrong);
    return kind == Kind::Ghost ? 16 + text + 16 : 16 + text + 10 + 26 + 5;
}

// Pill buttons; a primary one carries an arrow in a small round tip on the right.
void Button(int id, float x, float y, const std::wstring& label, Kind kind, bool enabled = true,
            const wchar_t* glyph = nullptr) {
    const Theme& t = g_gfx.theme;
    D2D1_RECT_F r = R(x, y, ButtonWidth(label, kind), kButtonH);
    g_hits.push_back({id, r, enabled});
    bool hover = enabled && g_hover == id, press = hover && g_press == id;
    float radius = kButtonH / 2;
    if (press) r = D2D1::RectF(r.left + 1, r.top + 1, r.right - 1, r.bottom - 1);   // a slight press-in
    D2D1_COLOR_F ink = t.text1;
    bool solid = kind == Kind::Primary;
    if (solid) {
        D2D1_COLOR_F bg = hover ? t.accentHover : t.accent;
        if (!enabled) bg.a = .3f;
        g_gfx.Fill(r, bg, radius);
        ink = t.accentInk;
    } else {
        g_gfx.Fill(r, hover ? t.ghostHover : t.ghost, radius);
        g_gfx.Stroke(r, t.ghostLine, radius);
        if (!enabled) ink = t.textDisabled;
    }
    float h = r.bottom - r.top;
    if (kind == Kind::Icon) {
        g_gfx.Text(glyph, Font::Icon16, R(r.left, r.top + (h - 16) / 2, r.right - r.left, 18), ink, Align::Center);
    } else {
        g_gfx.Text(label, Font::BodyStrong, R(r.left + 16, r.top + (h - 20) / 2, r.right - r.left - 16, 20), ink);
        if (solid) {
            D2D1_POINT_2F c = {r.right - 5 - 13 + (hover ? 1.5f : 0), r.top + h / 2};   // the tip leans forward on hover
            g_gfx.Circle(c, 13, D2D1::ColorF(t.accentInk.r, t.accentInk.g, t.accentInk.b, .12f));
            g_gfx.Text(kGlyphArrow, Font::Icon16, R(c.x - 9, c.y - 8, 18, 18), ink, Align::Center);
        }
    }
    if (g_keyboard && g_focus == id)
        g_gfx.Stroke(D2D1::RectF(r.left - 3, r.top - 3, r.right + 3, r.bottom + 3), t.accentLine, radius + 3, 2);
}

// A row of buttons ending at `right`, the last one primary.
void Buttons(float right, float y, const std::vector<std::pair<int, std::wstring>>& buttons, Kind last, bool lastEnabled = true) {
    auto kind = [&](size_t i) { return i + 1 == buttons.size() ? last : Kind::Ghost; };
    float x = right + 8;
    for (size_t i = 0; i < buttons.size(); i++) x -= ButtonWidth(buttons[i].second, kind(i)) + 8;
    for (size_t i = 0; i < buttons.size(); i++) {   // left to right, so Tab goes the way they read
        Button(buttons[i].first, x, y, buttons[i].second, kind(i), i + 1 == buttons.size() ? lastEnabled : true);
        x += ButtonWidth(buttons[i].second, kind(i)) + 8;
    }
}

// A coloured pill with a glyph, for the verdict and notices. Returns its height.
float Badge(float x, float y, Tone tone, const std::wstring& text) {
    const Theme& t = g_gfx.theme;
    D2D1_COLOR_F fg = t.accent, tint = t.accentTint, line = t.accentLine;
    const wchar_t* glyph = kGlyphCheck;
    if (tone == Tone::Warn) { fg = t.warn; tint = t.warnTint; line = t.warnLine; glyph = kGlyphWarning; }
    if (tone == Tone::Bad) { fg = t.danger; tint = t.dangerTint; line = t.dangerLine; glyph = kGlyphError; }
    D2D1_RECT_F r = R(x, y, g_gfx.TextWidth(text, Font::BodyStrong) + 28 + 12, 26);
    g_gfx.Fill(r, tint, 13);
    g_gfx.Stroke(r, line, 13);
    g_gfx.Text(glyph, Font::Icon16, R(x + 9, y + 5, 18, 18), fg);
    g_gfx.Text(text, Font::BodyStrong, R(x + 28, y + 3, r.right - x - 28, 20), fg);
    return 26;
}

// A notice: a badge, and if there's more to say, a line of text under it.
float Notice(float x, float y, float w, Tone tone, const std::wstring& title, const std::wstring& text) {
    float h = Badge(x, y, tone, title);
    if (!text.empty()) h += 6 + g_gfx.Text(text, Font::Body, R(x + 2, y + h + 6, w - 2, 200), g_gfx.theme.text2, Align::Left, true);
    return h;
}

// Text in a few colours on one line, cut off with an ellipsis where it runs out of room.
void Pieces(float x, float y, float w, const std::vector<std::pair<std::wstring, D2D1_COLOR_F>>& pieces) {
    float right = x + w;
    for (auto& [text, color] : pieces) {
        float tw = g_gfx.TextWidth(text, Font::Body);
        g_gfx.Text(text, Font::Body, R(x, y, std::min(tw + 1, right - x), 20), color);
        x += tw;
        if (x >= right) break;
    }
}

void Detail(float x, float y, float w, const std::wstring& label, const std::wstring& value) {
    g_gfx.Text(label, Font::Body, R(x, y, 130, 20), g_gfx.theme.text3);
    g_gfx.Text(value, Font::Body, R(x + 130, y, w - 130, 20), g_gfx.theme.text1);
}

// ---- screens ---------------------------------------------------------------------------

bool CanExtract() { return g_a.problemCount == 0 && g_a.minFree >= int64_t(g_a.margin) && g_reading == 0; }

float DrawHome(float W) {
    const Theme& t = g_gfx.theme;
    float w = W - 2 * kPad;
    D2D1_RECT_F in = Card(R(kPad, kPad, w, 196));
    g_gfx.Stroke(D2D1::RectF(in.left + 8, in.top + 8, in.right - 8, in.bottom - 8), t.dashed, 8, 1, true);
    float cx = kPad + w / 2, y = kPad + 28;
    D2D1_RECT_F tile = R(cx - 26, y, 52, 52);
    g_gfx.Fill(tile, t.accentTint, 15);
    g_gfx.Stroke(tile, t.accentLine, 15);
    g_gfx.Text(kGlyphDownload, Font::Icon28, R(tile.left, y + 11, 52, 32), t.accent, Align::Center);
    g_gfx.Text(L"Drop a zip here", Font::Subtitle, R(kPad, y + 64, w, 28), t.text1, Align::Center);
    std::wstring choose = L"Choose zip";
    Button(kBrowse, cx - ButtonWidth(choose, Kind::Primary) / 2, y + 104, choose, Kind::Primary);
    y = kPad + 196;
    if (g_drives.empty()) return y;
    y += 12;
    float h = 12 + float(g_drives.size()) * 46 + 2;
    Card(R(kPad, y, w, h));
    float ry = y + 14, bx = kPad + 46, bw = w - 46 - 18;
    for (auto& d : g_drives) {
        float used = 1 - float(double(d.free) / double(d.total));
        g_gfx.Text(kGlyphDrive, Font::Icon16, R(kPad + 18, ry + 2, 18, 18), t.text2);
        g_gfx.Text(d.name, Font::BodyStrong, R(bx, ry, 140, 20), t.text1);
        g_gfx.Text(Fmt(double(d.free)) + L" free of " + Fmt(double(d.total)), Font::Caption, R(bx, ry + 2, bw, 16), t.text2, Align::Right);
        g_gfx.Fill(R(bx, ry + 26, bw, 4), t.track, 2);
        g_gfx.Fill(R(bx, ry + 26, std::max(4.f, bw * used), 4), used > .9f ? t.warn : t.accent, 2);
        ry += 46;
    }
    return y + h;
}

void BusyBar(float x, float y, float w) {
    const Theme& t = g_gfx.theme;
    g_gfx.Fill(R(x, y, w, 6), t.track, 3);
    float phase = float((GetTickCount64() - g_animStart) % 1600) / 1600.f;
    float seg = w * .32f, pos = -seg + phase * (w + seg);
    float a = std::max(x, x + pos), b = std::min(x + w, x + pos + seg);
    if (b > a) g_gfx.Fill(D2D1::RectF(a, y, b, y + 6), t.accent, 3);
}

float DrawReading(float W) {
    float w = W - 2 * kPad;
    g_gfx.Text(Leaf(g_zip), Font::Subtitle, R(kPad, kPad, w, 28), g_gfx.theme.text1);
    g_gfx.Text(L"Reading the zip\u2026", Font::Body, R(kPad, kPad + 32, w, 20), g_gfx.theme.text2);
    BusyBar(kPad, kPad + 66, w);
    Buttons(W - kPad, kPad + 92, {{kCancel, L"Cancel"}}, Kind::Ghost);
    return kPad + 92 + kButtonH;
}

float DrawReady(float W) {
    const Theme& t = g_gfx.theme;
    const Analysis& a = g_a;
    float x = kPad, w = W - 2 * kPad, y = kPad;
    g_gfx.Text(Leaf(a.zip), Font::Subtitle, R(x, y, w, 28), t.text1);
    g_gfx.Text(Fmt(double(a.zipBytes)) + L" \u2192 " + Fmt(double(a.unpacked)) + L"  \u00b7  " + Plural(a.files, L"file", L"files"),
               Font::Mono, R(x, y + 32, w, 18), t.text2);
    y += 66;

    std::wstring vol = VolumeName();
    if (a.problemCount) {
        std::wstring msg;
        for (size_t i = 0; i < std::min<size_t>(a.problems.size(), 3); i++)
            msg += (i ? L"\n" : L"") + Leaf(a.problems[i].name) + L": " + a.problems[i].reason;
        y += Notice(x, y, w, Tone::Bad, L"Can't extract this zip", msg) + 10;
    } else if (a.minFree >= int64_t(a.margin)) {
        y += Notice(x, y, w, Tone::Good, L"Fits on " + vol, L"") + 10;
    } else {
        y += Notice(x, y, w, Tone::Warn, L"Not enough space",
                    L"Free up " + Fmt(double(int64_t(a.margin) - a.minFree)) + L" more on " + vol + L".") + 10;
    }
    if (!a.lost.empty())
        y += Notice(x, y, w, Tone::Warn, Leaf(a.lost) + L" was lost last time and will be skipped", L"") + 10;
    else if (a.resuming)
        y += Notice(x, y, w, Tone::Good, L"Continuing where it stopped", L"") + 10;
    if (!a.sameVolume)
        y += Notice(x, y, w, Tone::Warn, L"The zip and the folder are on different drives",
                    L"The folder's drive needs room for every file.") + 10;
    if (a.punchFiles) {
        std::wstring names;
        for (auto& n : a.punchNames) names += (names.empty() ? L"" : L", ") + Leaf(n);
        if (a.punchFiles > a.punchNames.size()) names += L"\u2026";
        y += Notice(x, y, w, Tone::Warn, Plural(a.punchFiles, L"file is", L"files are") + L" bigger than the free space",
                    names + L". Their zip data is freed as they extract, so a power cut during one loses that file.") + 10;
    }
    y += 6;

    std::wstring change = L"Change";
    float cw = ButtonWidth(change, Kind::Ghost);
    D2D1_RECT_F in = Card(R(x, y, w, 72));
    float pathW = in.right - in.left - 52 - cw - 24;
    g_gfx.Text(kGlyphFolder, Font::Icon20, R(in.left + 16, in.top + 20, 24, 24), t.text2);
    g_gfx.Text(L"Extract to", Font::Caption, R(in.left + 50, in.top + 12, pathW, 16), t.text3);
    g_gfx.Text(Compact(a.dest, pathW * .85f), Font::Mono, R(in.left + 50, in.top + 32, pathW, 18), t.text1);
    Button(kChange, in.right - 14 - cw, in.top + (in.bottom - in.top - kButtonH) / 2, change, Kind::Ghost);
    y += 72 + 18;

    g_gfx.Text(L"The zip is deleted as it extracts.", Font::Caption, R(x, y + 10, w - 200, 16), t.text3);
    Buttons(W - kPad, y, {{kCancel, L"Cancel"}, {kExtract, L"Extract"}}, Kind::Primary, CanExtract());
    return y + kButtonH;
}

// The speed at time `t` on the graph's clock, along a Catmull-Rom curve through the samples.
double SpeedAt(double t) {
    size_t n = g_samples.size();
    double f = (t - g_samples[0].t) / kStep;
    size_t i = size_t(std::clamp(f, 0.0, double(n - 2)));
    double u = std::clamp(f - double(i), 0.0, 1.0);
    double p0 = g_samples[i ? i - 1 : 0].rate, p1 = g_samples[i].rate, p2 = g_samples[i + 1].rate;
    double p3 = g_samples[std::min(i + 2, n - 1)].rate;
    double v = .5 * (2 * p1 + (p2 - p0) * u + (2 * p0 - 5 * p1 + 4 * p2 - p3) * u * u + (3 * p1 - p0 - 3 * p2 + p3) * u * u * u);
    return std::max(v, 0.0);
}

bool GraphReady() { return g_samples.size() >= 3 && g_clock > kDelay; }

// The last half minute of speed, scrolling like Task Manager's graphs.
void SpeedGraph(const D2D1_RECT_F& box) {
    const Theme& t = g_gfx.theme;
    D2D1_RECT_F in = Card(box);
    D2D1_RECT_F r = D2D1::RectF(in.left + 12, in.top + 10, in.right - 12, in.bottom - 10);
    D2D1_RECT_F plot = D2D1::RectF(r.left, r.top + 24, r.right, r.bottom);   // room for the speed above the curve
    double tip = g_clock - kDelay;
    double left = tip - kSpan;   // now is always at the right edge, so the start comes in from the right
    float px = (r.right - r.left) / float(kSpan);
    for (int i = 1; i < 4; i++) {
        float gy = plot.top + (plot.bottom - plot.top) * float(i) / 4;
        g_gfx.Line({r.left, gy}, {r.right, gy}, D2D1::ColorF(1, 1, 1, .035f));
    }
    for (double gt = std::ceil(left / 3) * 3; gt < left + kSpan; gt += 3)   // grid lines move with the curve
        if (gt > left) g_gfx.Line({r.left + float(gt - left) * px, plot.top}, {r.left + float(gt - left) * px, r.bottom}, D2D1::ColorF(1, 1, 1, .035f));
    if (!GraphReady() || g_top == 0) {
        if (!g_paused) g_gfx.Text(L"Measuring speed\u2026", Font::Caption, R(r.left, r.top, r.right - r.left, 16), t.text3);
        return;
    }
    auto at = [&](double tt, double v) {
        return D2D1::Point2F(r.left + float(tt - left) * px, plot.bottom - float(std::min(v / g_top, 1.0)) * (plot.bottom - plot.top));
    };
    // Points fall on fixed times along the curve, so from one frame to the next they only slide.
    std::vector<D2D1_POINT_2F> pts;
    double t0 = g_samples[0].t + std::max(0.0, std::floor((left - g_samples[0].t) / kStep)) * kStep;
    for (int j = 0;; j++) {
        double tt = t0 + j * kStep / 4;
        if (tt >= tip) break;
        pts.push_back(at(tt, SpeedAt(tt)));
    }
    D2D1_POINT_2F end = at(tip, SpeedAt(tip));
    pts.push_back(end);
    D2D1_COLOR_F line = g_paused ? t.warn : t.accent;
    g_gfx.PushClip(r);
    g_gfx.Area(plot, pts, line, D2D1::ColorF(line.r, line.g, line.b, .16f));
    g_gfx.PopClip();
    g_gfx.Circle(end, 4.5f, t.card);
    g_gfx.Circle(end, 3, line);
    if (g_paused) g_gfx.Text(L"Paused", Font::Caption, R(r.left, r.top, r.right - r.left, 16), t.warn);
    else g_gfx.Text(Fmt(g_shownRate) + L"/s", Font::Caption, R(r.left, r.top, r.right - r.left, 16), t.text1);
}

float DrawRunning(float W) {
    const Theme& t = g_gfx.theme;
    const Progress& p = g_prog;
    float x = kPad, w = W - 2 * kPad, y = kPad;
    std::wstring dest = Leaf(g_a.dest);
    Pieces(x, y, w, {{L"Extracting " + Plural(g_a.files, L"file", L"files") + L" from ", t.text2}, {Leaf(g_a.zip), t.accent},
                     {L" to ", t.text2}, {dest.empty() ? g_a.dest : dest, t.accent}});
    y += 30;

    double shown = p.bytesTotal ? std::clamp(double(p.bytesDone) / double(p.bytesTotal), 0.0, 1.0) * 100 : 0;
    if (shown >= 99.95 && p.done < p.total) shown = 99.9;
    wchar_t pct[32];
    swprintf_s(pct, shown < 10 ? L"%.1f%% complete" : L"%.0f%% complete", shown);
    bool stopping = g_ex.Stopped();
    g_gfx.Text(stopping ? L"Stopping after this file" : pct, Font::Title, R(x, y, w - 100, 40), t.text1);
    Button(kPause, W - kPad - 2 * kButtonH - 8, y + 2, L"", Kind::Icon, !stopping, g_paused ? kGlyphPlay : kGlyphPause);
    Button(kStop, W - kPad - kButtonH, y + 2, L"", Kind::Icon, !stopping, kGlyphClose);
    y += 50;

    SpeedGraph(R(x, y, w, 150));
    y += 150 + 14;

    std::wstring left = g_paused ? L"Paused" : L"Calculating\u2026";
    if (!g_paused && g_rate > 0 && g_samples.size() > 3)
        left = Words(uint64_t(double(p.bytesTotal - std::min(p.bytesTotal, p.bytesDone)) / g_rate * 1000));
    std::wstring state = p.verifying ? L"  (checking)" : p.punching ? L"  (freeing as it goes)" : L"";
    Detail(x, y, w, L"Name:", (p.current.empty() ? L"Starting\u2026" : Leaf(p.current)) + state);
    Detail(x, y + 24, w, L"Time remaining:", left);
    Detail(x, y + 48, w, L"Items remaining:", Num(p.total - std::min(p.total, p.done)) + L" (" +
                                                 Fmt(double(p.bytesTotal - std::min(p.bytesTotal, p.bytesDone))) + L")");
    Detail(x, y + 72, w, L"Free space:", Fmt(double(p.freeBytes)) + L" on " + VolumeName());
    return y + 72 + 20;
}

float DrawEnd(float W) {
    const Theme& t = g_gfx.theme;
    float x = kPad, w = W - 2 * kPad, y = kPad;
    D2D1_COLOR_F fg = t.accent, tint = t.accentTint;
    const wchar_t* glyph = kGlyphCheck;
    std::wstring title;
    std::vector<std::pair<int, std::wstring>> buttons;
    switch (g_end) {
        case EndKind::Done:
            title = L"Done";
            buttons = {{kAnother, L"Extract another"}, {kOpen, L"Open folder"}};
            break;
        case EndKind::Stopped:
            fg = t.warn; tint = t.warnTint; glyph = kGlyphPause;
            title = L"Stopped";
            buttons = {{kAnother, L"Close"}, {kResume, L"Resume"}};
            break;
        case EndKind::Problem:
            fg = t.danger; tint = t.dangerTint; glyph = kGlyphWarning;
            title = L"Stopped";
            buttons = {{kAnother, L"Close"}, {kResume, L"Resume"}};
            break;
        case EndKind::CantOpen:
            fg = t.danger; tint = t.dangerTint; glyph = kGlyphError;
            title = L"Can't open this zip";
            buttons = {{kAnother, L"Choose another zip"}, {kRetry, L"Try again"}};
            break;
    }
    D2D1_POINT_2F c = {x + 22, y + 22};
    g_gfx.Circle(c, 22, tint);
    g_gfx.Text(glyph, Font::Icon20, R(c.x - 12, c.y - 11, 24, 24), fg, Align::Center);
    g_gfx.Text(title, Font::Subtitle, R(x + 60, y, w - 60, 28), t.text1);
    float th = g_gfx.Text(g_endText, Font::Body, R(x + 60, y + 30, w - 60, 200), t.text2, Align::Left, true);
    y = std::max(y + 44, y + 30 + th) + 22;
    Buttons(W - kPad, y, buttons, Kind::Primary);
    return y + kButtonH;
}

void Paint() {
    if (!g_gfx.Begin()) return;
    g_hits.clear();
    D2D1_SIZE_F size = g_gfx.Size();
    float W = size.width;
    float bottom = 0;
    switch (g_screen) {
        case Screen::Home: bottom = DrawHome(W); break;
        case Screen::Reading: bottom = DrawReading(W); break;
        case Screen::Ready: bottom = DrawReady(W); break;
        case Screen::Running: bottom = DrawRunning(W); break;
        case Screen::End: bottom = DrawEnd(W); break;
    }
    g_gfx.End();
    // The window is as tall as its content, like a copy dialog.
    int want = int(std::ceil((bottom + kPad) * g_gfx.Scale()));
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    if (want != rc.bottom && want != g_fitAsked) {
        g_fitAsked = want;
        PostMessageW(g_hwnd, WM_APP_FIT, WPARAM(want), 0);
    }
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

void LoadDrives() {
    g_drives.clear();
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26 && g_drives.size() < 4; i++) {
        if (!(mask & (1u << i))) continue;
        wchar_t root[] = {wchar_t(L'A' + i), L':', L'\\', 0};
        ULARGE_INTEGER avail{}, total{};
        if (GetDriveTypeW(root) == DRIVE_FIXED && GetDiskFreeSpaceExW(root, &avail, &total, nullptr) && total.QuadPart)
            g_drives.push_back({DriveName(std::wstring(root, 2)), avail.QuadPart, total.QuadPart});
    }
}

void Go(Screen s) {
    if (s == Screen::Home) {
        LoadDrives();
        g_readSeq++;   // a read still going is no longer wanted
        KillTimer(g_hwnd, kAnim);
        if (!g_running) SetTaskbar(TBPF_NOPROGRESS);
    }
    g_screen = s;
    g_focus = kNone;
    g_hover = g_press = kNone;
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// "42% Game.zip" while extracting, like a download manager; just the name otherwise.
void SetTitle(const std::wstring& prefix) {
    std::wstring t = prefix.empty() ? L"ZeroZip" : prefix + L" " + Leaf(g_a.zip);
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
    double dt = g_clockAt ? std::min(.25, double(now - g_clockAt) / 1000.0) : 0;
    g_clockAt = now;
    if (!g_paused && g_prog.bytesTotal) {
        if (g_samples.empty()) g_samples.push_back({0, 0, g_prog.bytesDone});
        g_clock += dt;
        if (g_clock >= g_samples.back().t + kStep) {
            // Speed over the last second, eased a little: progress comes in 4 MB steps,
            // so anything shorter jumps around.
            const Sample& from = g_samples[g_samples.size() - std::min<size_t>(4, g_samples.size())];
            Sample next{g_samples.back().t + kStep, 0, g_prog.bytesDone};
            double avg = double(next.bytes - std::min(next.bytes, from.bytes)) / (next.t - from.t);
            double prev = g_samples.back().rate;
            next.rate = g_samples.size() == 1 ? avg : prev + (avg - prev) * (1 - std::exp(-kStep / .25));
            if (g_samples.size() == 1) g_samples[0].rate = avg;   // start level, not with a climb from nothing
            g_rate = g_rate == 0 ? avg : g_rate + (avg - g_rate) * (1 - std::exp(-kStep / 3));
            g_samples.push_back(next);
            while (g_samples.size() > 8 && g_samples[1].t < g_clock - kDelay - kSpan - kStep) g_samples.pop_front();
            if (GraphReady()) g_shownRate = SpeedAt(g_clock - kDelay);
        }
    }
    if (g_prog.bytesTotal) {
        int pct = int(double(g_prog.bytesDone) * 100 / double(g_prog.bytesTotal));
        if (g_paused ? g_titlePct != -2 : pct != g_titlePct) {   // only when it changes; this runs every frame
            g_titlePct = g_paused ? -2 : pct;
            SetTaskbar(g_paused ? TBPF_PAUSED : TBPF_NORMAL, g_prog.bytesDone, g_prog.bytesTotal);
            SetTitle(g_paused ? L"Paused" : std::to_wstring(pct) + L"%");   // e.g. "42% Game.zip"
        }
    }
}

// The graph's height follows the highest speed on screen: it makes room quickly and settles back slowly.
void Animate() {
    if (!GraphReady()) return;
    double tip = g_clock - kDelay, peak = SpeedAt(tip);
    for (auto& p : g_samples)
        if (p.t >= tip - kSpan && p.t <= tip) peak = std::max(peak, p.rate);
    double want = std::max(peak * 1.25, 1048576.0);
    if (g_top == 0) g_top = want;
    else g_top += (want - g_top) * (want > g_top ? .1 : .015);
}

void StartRun() {
    if (g_running || g_reading > 0) return;
    g_running = true;
    g_paused = false;
    g_prog = Progress();
    g_samples.clear();
    g_clock = g_rate = g_shownRate = g_top = 0;
    g_clockAt = 0;
    g_titlePct = -1;
    {
        std::lock_guard<std::mutex> l(g_mu);
        g_shared = Progress();
    }
    g_ex.Reset();
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);   // don't let the PC sleep mid-run
    ShutdownBlockReasonCreate(g_hwnd, L"ZeroZip is extracting a zip");
    SetTimer(g_hwnd, kTick, 33, nullptr);   // 30 fps while extracting
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
    SetTaskbar(r->ok || r->stopped ? TBPF_NOPROGRESS : TBPF_ERROR, 1, 1);
    if (r->ok) {
        ShowEnd(EndKind::Done, L"Extracted to " + Compact(g_a.dest, 290));
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
        case kCancel:
        case kAnother: Go(Screen::Home); break;   // also abandons a read in progress
        case kChange: {
            std::wstring d = Pick(true, g_a.dest);
            if (!d.empty()) Read(g_a.zip, d);
            break;
        }
        case kExtract:
            if (CanExtract()) StartRun();
            break;
        case kPause:
            g_paused = !g_paused;
            g_ex.Pause(g_paused);
            g_titlePct = -1;
            Pull();
            break;
        case kStop:   // nothing is lost by stopping, so it doesn't ask
            g_paused = false;
            g_ex.Stop();
            break;
        case kOpen: ShellExecuteW(g_hwnd, L"open", g_a.dest.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
        case kResume: Read(g_a.zip, g_a.dest); break;
        case kRetry: Read(g_zip, g_zipDest); break;
    }
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

int HitAt(LPARAM lp) {
    float s = g_gfx.Scale(), x = float(GET_X_LPARAM(lp)) / s, y = float(GET_Y_LPARAM(lp)) / s;
    for (auto it = g_hits.rbegin(); it != g_hits.rend(); ++it)
        if (x >= it->r.left && x < it->r.right && y >= it->r.top && y < it->r.bottom) return it->enabled ? it->id : kNone;
    return kNone;
}

// Enter presses the screen's obvious next step, but never Extract: that deletes the zip,
// so it takes a click or an explicit Tab to it.
int DefaultButton() {
    for (auto& h : g_hits)
        if (h.enabled && (h.id == kBrowse || h.id == kOpen || h.id == kResume || h.id == kRetry)) return h.id;
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
        case WM_APP_FIT: {
            UINT dpi = GetDpiForWindow(hwnd);
            RECT r{0, 0, MulDiv(int(kWidth), int(dpi), 96), int(wp)};
            AdjustWindowRectExForDpi(&r, DWORD(GetWindowLongW(hwnd, GWL_STYLE)), FALSE, DWORD(GetWindowLongW(hwnd, GWL_EXSTYLE)), dpi);
            RECT cur;
            GetWindowRect(hwnd, &cur);
            MONITORINFO mi{sizeof mi};
            GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
            int h = std::min<int>(r.bottom - r.top, mi.rcWork.bottom - mi.rcWork.top);
            int top = std::max<int>(mi.rcWork.top, std::min<int>(cur.top, mi.rcWork.bottom - h));   // keep it on screen
            SetWindowPos(hwnd, nullptr, cur.left, top, r.right - r.left, h, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_DPICHANGED: {
            g_gfx.SetDpi(HIWORD(wp));
            auto* r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_MOUSEMOVE: {
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
            g_press = HitAt(lp);
            if (g_press) SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            int id = HitAt(lp), pressed = g_press;
            g_press = kNone;
            ReleaseCapture();
            InvalidateRect(hwnd, nullptr, FALSE);
            if (id && id == pressed) Click(id);
            return 0;
        }
        case WM_CAPTURECHANGED:
            g_press = kNone;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_KEYDOWN:
            if (lp & 0x40000000) return 0;   // a held key repeats; never let that press a button
            if (wp == VK_TAB) {
                MoveFocus(GetKeyState(VK_SHIFT) < 0);
            } else if (wp == VK_RETURN || wp == VK_SPACE) {
                int id = g_keyboard && g_focus ? g_focus : wp == VK_RETURN ? DefaultButton() : int(kNone);
                if (id) Click(id);
            } else if (wp == VK_ESCAPE) {
                if (g_screen == Screen::Ready || g_screen == Screen::End || g_screen == Screen::Reading) Go(Screen::Home);
            } else if (wp == 'O' && GetKeyState(VK_CONTROL) < 0 && !g_running) {
                Click(kBrowse);
            }
            return 0;
        case WM_DROPFILES: {
            UINT n = DragQueryFileW(HDROP(wp), 0, nullptr, 0);
            std::wstring path(n, L'\0');
            if (!g_running && n && DragQueryFileW(HDROP(wp), 0, path.data(), n + 1)) {
                SetForegroundWindow(hwnd);
                Read(path, L"");
            }
            DragFinish(HDROP(wp));
            return 0;
        }
        case WM_TIMER:
            if (wp == kTick && g_running) {
                Pull();
                Animate();
            }
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
    constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;   // a fixed-size dialog-like window
    RECT size{0, 0, MulDiv(int(kWidth), int(dpi), 96), MulDiv(244, int(dpi), 96)};
    AdjustWindowRectExForDpi(&size, style, FALSE, WS_EX_ACCEPTFILES, dpi);
    int w = size.right - size.left, h = size.bottom - size.top;
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    g_hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"ZeroZip", style,
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

    LoadDrives();
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
