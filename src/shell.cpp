// "Extract with ZeroZip" in the Windows 11 right-click menu. Explorer loads this DLL through
// ZeroZip's package; it shows the entry for a single zip and opens that zip in ZeroZip.

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>

#include <new>
#include <string>

namespace {

// {5D1F1C3E-6A51-4F6E-9C1B-2F7A8A0E1D42}, also in the package manifest
constexpr CLSID kClsid = {0x5d1f1c3e, 0x6a51, 0x4f6e, {0x9c, 0x1b, 0x2f, 0x7a, 0x8a, 0x0e, 0x1d, 0x42}};
long g_objects = 0;
HMODULE g_module = nullptr;

// Where ZeroZip.exe is now; it writes this each time it starts. If it's missing, the copy
// kept next to this DLL is used.
std::wstring ExePath() {
    wchar_t path[MAX_PATH * 2];
    DWORD size = sizeof path;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\ZeroZip", L"Path", RRF_RT_REG_SZ, nullptr, path, &size) == ERROR_SUCCESS &&
        GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
        return path;
    GetModuleFileNameW(g_module, path, MAX_PATH * 2);
    PathRemoveFileSpecW(path);
    return std::wstring(path) + L"\\ZeroZip.exe";
}

// The zip's path, if exactly one zip is selected.
std::wstring OnlyZip(IShellItemArray* items) {
    DWORD n = 0;
    IShellItem* item = nullptr;
    if (!items || FAILED(items->GetCount(&n)) || n != 1 || FAILED(items->GetItemAt(0, &item))) return {};
    PWSTR path = nullptr;
    HRESULT hr = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
    item->Release();
    if (FAILED(hr)) return {};
    std::wstring s = path;
    CoTaskMemFree(path);
    return lstrcmpiW(PathFindExtensionW(s.c_str()), L".zip") == 0 ? s : std::wstring();
}

class Command : public IExplorerCommand {
    long refs_ = 1;

public:
    Command() { InterlockedIncrement(&g_objects); }
    virtual ~Command() { InterlockedDecrement(&g_objects); }

    IFACEMETHODIMP QueryInterface(REFIID iid, void** out) override {
        static const QITAB table[] = {QITABENT(Command, IExplorerCommand), {}};
        return QISearch(this, table, iid, out);
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        long n = InterlockedDecrement(&refs_);
        if (!n) delete this;
        return n;
    }

    IFACEMETHODIMP GetTitle(IShellItemArray*, LPWSTR* name) override { return SHStrDupW(L"Extract with ZeroZip", name); }
    IFACEMETHODIMP GetIcon(IShellItemArray*, LPWSTR* icon) override { return SHStrDupW((ExePath() + L",0").c_str(), icon); }
    IFACEMETHODIMP GetToolTip(IShellItemArray*, LPWSTR* tip) override {
        *tip = nullptr;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP GetCanonicalName(GUID* name) override {
        *name = kClsid;
        return S_OK;
    }
    IFACEMETHODIMP GetState(IShellItemArray* items, BOOL, EXPCMDSTATE* state) override {
        *state = OnlyZip(items).empty() ? ECS_HIDDEN : ECS_ENABLED;
        return S_OK;
    }
    IFACEMETHODIMP Invoke(IShellItemArray* items, IBindCtx*) override {
        std::wstring zip = OnlyZip(items);
        if (zip.empty()) return E_INVALIDARG;
        std::wstring exe = ExePath(), args = L"\"" + zip + L"\"";
        return INT_PTR(ShellExecuteW(nullptr, L"open", exe.c_str(), args.c_str(), nullptr, SW_SHOWNORMAL)) > 32 ? S_OK : E_FAIL;
    }
    IFACEMETHODIMP GetFlags(EXPCMDFLAGS* flags) override {
        *flags = ECF_DEFAULT;
        return S_OK;
    }
    IFACEMETHODIMP EnumSubCommands(IEnumExplorerCommand** commands) override {
        *commands = nullptr;
        return E_NOTIMPL;
    }
};

class Factory : public IClassFactory {
public:
    IFACEMETHODIMP QueryInterface(REFIID iid, void** out) override {
        static const QITAB table[] = {QITABENT(Factory, IClassFactory), {}};
        return QISearch(this, table, iid, out);
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return 2; }   // a single static instance
    IFACEMETHODIMP_(ULONG) Release() override { return 1; }
    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID iid, void** out) override {
        *out = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        Command* c = new (std::nothrow) Command;
        if (!c) return E_OUTOFMEMORY;
        HRESULT hr = c->QueryInterface(iid, out);
        c->Release();
        return hr;
    }
    IFACEMETHODIMP LockServer(BOOL lock) override {
        lock ? InterlockedIncrement(&g_objects) : InterlockedDecrement(&g_objects);
        return S_OK;
    }
} g_factory;

}   // namespace

STDAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void** out) {
    *out = nullptr;
    return clsid == kClsid ? g_factory.QueryInterface(iid, out) : CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow() { return g_objects ? S_FALSE : S_OK; }

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
