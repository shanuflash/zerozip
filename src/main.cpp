// ZeroZip: Win32 window hosting the WebView2 UI, bridged to the engine.
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wrl.h>

#include <atomic>
#include <deque>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "WebView2.h"
#include "engine.h"
#include "resource.h"

#pragma comment(lib, "shlwapi.lib")

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kHost[] = L"https://zerozip.example/";
constexpr wchar_t kVersion[] = L"" ZEROZIP_VERSION_STR;
constexpr UINT WM_APP_JSON = WM_APP + 1;   // lParam: Message*
constexpr UINT WM_APP_DONE = WM_APP + 2;   // lParam: Finished*
constexpr UINT_PTR kTimer = 1;
constexpr BYTE kBgR = 7, kBgG = 8, kBgB = 10;
constexpr COLORREF kBg = RGB(kBgR, kBgG, kBgB);

HWND g_hwnd;
ComPtr<ICoreWebView2Environment> g_env;
ComPtr<ICoreWebView2Controller> g_ctrl;
ComPtr<ICoreWebView2> g_web;
ComPtr<ITaskbarList3> g_taskbar;
std::wstring g_startZip;
std::wstring g_uiDir;   // ZEROZIP_UI: serve the page from disk while working on it
bool g_dev = false;

Extractor g_ex;
std::thread g_worker;
bool g_running = false, g_paused = false;
ULONGLONG g_started = 0;
std::wstring g_analysis;            // last analysis sent, replayed if the page reloads mid-run
std::atomic<int> g_analyzing{0};    // analysis threads still working
std::mutex g_mu;
Progress g_prog;
struct LogItem { std::wstring name; uint64_t size; FileResult result; };
std::deque<LogItem> g_log;

struct Message { bool analysis; std::wstring json; };
struct Finished { bool ok, stopped; std::wstring json; };

// ---- JSON ----------------------------------------------------------------------

std::wstring Q(const std::wstring& s) {
    std::wstring o = L"\"";
    for (wchar_t c : s) {
        switch (c) {
            case L'"': o += L"\\\""; break;
            case L'\\': o += L"\\\\"; break;
            case L'\n': o += L"\\n"; break;
            case L'\r': o += L"\\r"; break;
            case L'\t': o += L"\\t"; break;
            default:
                if (c < 32) {
                    wchar_t b[8];
                    swprintf_s(b, L"\\u%04x", c);
                    o += b;
                } else {
                    o += c;
                }
        }
    }
    return o + L"\"";
}
std::wstring N(uint64_t v) { return std::to_wstring(v); }
std::wstring N(int64_t v) { return std::to_wstring(v); }
std::wstring B(bool v) { return v ? L"true" : L"false"; }

template <class T>
void PostOrDrop(UINT msg, T* payload) {
    if (!PostMessageW(g_hwnd, msg, 0, (LPARAM)payload)) delete payload;
}

void Post(std::wstring json, bool analysis = false) { PostOrDrop(WM_APP_JSON, new Message{analysis, std::move(json)}); }

void Send(const std::wstring& json) {
    if (g_web) g_web->PostWebMessageAsJson(json.c_str());
}

std::wstring BaseName(const std::wstring& p) {
    size_t k = p.find_last_of(L"\\/");
    return k == std::wstring::npos ? p : p.substr(k + 1);
}

std::wstring List(const std::vector<std::wstring>& v) {
    std::wstring s = L"[";
    for (size_t i = 0; i < v.size(); i++) s += (i ? L"," : L"") + Q(v[i]);
    return s + L"]";
}

// ---- analysis --------------------------------------------------------------------

std::wstring AnalysisJson(const Analysis& a) {
    std::wstring curve = L"[";
    for (size_t i = 0; i < a.curve.size(); i++) {
        wchar_t b[64];
        swprintf_s(b, L"%s[%.5f,%lld]", i ? L"," : L"", a.curve[i].first, (long long)a.curve[i].second);
        curve += b;
    }
    curve += L"]";
    std::wstring problems = L"[";
    for (size_t i = 0; i < a.problems.size(); i++)
        problems += (i ? L"," : L"") + std::wstring(L"{\"name\":") + Q(a.problems[i].name) + L",\"reason\":" +
                    Q(a.problems[i].reason) + L"}";
    problems += L"]";
    bool fits = a.problemCount == 0 && a.minFree >= int64_t(a.margin);
    return L"{\"type\":\"analysis\",\"zip\":" + Q(a.zip) + L",\"zipName\":" + Q(BaseName(a.zip)) + L",\"dest\":" +
           Q(a.dest) + L",\"zipBytes\":" + N(a.zipBytes) + L",\"unpacked\":" + N(a.unpacked) + L",\"free\":" +
           N(a.freeBytes) + L",\"normalNeed\":" + N(a.normalNeed) + L",\"minFree\":" + N(a.minFree) +
           L",\"endFree\":" + N(a.endFree) + L",\"margin\":" + N(a.margin) + L",\"files\":" + N(uint64_t(a.files)) +
           L",\"dirs\":" + N(uint64_t(a.dirs)) + L",\"present\":" + N(uint64_t(a.present)) + L",\"sameVolume\":" +
           B(a.sameVolume) + L",\"sparse\":" + B(a.sparse) + L",\"resuming\":" + B(a.resuming) + L",\"volume\":" +
           Q(a.volume) + L",\"fits\":" + B(fits) + L",\"curve\":" + curve + L",\"problems\":" + problems +
           L",\"problemCount\":" + N(uint64_t(a.problemCount)) + L",\"punchFiles\":" + N(uint64_t(a.punchFiles)) +
           L",\"punchNames\":" + List(a.punchNames) + L",\"lost\":" + Q(a.lost) + L"}";
}

void AnalyzeAsync(std::wstring zip, std::wstring dest) {
    if (g_running) return;
    Post(L"{\"type\":\"analyzing\",\"zip\":" + Q(zip) + L",\"zipName\":" + Q(BaseName(zip)) + L"}", true);
    g_analyzing++;
    std::thread([zip, dest] {
        std::wstring err, json;
        try {
            Analysis a;
            json = Analyze(zip, dest, a, err) ? AnalysisJson(a) : L"";
        } catch (const std::exception&) {
            err = L"The zip's file list is too large or damaged to read.";
        }
        if (json.empty()) json = L"{\"type\":\"analysisError\",\"zip\":" + Q(zip) + L",\"error\":" + Q(err) + L"}";
        Post(std::move(json), true);
        g_analyzing--;
    }).detach();
}

// ---- extraction --------------------------------------------------------------------

void SetTaskbar(TBPFLAG state, uint64_t done = 0, uint64_t total = 0) {
    if (!g_taskbar) return;
    g_taskbar->SetProgressState(g_hwnd, state);
    if (total && state != TBPF_NOPROGRESS) g_taskbar->SetProgressValue(g_hwnd, done, total);
}

const wchar_t* ResultName(FileResult r) {
    return r == FileResult::Verified ? L"verified" : r == FileResult::Lost ? L"lost" : L"written";
}

void SendProgress() {
    Progress p;
    std::vector<LogItem> log;
    {
        std::lock_guard<std::mutex> l(g_mu);
        p = g_prog;
        log.assign(g_log.begin(), g_log.end());
        g_log.clear();
    }
    std::wstring items = L"[";
    size_t from = log.size() > 8 ? log.size() - 8 : 0;
    for (size_t i = from; i < log.size(); i++)
        items += (i > from ? L"," : L"") + std::wstring(L"{\"name\":") + Q(log[i].name) + L",\"size\":" +
                 N(log[i].size) + L",\"result\":\"" + ResultName(log[i].result) + L"\"}";
    items += L"]";
    Send(L"{\"type\":\"progress\",\"done\":" + N(uint64_t(p.done)) + L",\"total\":" + N(uint64_t(p.total)) +
         L",\"bytesDone\":" + N(p.bytesDone) + L",\"bytesTotal\":" + N(p.bytesTotal) + L",\"zipBytes\":" +
         N(p.zipBytes) + L",\"free\":" + N(p.freeBytes) + L",\"current\":" + Q(p.current) + L",\"verifying\":" +
         B(p.verifying) + L",\"punching\":" + B(p.punching) + L",\"stopping\":" + B(g_ex.Stopped()) +
         L",\"paused\":" + B(g_paused) + L",\"elapsed\":" + N(uint64_t(GetTickCount64() - g_started)) + L",\"log\":" +
         items + L"}");
    if (p.bytesTotal) SetTaskbar(g_paused ? TBPF_PAUSED : TBPF_NORMAL, p.bytesDone, p.bytesTotal);
}

void StartRun(const std::wstring& zip, const std::wstring& dest) {
    if (g_running || g_analyzing > 0) return;
    g_running = true;
    g_paused = false;
    g_started = GetTickCount64();
    {
        std::lock_guard<std::mutex> l(g_mu);
        g_prog = Progress();
        g_log.clear();
    }
    g_ex.Reset();
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);   // don't let the PC sleep mid-run
    SetTimer(g_hwnd, kTimer, 100, nullptr);
    Send(L"{\"type\":\"started\"}");
    g_worker = std::thread([zip, dest] {
        std::wstring err;
        bool ok = false;
        try {
            ok = g_ex.Run(
                zip, dest,
                [](const Progress& p) {
                    std::lock_guard<std::mutex> l(g_mu);
                    g_prog = p;
                },
                [](const std::wstring& name, uint64_t size, FileResult result) {
                    std::lock_guard<std::mutex> l(g_mu);
                    g_log.push_back({name, size, result});
                    if (g_log.size() > 64) g_log.pop_front();
                },
                err);
        } catch (const std::exception&) {
            err = L"ZeroZip ran out of memory. Everything done so far is kept; open the zip again to resume.";
        }
        bool stopped = !ok && err.empty();
        PostOrDrop(WM_APP_DONE, new Finished{ok, stopped,
                                             L"{\"type\":\"finished\",\"ok\":" + B(ok) + L",\"stopped\":" + B(stopped) +
                                                 L",\"error\":" + Q(err) + L",\"dest\":" + Q(dest) + L",\"zip\":" +
                                                 Q(zip) + L"}"});
    });
}

void FinishRun(Finished* f) {
    if (g_worker.joinable()) g_worker.join();
    KillTimer(g_hwnd, kTimer);
    SetThreadExecutionState(ES_CONTINUOUS);
    SendProgress();
    SetTaskbar(f->ok || f->stopped ? TBPF_NOPROGRESS : TBPF_ERROR, 1, 1);
    if (!f->ok && !f->stopped) FlashWindow(g_hwnd, TRUE);
    Send(f->json);
    delete f;
    g_running = false;
    g_paused = false;
}

// ---- dialogs -------------------------------------------------------------------------

std::wstring PickFile() {
    ComPtr<IFileOpenDialog> d;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return {};
    COMDLG_FILTERSPEC spec[] = {{L"Zip archives", L"*.zip"}, {L"All files", L"*.*"}};
    d->SetFileTypes(2, spec);
    d->SetTitle(L"Choose a zip to extract");
    if (FAILED(d->Show(g_hwnd))) return {};
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(d->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return {};
    std::wstring s = path;
    CoTaskMemFree(path);
    return s;
}

std::wstring PickFolder(const std::wstring& start) {
    ComPtr<IFileOpenDialog> d;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return {};
    DWORD opts = 0;
    d->GetOptions(&opts);
    d->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    d->SetTitle(L"Extract to");
    std::wstring dir = start;
    while (!dir.empty() && GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) {
        size_t k = dir.find_last_of(L'\\');
        dir = k == std::wstring::npos ? L"" : dir.substr(0, k);
    }
    ComPtr<IShellItem> folder;
    if (!dir.empty() && SUCCEEDED(SHCreateItemFromParsingName(dir.c_str(), nullptr, IID_PPV_ARGS(&folder))))
        d->SetFolder(folder.Get());
    if (FAILED(d->Show(g_hwnd))) return {};
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(d->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return {};
    std::wstring s = path;
    CoTaskMemFree(path);
    return s;
}

// ---- messages from the page ------------------------------------------------------------

std::vector<std::wstring> Split(const std::wstring& s) {
    std::vector<std::wstring> out(1);
    for (wchar_t c : s) {
        if (c == L'\t') out.emplace_back();
        else out.back() += c;
    }
    return out;
}

void OnCommand(const std::wstring& raw, const std::vector<std::wstring>& dropped) {
    auto a = Split(raw);
    auto arg = [&](size_t i) { return i < a.size() ? a[i] : std::wstring(); };
    const std::wstring& cmd = a[0];
    if (cmd == L"ready") {
        if (g_running && !g_analysis.empty()) {   // the page was reloaded mid-run
            Send(g_analysis);
            Send(L"{\"type\":\"started\"}");
            SendProgress();
        } else if (!g_startZip.empty()) {
            AnalyzeAsync(g_startZip, L"");
            g_startZip.clear();
        }
    } else if (cmd == L"pickZip") {
        if (g_running) return;
        std::wstring z = PickFile();
        if (!z.empty()) AnalyzeAsync(z, L"");
    } else if (cmd == L"pickDest") {
        if (g_running) return;
        std::wstring d = PickFolder(arg(2));
        if (!d.empty()) AnalyzeAsync(arg(1), d);
    } else if (cmd == L"analyze") {
        AnalyzeAsync(arg(1), arg(2));
    } else if (cmd == L"drop") {
        if (!dropped.empty()) AnalyzeAsync(dropped[0], L"");
    } else if (cmd == L"start") {
        StartRun(arg(1), arg(2));
    } else if (cmd == L"pause") {
        if (g_running) { g_paused = true; g_ex.Pause(true); SendProgress(); }
    } else if (cmd == L"resume") {
        if (g_running) { g_paused = false; g_ex.Pause(false); SendProgress(); }
    } else if (cmd == L"stop") {
        if (g_running) { g_paused = false; g_ex.Stop(); SendProgress(); }
    } else if (cmd == L"open") {
        ShellExecuteW(g_hwnd, L"open", arg(1).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

// ---- serving the page ----------------------------------------------------------------------

bool LoadAsset(const std::wstring& path, std::string& data, std::wstring& type) {
    struct Asset { const wchar_t* path; int id; const wchar_t* type; };
    static const Asset kAssets[] = {
        {L"index.html", IDR_INDEX, L"text/html; charset=utf-8"},
        {L"fonts/Geist-Variable.woff2", IDR_FONT_SANS, L"font/woff2"},
        {L"fonts/GeistMono-Variable.woff2", IDR_FONT_MONO, L"font/woff2"},
    };
    for (auto& a : kAssets) {
        if (path != a.path) continue;
        type = a.type;
        if (!g_uiDir.empty()) {
            std::wstring file = g_uiDir + L"\\" + path;
            for (auto& c : file) if (c == L'/') c = L'\\';
            HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE) return false;
            LARGE_INTEGER sz{};
            GetFileSizeEx(h, &sz);
            data.resize(size_t(sz.QuadPart));
            DWORD got = 0;
            ReadFile(h, data.data(), DWORD(data.size()), &got, nullptr);
            CloseHandle(h);
            return true;
        }
        HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(a.id), RT_RCDATA);
        if (!r) return false;
        HGLOBAL g = LoadResource(nullptr, r);
        data.assign(static_cast<const char*>(LockResource(g)), SizeofResource(nullptr, r));
        return true;
    }
    return false;
}

void Resize() {
    if (!g_ctrl) return;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    g_ctrl->put_Bounds(rc);
}

void NoRuntime() {
    int r = MessageBoxW(g_hwnd,
                        L"ZeroZip needs the Microsoft Edge WebView2 Runtime, which comes with Windows 11 and most "
                        L"Windows 10 PCs.\n\nOpen the download page now?",
                        L"ZeroZip", MB_YESNO | MB_ICONINFORMATION);
    if (r == IDYES)
        ShellExecuteW(nullptr, L"open", L"https://developer.microsoft.com/microsoft-edge/webview2/", nullptr, nullptr,
                      SW_SHOWNORMAL);
    DestroyWindow(g_hwnd);
}

HRESULT OnController(HRESULT hr, ICoreWebView2Controller* ctrl) {
    if (FAILED(hr) || !ctrl) { NoRuntime(); return S_OK; }
    g_ctrl = ctrl;
    g_ctrl->get_CoreWebView2(&g_web);

    ComPtr<ICoreWebView2Controller2> c2;
    if (SUCCEEDED(g_ctrl.As(&c2))) c2->put_DefaultBackgroundColor({255, kBgR, kBgG, kBgB});

    ComPtr<ICoreWebView2Settings> s;
    g_web->get_Settings(&s);
    s->put_AreDefaultContextMenusEnabled(g_dev);
    s->put_AreDevToolsEnabled(g_dev);
    s->put_IsStatusBarEnabled(FALSE);
    s->put_IsZoomControlEnabled(FALSE);
    s->put_IsBuiltInErrorPageEnabled(FALSE);
    ComPtr<ICoreWebView2Settings3> s3;
    if (SUCCEEDED(s.As(&s3))) s3->put_AreBrowserAcceleratorKeysEnabled(g_dev);
    ComPtr<ICoreWebView2Settings4> s4;
    if (SUCCEEDED(s.As(&s4))) { s4->put_IsGeneralAutofillEnabled(FALSE); s4->put_IsPasswordAutosaveEnabled(FALSE); }

    std::wstring filter = std::wstring(kHost) + L"*";
    g_web->AddWebResourceRequestedFilter(filter.c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    g_web->add_WebResourceRequested(
        Callback<ICoreWebView2WebResourceRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                ComPtr<ICoreWebView2WebResourceRequest> req;
                args->get_Request(&req);
                PWSTR uri = nullptr;
                req->get_Uri(&uri);
                std::wstring path = uri + wcslen(kHost);
                CoTaskMemFree(uri);
                path = path.substr(0, path.find_first_of(L"?#"));
                if (path.empty()) path = L"index.html";
                std::string data;
                std::wstring type;
                ComPtr<ICoreWebView2WebResourceResponse> resp;
                if (LoadAsset(path, data, type)) {
                    ComPtr<IStream> stream;
                    stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(data.data()), UINT(data.size())));
                    std::wstring headers = L"Content-Type: " + type + L"\r\nCache-Control: no-store";
                    g_env->CreateWebResourceResponse(stream.Get(), 200, L"OK", headers.c_str(), &resp);
                } else {
                    g_env->CreateWebResourceResponse(nullptr, 404, L"Not Found", L"", &resp);
                }
                args->put_Response(resp.Get());
                return S_OK;
            })
            .Get(),
        nullptr);

    g_web->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                PWSTR msg = nullptr;
                if (FAILED(args->TryGetWebMessageAsString(&msg)) || !msg) return S_OK;
                std::wstring m = msg;
                CoTaskMemFree(msg);
                std::vector<std::wstring> dropped;
                ComPtr<ICoreWebView2WebMessageReceivedEventArgs2> a2;
                ComPtr<ICoreWebView2ObjectCollectionView> objs;
                if (SUCCEEDED(args->QueryInterface(IID_PPV_ARGS(&a2))) && SUCCEEDED(a2->get_AdditionalObjects(&objs)) && objs) {
                    UINT n = 0;
                    objs->get_Count(&n);
                    for (UINT i = 0; i < n; i++) {
                        ComPtr<IUnknown> o;
                        ComPtr<ICoreWebView2File> f;
                        PWSTR p = nullptr;
                        if (SUCCEEDED(objs->GetValueAtIndex(i, &o)) && SUCCEEDED(o.As(&f)) && SUCCEEDED(f->get_Path(&p))) {
                            dropped.push_back(p);
                            CoTaskMemFree(p);
                        }
                    }
                }
                OnCommand(m, dropped);
                return S_OK;
            })
            .Get(),
        nullptr);

    // Stay on our page: dropped files and stray links must not navigate the window away.
    g_web->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                PWSTR uri = nullptr;
                args->get_Uri(&uri);
                if (wcsncmp(uri, kHost, wcslen(kHost)) != 0) args->put_Cancel(TRUE);
                CoTaskMemFree(uri);
                return S_OK;
            })
            .Get(),
        nullptr);
    g_web->add_NewWindowRequested(
        Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                args->put_Handled(TRUE);
                return S_OK;
            })
            .Get(),
        nullptr);
    // The engine keeps running if the page's process dies; reload it and it catches up.
    g_web->add_ProcessFailed(
        Callback<ICoreWebView2ProcessFailedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
                COREWEBVIEW2_PROCESS_FAILED_KIND kind;
                args->get_ProcessFailedKind(&kind);
                if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED ||
                    kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE)
                    g_web->Reload();
                return S_OK;
            })
            .Get(),
        nullptr);

    Resize();
    g_web->Navigate((std::wstring(kHost) + L"index.html").c_str());
    return S_OK;
}

void InitWebView() {
    wchar_t local[MAX_PATH];
    std::wstring data;
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local))) data = std::wstring(local) + L"\\ZeroZip";
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, data.empty() ? nullptr : data.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT r, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(r) || !env) { NoRuntime(); return S_OK; }
                g_env = env;
                return env->CreateCoreWebView2Controller(
                    g_hwnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(OnController).Get());
            })
            .Get());
    if (FAILED(hr)) NoRuntime();
}

// ---- window ----------------------------------------------------------------------------------

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            Resize();
            return 0;
        case WM_GETMINMAXINFO: {
            UINT dpi = GetDpiForWindow(hwnd);
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            mm->ptMinTrackSize = {MulDiv(860, dpi, 96), MulDiv(620, dpi, 96)};
            return 0;
        }
        case WM_DPICHANGED: {
            auto* r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_TIMER:
            if (wp == kTimer && g_running) SendProgress();
            return 0;
        case WM_APP_JSON: {
            auto* m = reinterpret_cast<Message*>(lp);
            // A late analysis must not pull the page off the running screen.
            if (!(m->analysis && g_running)) {
                if (m->json.rfind(L"{\"type\":\"analysis\"", 0) == 0) g_analysis = m->json;
                Send(m->json);
            }
            delete m;
            return 0;
        }
        case WM_APP_DONE:
            FinishRun(reinterpret_cast<Finished*>(lp));
            return 0;
        case WM_CLOSE:
            if (g_running) {
                int r = MessageBoxW(hwnd,
                                    L"ZeroZip is still extracting.\n\nStop and close? Everything done so far is kept, "
                                    L"and opening the zip again picks up where it stopped. A file that is being freed "
                                    L"as it goes is finished first.",
                                    L"ZeroZip", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2);
                if (r != IDOK) return 0;
                g_ex.Stop();
                if (g_worker.joinable()) g_worker.join();
            }
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            g_ctrl.Reset();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1) g_startZip = argv[1];
    LocalFree(argv);
    wchar_t buf[MAX_PATH];
    if (GetEnvironmentVariableW(L"ZEROZIP_UI", buf, MAX_PATH)) g_uiDir = buf;
    g_dev = GetEnvironmentVariableW(L"ZEROZIP_DEV", buf, MAX_PATH) > 0;

    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(kBg);
    wc.lpszClassName = L"ZeroZipWindow";
    RegisterClassExW(&wc);

    UINT dpi = GetDpiForSystem();
    int w = MulDiv(1120, dpi, 96), h = MulDiv(760, dpi, 96);
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    w = std::min<int>(w, work.right - work.left);
    h = std::min<int>(h, work.bottom - work.top);
    std::wstring title = std::wstring(L"ZeroZip ") + kVersion;
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW,
                             work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 2, w,
                             h, nullptr, nullptr, inst, nullptr);
    BOOL dark = TRUE;
    COLORREF caption = kBg, border = RGB(24, 26, 30);
    DwmSetWindowAttribute(g_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof dark);
    DwmSetWindowAttribute(g_hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof caption);
    DwmSetWindowAttribute(g_hwnd, DWMWA_BORDER_COLOR, &border, sizeof border);
    ShowWindow(g_hwnd, show);
    UpdateWindow(g_hwnd);

    CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_taskbar));
    if (g_taskbar && FAILED(g_taskbar->HrInit())) g_taskbar.Reset();
    InitWebView();

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    // Analysis threads only read; give them a moment so none outlives the globals.
    for (int i = 0; i < 100 && g_analyzing > 0; i++) Sleep(50);
    g_taskbar.Reset();
    g_web.Reset();
    g_env.Reset();
    CoUninitialize();
    return 0;
}
