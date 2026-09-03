// language: C++, file: main.cpp, runtime: Win32 / x64, toolset: MSVC v143
// link: psapi.lib comctl32.lib comdlg32.lib winhttp.lib shlwapi.lib
// SubSystem: Windows
//
// Injection: LoadLibraryA + VAC hook bypass (AnarchyInjector method, proven on CS2).
// DLL fetched from server into memory, written to temp file, injected, temp deleted.
//
// License persistence: key stored in HKCU\Software\KryptiK\Key.
// Splash "Checking License Status..." -> auto-validate -> main window or key window.

#include <Windows.h>
#include <winhttp.h>
#include <TlHelp32.h>
#include <psapi.h>
#include <CommCtrl.h>
#include <commdlg.h>
#include <shlwapi.h>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <sddl.h>
#include <thread>
#include <chrono>
#include <vector>
#include <string>
#include <sstream>
#include <objbase.h>
#include <windowsx.h>

#pragma warning(disable: 6387 28251 4505)
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")

#include "resource.h"
#include <dwmapi.h>
#pragma comment(lib,"dwmapi.lib")

static const wchar_t* SERVER_HOST = L"kryptik-production.up.railway.app";
static const INTERNET_PORT SERVER_PORT = INTERNET_DEFAULT_HTTPS_PORT;

#define IDC_BTN_INJECT      102
#define IDC_STATIC_STATUS   105
#define IDC_STATIC_DOT      106
#define IDC_TIMER_POLL      1001
#define IDC_EDIT_KEY        201
#define IDC_BTN_ACTIVATE    202
#define IDC_STATIC_KEYMSG   203
#define IDC_STATIC_SPLASH   204
#define IDC_STATIC_TIER     207
#define IDC_STATIC_SVSTATUS 208
#define IDC_STATIC_INJSTATUS 209
#define WM_LICENSE_OK       (WM_USER + 1)
#define WM_LICENSE_NONE     (WM_USER + 2)


// ── Colour palette ───────────────────────────────────────────────────────────
static constexpr COLORREF CLR_BG = RGB(11, 11, 14);   // window bg
static constexpr COLORREF CLR_CARD = RGB(19, 19, 23);   // info card bg
static constexpr COLORREF CLR_CARDBDR = RGB(30, 30, 38);   // card border
static constexpr COLORREF CLR_PANEL = RGB(22, 22, 28);   // input bg
static constexpr COLORREF CLR_GREEN = RGB(0, 200, 75);
static constexpr COLORREF CLR_RED = RGB(220, 55, 55);
static constexpr COLORREF CLR_DIM = RGB(68, 68, 85);   // label text
static constexpr COLORREF CLR_WHITE = RGB(220, 220, 228);  // value text
static constexpr COLORREF CLR_BTN_BG = RGB(22, 22, 28);
static constexpr COLORREF CLR_BTN_HOV = RGB(30, 30, 38);
static constexpr COLORREF CLR_SEP = RGB(30, 30, 38);
static constexpr COLORREF CLR_BORDER = RGB(0, 200, 75);
static constexpr COLORREF CLR_YELLOW = RGB(235, 165, 0);
// Title gradient: warm coral-pink → peach
static const COLORREF GRAD_STOPS[] = {
    RGB(225, 85, 120),   // hot pink
    RGB(240, 120, 90),   // coral
    RGB(255, 185, 130),  // peach
    RGB(255, 215, 180),  // light peach
};
static const int GRAD_STOP_COUNT = 4;

static BYTE LerpByte(BYTE a, BYTE b, float t) { return(BYTE)(a + (b - a) * t); }
static COLORREF GradientColor(float t) {
    if (t <= 0.f)return GRAD_STOPS[0];
    if (t >= 1.f)return GRAD_STOPS[GRAD_STOP_COUNT - 1];
    float s = t * (GRAD_STOP_COUNT - 1); int i = (int)s; float f = s - i;
    COLORREF a = GRAD_STOPS[i], b = GRAD_STOPS[i + 1];
    return RGB(LerpByte(GetRValue(a), GetRValue(b), f),
        LerpByte(GetGValue(a), GetGValue(b), f),
        LerpByte(GetBValue(a), GetBValue(b), f));
}
static void DrawGradientText(HDC hdc, const wchar_t* txt, RECT rc, HFONT font) {
    HFONT old = (HFONT)SelectObject(hdc, font);
    SetBkMode(hdc, TRANSPARENT);
    int len = (int)wcslen(txt); if (!len) { SelectObject(hdc, old);return; }
    SIZE total; GetTextExtentPoint32W(hdc, txt, len, &total);
    int sx = rc.left + (rc.right - rc.left - total.cx) / 2;
    int y = rc.top + (rc.bottom - rc.top - total.cy) / 2, x = sx;
    for (int i = 0;i < len;++i) {
        SIZE cs; GetTextExtentPoint32W(hdc, txt + i, 1, &cs);
        float t = total.cx > 0 ? (float)(x - sx + cs.cx / 2) / (float)total.cx : 0.f;
        SetTextColor(hdc, GradientColor(t)); TextOutW(hdc, x, y, txt + i, 1); x += cs.cx;
    }
    SelectObject(hdc, old);
}

HANDLE hProcess = nullptr;
const std::wstring TARGET_PROCESS = L"cs2.exe";
static std::string  g_sessionToken;
static std::string  g_presenceToken;
static std::wstring g_keyTier, g_keyExpiry;
static HWND g_hSplashWnd = nullptr, g_hKeyWnd = nullptr, g_hEditKey = nullptr;
static HWND g_hBtnActivate = nullptr, g_hKeyMsg = nullptr;
static HWND g_hWnd = nullptr;
static HWND g_hBtnInject = nullptr;
static HWND g_hInjStatus = nullptr;      // single-line injection progress
static std::wstring g_serverStatus = L"undetected";
// Info drawn directly in WM_PAINT — no child windows needed for labels/values
static bool g_cs2Running = false, g_injecting = false, g_activating = false;
static HFONT  g_fontMono = nullptr, g_fontTitle = nullptr, g_fontSmall = nullptr;
static HBRUSH g_brBg = nullptr, g_brCard = nullptr, g_brPanel = nullptr;
static HBRUSH g_brGreen = nullptr, g_brRed = nullptr, g_brBtn = nullptr;

LRESULT CALLBACK SplashWndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK KeyWndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK MainWndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK DotProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK BtnProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
void SetInjStatus(const std::wstring& msg);
void ApplyTierAndStatus();
void DoInject();

// ── HWID ─────────────────────────────────────────────────────────────────────
static std::string GetHWID() {
    char buf[256] = {};DWORD sz = sizeof(buf);HKEY hk;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography", 0,
        KEY_READ | KEY_WOW64_64KEY, &hk) == ERROR_SUCCESS) {
        RegQueryValueExA(hk, "MachineGuid", nullptr, nullptr, (LPBYTE)buf, &sz);
        RegCloseKey(hk);
    }
    if (!buf[0]) {
        char name[MAX_COMPUTERNAME_LENGTH + 1] = {};DWORD nl = sizeof(name);
        GetComputerNameA(name, &nl);std::string s(name);unsigned long h = 5381;
        for (char c : s)h = ((h << 5) + h) + c;sprintf_s(buf, "%08lx-fallback", h);
    }
    return std::string(buf);
}

// ── REGISTRY ─────────────────────────────────────────────────────────────────
static void SaveKeyToRegistry(const std::string& key) {
    HKEY hk;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\KryptiK", 0, nullptr,
        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hk, nullptr) == ERROR_SUCCESS) {
        RegSetValueExA(hk, "Key", 0, REG_SZ, (const BYTE*)key.c_str(), (DWORD)(key.size() + 1));
        RegCloseKey(hk);
    }
}
static std::string LoadKeyFromRegistry() {
    char buf[256] = {};DWORD sz = sizeof(buf);HKEY hk;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\KryptiK", 0, KEY_READ, &hk) == ERROR_SUCCESS) {
        RegQueryValueExA(hk, "Key", nullptr, nullptr, (LPBYTE)buf, &sz);RegCloseKey(hk);
    }
    return std::string(buf);
}
static void ClearKeyFromRegistry() {
    HKEY hk;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\KryptiK", 0, KEY_WRITE, &hk) == ERROR_SUCCESS) {
        RegDeleteValueA(hk, "Key");RegCloseKey(hk);
    }
}

// ── HTTP ──────────────────────────────────────────────────────────────────────
struct HttpResult { int status = 0;std::string body;bool ok()const { return status >= 200 && status < 300; } };

static std::string JsonGet(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);if (pos == std::string::npos)return"";
    pos = json.find(':', pos + needle.size());if (pos == std::string::npos)return"";
    ++pos;while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r'))++pos;
    if (pos >= json.size())return"";
    if (json[pos] == '"') { ++pos;auto end = json.find('"', pos);return end == std::string::npos ? "" : json.substr(pos, end - pos); }
    auto end = json.find_first_of(",}\n", pos);
    std::string val = json.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    while (!val.empty() && (val.back() == ' ' || val.back() == '\r' || val.back() == '\n'))val.pop_back();
    return val;
}

static HttpResult HttpRequest(const wchar_t* method, const wchar_t* path,
    const std::string& body = "", const std::string& authToken = "")
{
    HttpResult result;
    HINTERNET hSess = WinHttpOpen(L"KryptiK/1.5", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSess)return result;
    HINTERNET hConn = WinHttpConnect(hSess, SERVER_HOST, SERVER_PORT, 0);
    if (!hConn) { WinHttpCloseHandle(hSess);return result; }
    HINTERNET hReq = WinHttpOpenRequest(hConn, method, path, nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hReq) { WinHttpCloseHandle(hConn);WinHttpCloseHandle(hSess);return result; }
    std::wstring hdrs = L"Content-Type: application/json\r\n";
    if (!authToken.empty()) {
        std::wstring tok(authToken.begin(), authToken.end());
        hdrs += L"Authorization: Bearer " + tok + L"\r\n";
    }
    BOOL sent = WinHttpSendRequest(hReq, hdrs.c_str(), (DWORD)hdrs.size(),
        body.empty() ? nullptr : (LPVOID)body.c_str(), (DWORD)body.size(), (DWORD)body.size(), 0);
    if (sent)WinHttpReceiveResponse(hReq, nullptr);
    DWORD sc = 0, sz = sizeof(sc);
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &sc, &sz, WINHTTP_NO_HEADER_INDEX);
    result.status = (int)sc;
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0) {
        std::vector<char>buf(avail + 1, 0);DWORD read = 0;
        WinHttpReadData(hReq, buf.data(), avail, &read);result.body.append(buf.data(), read);
    }
    WinHttpCloseHandle(hReq);WinHttpCloseHandle(hConn);WinHttpCloseHandle(hSess);
    return result;
}

struct ActivateResult { bool valid = false;std::string token, presence_token, tier, expires_at, error; };
static ActivateResult Activate(const std::string& key, const std::string& hwid) {
    std::string body = "{\"key\":\"" + key + "\",\"hwid\":\"" + hwid + "\"}";
    auto r = HttpRequest(L"POST", L"/activate", body);
    ActivateResult res;res.error = JsonGet(r.body, "error");
    if (JsonGet(r.body, "valid") == "true") {
        res.valid = true;res.token = JsonGet(r.body, "token");
        res.presence_token = JsonGet(r.body, "presence_token");
        res.tier = JsonGet(r.body, "tier");res.expires_at = JsonGet(r.body, "expires_at");
    }
    return res;
}

struct PresenceHandoff {
    char magic[8];
    DWORD version;
    DWORD owner_pid;
    char token[64];
};

static HANDLE CreatePresenceHandoff(DWORD pid) {
    if (g_presenceToken.empty() || g_presenceToken.size() >= 64) return nullptr;
    wchar_t name[96] = {};
    swprintf_s(name, L"Local\\KryptiKPresence_%lu", pid);
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        sizeof(PresenceHandoff), name);
    if (!mapping) return nullptr;
    auto* view = static_cast<PresenceHandoff*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0,
        sizeof(PresenceHandoff)));
    if (!view) { CloseHandle(mapping); return nullptr; }
    ZeroMemory(view, sizeof(*view));
    memcpy(view->magic, "KRYPTIK", 7);
    view->version = 1;
    view->owner_pid = pid;
    memcpy(view->token, g_presenceToken.data(), g_presenceToken.size());
    FlushViewOfFile(view, sizeof(*view));
    UnmapViewOfFile(view);
    return mapping;
}
static std::vector<BYTE> FetchDll(const std::string& token) {
    HttpResult r = HttpRequest(L"GET", L"/dll", "", token);
    if (!r.ok() || r.body.empty())return{};
    return std::vector<BYTE>(r.body.begin(), r.body.end());
}

static std::wstring FetchServerStatus() {
    HttpResult r = HttpRequest(L"GET", L"/status");
    if (!r.ok())return L"undetected";
    std::string s = JsonGet(r.body, "status");
    if (s == "updating")return L"updating";
    if (s == "detected")return L"detected";
    return L"undetected";
}

// ── PROCESS / MODULE UTILITIES ────────────────────────────────────────────────
namespace Helper {
    static bool IsElevated() {
        BOOL b = FALSE;HANDLE hTok = NULL;TOKEN_ELEVATION te = {};DWORD sz = sizeof(te);
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hTok))return FALSE;
        if (GetTokenInformation(hTok, TokenElevation, &te, sizeof(te), &sz))b = te.TokenIsElevated;
        if (hTok)CloseHandle(hTok);return b;
    }
}

static bool WaitForModules(HANDLE hProc, const std::vector<std::wstring>& names, DWORD timeoutMs) {
    auto start = std::chrono::steady_clock::now();
    std::vector<HMODULE>mods(1024);
    while (true) {
        DWORD needed = 0;
        if (!EnumProcessModules(hProc, mods.data(), (DWORD)(mods.size() * sizeof(HMODULE)), &needed)) {
            Sleep(200);goto chk;
        }
        if (needed > mods.size() * sizeof(HMODULE)) { mods.resize(needed / sizeof(HMODULE));continue; }
        {
            DWORD cnt = needed / sizeof(HMODULE);int found = 0;
            for (auto& name : names) {
                for (DWORD i = 0;i < cnt;++i) {
                    wchar_t path[MAX_PATH] = {};
                    GetModuleFileNameExW(hProc, mods[i], path, MAX_PATH);
                    std::wstring lp(path), ln(name);
                    std::transform(lp.begin(), lp.end(), lp.begin(), ::towlower);
                    std::transform(ln.begin(), ln.end(), ln.begin(), ::towlower);
                    if (lp.find(ln) != std::wstring::npos) { ++found;break; }
                }
            }
            if (found == (int)names.size())return true;
        }
    chk:
        auto el = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if ((DWORD)el > timeoutMs) { SetInjStatus(L"[WARN] Module wait timed out.");return false; }
        Sleep(200);
    }
}

static DWORD WaitForCS2(DWORD timeoutMs = 45000) {
    DWORD elapsed = 0;
    while (elapsed < timeoutMs) {
        PROCESSENTRY32W e = {};e.dwSize = sizeof(e);
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            if (Process32FirstW(snap, &e))do {
                if (_wcsicmp(e.szExeFile, L"cs2.exe") == 0) { CloseHandle(snap);return e.th32ProcessID; }
            } while (Process32NextW(snap, &e));
            CloseHandle(snap);
        }
        Sleep(200);elapsed += 200;
    }
    return 0;
}

// ── VAC HOOK BYPASS ───────────────────────────────────────────────────────────
namespace HookBypass {
    static BOOL UnhookMethod(const char* name, const wchar_t* dll, PBYTE saveOut) {
        HMODULE h = GetModuleHandleW(dll);if (!h)return FALSE;
        LPVOID addr = GetProcAddress(h, name);if (!addr)return FALSE;
        BYTE game[6] = {};
        if (!ReadProcessMemory(hProcess, addr, game, 6, nullptr))return FALSE;
        if (saveOut)memcpy(saveOut, game, 6);
        BYTE local[6] = {};memcpy(local, addr, 6);
        return WriteProcessMemory(hProcess, addr, local, 6, nullptr);
    }
    static BOOL RestoreHook(const char* name, const wchar_t* dll, PBYTE saved) {
        HMODULE h = GetModuleHandleW(dll);if (!h)return FALSE;
        LPVOID addr = GetProcAddress(h, name);if (!addr)return FALSE;
        return WriteProcessMemory(hProcess, addr, saved, 6, nullptr);
    }
    enum Idx {
        LOADLIBEXW = 1, VIRALLOC, FREELIB, LOADLIBEXA, LOADLIBW, LOADLIBA,
        VIRALLOCEX, _7, _8, _9, LDRLOADDLL, NTOPENFILE, VIRPROT,
        CREATPROW, CREATPROA, VIRPROTEX, FREELIB_, LOADLIBEXA_, LOADLIBEXW_, RESUMETHREAD
    };
    BYTE ob[30][6];
    BYTE ok[30];
    static BOOL Bypass(bool dis = false) {
        BOOL r = TRUE;
        memset(ok, 0, sizeof(ok));
#define UH(n,d,i) do { BOOL s = UnhookMethod(n,d,dis?nullptr:ob[i]); ok[i] = (BYTE)s; r &= s; } while(0)
        UH("LoadLibraryExW", L"kernel32", LOADLIBEXW);UH("VirtualAlloc", L"kernel32", VIRALLOC);
        UH("FreeLibrary", L"kernel32", FREELIB);UH("LoadLibraryExA", L"kernel32", LOADLIBEXA);
        UH("LoadLibraryW", L"kernel32", LOADLIBW);UH("LoadLibraryA", L"kernel32", LOADLIBA);
        UH("VirtualAllocEx", L"kernel32", VIRALLOCEX);UH("LdrLoadDll", L"ntdll", LDRLOADDLL);
        UH("NtOpenFile", L"ntdll", NTOPENFILE);UH("VirtualProtect", L"kernel32", VIRPROT);
        UH("CreateProcessW", L"kernel32", CREATPROW);UH("CreateProcessA", L"kernel32", CREATPROA);
        UH("VirtualProtectEx", L"kernel32", VIRPROTEX);UH("FreeLibrary", L"KernelBase", FREELIB_);
        UH("LoadLibraryExA", L"KernelBase", LOADLIBEXA_);UH("LoadLibraryExW", L"KernelBase", LOADLIBEXW_);
        UH("ResumeThread", L"KernelBase", RESUMETHREAD);
#undef UH
        return r;
    }
    static BOOL Restore() {
        BOOL r = TRUE;
#define RH(n,d,i) do { if (ok[i]) r &= RestoreHook(n,d,ob[i]); } while(0)
        RH("LoadLibraryExW", L"kernel32", LOADLIBEXW);RH("VirtualAlloc", L"kernel32", VIRALLOC);
        RH("FreeLibrary", L"kernel32", FREELIB);RH("LoadLibraryExA", L"kernel32", LOADLIBEXA);
        RH("LoadLibraryW", L"kernel32", LOADLIBW);RH("LoadLibraryA", L"kernel32", LOADLIBA);
        RH("VirtualAllocEx", L"kernel32", VIRALLOCEX);RH("LdrLoadDll", L"ntdll", LDRLOADDLL);
        RH("NtOpenFile", L"ntdll", NTOPENFILE);RH("VirtualProtect", L"kernel32", VIRPROT);
        RH("CreateProcessW", L"kernel32", CREATPROW);RH("CreateProcessA", L"kernel32", CREATPROA);
        RH("VirtualProtectEx", L"kernel32", VIRPROTEX);RH("FreeLibrary", L"KernelBase", FREELIB_);
        RH("LoadLibraryExA", L"KernelBase", LOADLIBEXA_);RH("LoadLibraryExW", L"KernelBase", LOADLIBEXW_);
        RH("ResumeThread", L"KernelBase", RESUMETHREAD);
#undef RH
        return r;
    }
}

// ── GUI HELPERS ───────────────────────────────────────────────────────────────
void SetInjStatus(const std::wstring& msg) {
    if (g_hInjStatus)SetWindowTextW(g_hInjStatus, msg.c_str());
}
// Shim — prelaunch.cpp and any legacy code still calling Log() routes here.
void Log(const std::wstring& msg) { SetInjStatus(msg); }
static bool IsCS2Running() {
    PROCESSENTRY32W e = {};e.dwSize = sizeof(e);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)return false;
    bool found = false;
    if (Process32FirstW(snap, &e))do {
        if (_wcsicmp(e.szExeFile, L"cs2.exe") == 0) { found = true;break; }
    } while (Process32NextW(snap, &e));
    CloseHandle(snap);return found;
}
static void UpdateCS2Status(bool running) {
    if (g_cs2Running == running)return;
    g_cs2Running = running;
    if (g_hWnd)InvalidateRect(g_hWnd, nullptr, FALSE);
}
struct BtnState { bool hover = false, pressed = false; };
static BtnState g_btnInjectState, g_btnActivateState;
static void DrawHackerButton(HDC hdc, RECT rc, const wchar_t* txt, bool hover, bool pressed, bool enabled = true) {
    // fill outer rect with bg first to erase corners cleanly
    HBRUSH bgBr = CreateSolidBrush(CLR_BG);FillRect(hdc, &rc, bgBr);DeleteObject(bgBr);
    COLORREF bg = !enabled ? RGB(15, 15, 20) : pressed ? RGB(32, 32, 40) : hover ? RGB(28, 28, 36) : CLR_BTN_BG;
    COLORREF col = !enabled ? CLR_DIM : CLR_WHITE;
    COLORREF bor = !enabled ? RGB(35, 35, 45) : (hover || pressed) ? RGB(70, 70, 90) : RGB(38, 38, 50);
    HBRUSH hBr = CreateSolidBrush(bg);
    HPEN pen = CreatePen(PS_SOLID, 1, bor);
    HPEN op = (HPEN)SelectObject(hdc, pen);
    HBRUSH ob = (HBRUSH)SelectObject(hdc, hBr);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 10, 10);
    SelectObject(hdc, op);SelectObject(hdc, ob);DeleteObject(pen);DeleteObject(hBr);
    SetBkMode(hdc, TRANSPARENT);SetTextColor(hdc, col);SelectObject(hdc, g_fontMono);
    DrawTextW(hdc, txt, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// ── INJECT THREAD ─────────────────────────────────────────────────────────────
void DoInject() {
    g_injecting = true;EnableWindow(g_hBtnInject, FALSE);

    SetInjStatus(L"Fetching cheat...");

    std::vector<BYTE> dllBytes = FetchDll(g_sessionToken);
    if (dllBytes.empty()) {
        SetInjStatus(L"Error: server unreachable or key expired.");
        EnableWindow(g_hBtnInject, TRUE);g_injecting = false;return;
    }
    SetInjStatus(L"Cheat received. Launching CS2...");

    // write to temp file
    wchar_t tmpDir[MAX_PATH] = {}, tmpPath[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmpDir);
    swprintf_s(tmpPath, L"%skryptik_%u.dll", tmpDir, (DWORD)GetTickCount64());
    {
        HANDLE hf = CreateFileW(tmpPath, GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hf == INVALID_HANDLE_VALUE) {
            SetInjStatus(L"Error: failed to write temp file.");
            SecureZeroMemory(dllBytes.data(), dllBytes.size());
            EnableWindow(g_hBtnInject, TRUE);g_injecting = false;return;
        }
        DWORD written = 0;
        WriteFile(hf, dllBytes.data(), (DWORD)dllBytes.size(), &written, nullptr);
        CloseHandle(hf);
        SecureZeroMemory(dllBytes.data(), dllBytes.size());
    }

    // launch CS2 if not running
    if (!IsCS2Running()) {

        ShellExecuteW(nullptr, L"open", L"steam://rungameid/730", nullptr, nullptr, SW_SHOW);
    }

    // wait for PID
    SetInjStatus(L"Waiting for CS2...");
    DWORD cs2pid = WaitForCS2(45000);
    if (!cs2pid) {
        SetInjStatus(L"Error: CS2 did not start within 45s.");
        DeleteFileW(tmpPath);EnableWindow(g_hBtnInject, TRUE);g_injecting = false;return;
    }


    // open process
    DWORD acc = Helper::IsElevated() ? PROCESS_ALL_ACCESS :
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ;
    hProcess = OpenProcess(acc, FALSE, cs2pid);
    if (!hProcess) {
        SetInjStatus(L"Error: could not open CS2. Run as admin.");
        DeleteFileW(tmpPath);EnableWindow(g_hBtnInject, TRUE);g_injecting = false;return;
    }

    // wait for game modules
    SetInjStatus(L"Waiting for game to load...");
    std::vector<std::wstring> gameMods = { L"client.dll",L"engine2.dll",L"server.dll" };
    WaitForModules(hProcess, gameMods, 90000);


    // stabilise 20s
    SetInjStatus(L"Stabilizing...");
    for (int i = 0;i < 200;++i) {
        Sleep(100);DWORD ex = 0;
        if (!GetExitCodeProcess(hProcess, &ex) || ex != STILL_ACTIVE) {
            SetInjStatus(L"Error: CS2 crashed during wait.");
            CloseHandle(hProcess);hProcess = nullptr;
            DeleteFileW(tmpPath);EnableWindow(g_hBtnInject, TRUE);g_injecting = false;return;
        }
    }

    // bypass VAC hooks
    HookBypass::Bypass();


    // inject via LoadLibraryA
    SetInjStatus(L"Injecting...");
    HANDLE presenceMapping = CreatePresenceHandoff(cs2pid);
    char tmpPathA[MAX_PATH] = {};
    WideCharToMultiByte(CP_ACP, 0, tmpPath, -1, tmpPathA, MAX_PATH, nullptr, nullptr);

    LPVOID remPath = VirtualAllocEx(hProcess, nullptr, strlen(tmpPathA) + 1,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remPath) {
        SetInjStatus(L"Error: memory allocation failed.");
        HookBypass::Restore();CloseHandle(hProcess);hProcess = nullptr;
        if (presenceMapping) CloseHandle(presenceMapping);
        DeleteFileW(tmpPath);EnableWindow(g_hBtnInject, TRUE);g_injecting = false;return;
    }

    WriteProcessMemory(hProcess, remPath, tmpPathA, strlen(tmpPathA) + 1, nullptr);

    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
    LPTHREAD_START_ROUTINE pLoadLib = (LPTHREAD_START_ROUTINE)GetProcAddress(hK32, "LoadLibraryA");

    HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0, pLoadLib, remPath, 0, nullptr);
    if (!hThread) {
        SetInjStatus(L"Error: CreateRemoteThread failed.");
        VirtualFreeEx(hProcess, remPath, 0, MEM_RELEASE);
        HookBypass::Restore();CloseHandle(hProcess);hProcess = nullptr;
        if (presenceMapping) CloseHandle(presenceMapping);
        DeleteFileW(tmpPath);EnableWindow(g_hBtnInject, TRUE);g_injecting = false;return;
    }

    WaitForSingleObject(hThread, 10000);
    DWORD exitCode = 0;GetExitCodeThread(hThread, &exitCode);
    CloseHandle(hThread);VirtualFreeEx(hProcess, remPath, 0, MEM_RELEASE);
    Sleep(5000);
    if (presenceMapping) CloseHandle(presenceMapping);

    // leave ntdll/k32 unhooked — Restore() put VAC bytes back and
    // wrote zeros onto APIs that failed to save. that pops mid-match
    // when the game hits NtOpenFile / VirtualProtect / CreateProcess.
    if (!DeleteFileW(tmpPath))MoveFileExW(tmpPath, nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);

    if (exitCode != 0) {
        SetInjStatus(L"Injected. Closing loader...");
        Sleep(1200);
        PostMessageW(g_hWnd, WM_CLOSE, 0, 0);  // clean exit from UI thread
        g_injecting = false;
        return;
    }
    else {
        SetInjStatus(L"Error: DLL failed to load. Check CS2 version & admin rights.");
    }

    CloseHandle(hProcess);hProcess = nullptr;
    EnableWindow(g_hBtnInject, TRUE);g_injecting = false;
}

// ── ACTIVATE / TRANSITION ─────────────────────────────────────────────────────
static void ApplyTierAndStatus() {
    if (g_hWnd)InvalidateRect(g_hWnd, nullptr, FALSE);
    SetInjStatus(L"Ready - Press Launch && Inject.");
}

static void TransitionToMain(const ActivateResult& res) {
    g_sessionToken = res.token;
    g_presenceToken = res.presence_token;
    g_keyTier = std::wstring(res.tier.begin(), res.tier.end());
    g_keyExpiry = std::wstring(res.expires_at.begin(), res.expires_at.end());
    // fetch status if not already done
    if (g_serverStatus.empty())g_serverStatus = FetchServerStatus();
    if (g_hSplashWnd)ShowWindow(g_hSplashWnd, SW_HIDE);
    if (g_hKeyWnd)ShowWindow(g_hKeyWnd, SW_HIDE);
    ShowWindow(g_hWnd, SW_SHOW);UpdateWindow(g_hWnd);
    ApplyTierAndStatus();
}

void DoActivate(std::wstring keyW) {
    g_activating = true;
    if (g_hBtnActivate)EnableWindow(g_hBtnActivate, FALSE);
    if (g_hKeyMsg)SetWindowTextW(g_hKeyMsg, L"contacting server...");
    std::string key(keyW.begin(), keyW.end()), hwid = GetHWID();
    key.erase(0, key.find_first_not_of(" \t\r\n"));
    if (!key.empty())key.erase(key.find_last_not_of(" \t\r\n") + 1);
    if (key.empty()) {
        if (g_hKeyMsg)SetWindowTextW(g_hKeyMsg, L"enter a license key.");
        if (g_hBtnActivate)EnableWindow(g_hBtnActivate, TRUE);
        g_activating = false;return;
    }
    auto res = Activate(key, hwid);
    if (res.valid) { SaveKeyToRegistry(key);TransitionToMain(res); }
    else {
        std::wstring errMsg;
        if (res.error == "invalid_key")      errMsg = L"invalid license key.";
        else if (res.error == "key_revoked") errMsg = L"this key has been revoked.";
        else if (res.error == "key_expired") errMsg = L"license expired.";
        else if (res.error == "wrong_hwid")  errMsg = L"key locked to another machine.";
        else if (res.error == "too_many_requests")errMsg = L"slow down — too many attempts.";
        else if (res.error.empty())        errMsg = L"server unreachable.";
        else errMsg = L"error: " + std::wstring(res.error.begin(), res.error.end());
        if (g_hKeyMsg)SetWindowTextW(g_hKeyMsg, errMsg.c_str());
        if (g_hBtnActivate)EnableWindow(g_hBtnActivate, TRUE);
    }
    g_activating = false;
}

void DoCheckLicense() {
    std::string storedKey = LoadKeyFromRegistry();
    if (storedKey.empty()) { PostMessageW(g_hSplashWnd, WM_LICENSE_NONE, 0, 0);return; }
    std::string hwid = GetHWID();
    auto res = Activate(storedKey, hwid);
    if (res.valid) {
        g_sessionToken = res.token;
        g_presenceToken = res.presence_token;
        g_keyTier = std::wstring(res.tier.begin(), res.tier.end());
        g_keyExpiry = std::wstring(res.expires_at.begin(), res.expires_at.end());
        g_serverStatus = FetchServerStatus();
        PostMessageW(g_hSplashWnd, WM_LICENSE_OK, 0, 0);
    }
    else {
        if (res.error == "key_expired" || res.error == "key_revoked" || res.error == "invalid_key")
            ClearKeyFromRegistry();
        PostMessageW(g_hSplashWnd, WM_LICENSE_NONE, 0, 0);
    }
}

// ── DOT / BUTTON PROCS ────────────────────────────────────────────────────────
LRESULT CALLBACK DotProc(HWND hwnd, UINT msg, WPARAM, LPARAM) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;GetClientRect(hwnd, &rc);
        HBRUSH br = CreateSolidBrush(CLR_BG);FillRect(hdc, &rc, br);DeleteObject(br);
        COLORREF c = g_cs2Running ? CLR_GREEN : CLR_RED;
        br = CreateSolidBrush(c);HPEN pen = CreatePen(PS_SOLID, 0, c);
        HPEN op = (HPEN)SelectObject(hdc, pen);HBRUSH ob = (HBRUSH)SelectObject(hdc, br);
        Ellipse(hdc, rc.left + 1, rc.top + 1, rc.right - 1, rc.bottom - 1);
        SelectObject(hdc, op);SelectObject(hdc, ob);DeleteObject(br);DeleteObject(pen);
        EndPaint(hwnd, &ps);return 0;
    }
    return DefWindowProcW(hwnd, msg, 0, 0);
}
LRESULT CALLBACK BtnProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    BtnState* s = (id == IDC_BTN_INJECT) ? &g_btnInjectState : &g_btnActivateState;
    switch (msg) {
    case WM_MOUSEMOVE:if (!s->hover) { s->hover = true;SetCapture(hwnd);InvalidateRect(hwnd, nullptr, TRUE); }break;
    case WM_MOUSELEAVE:case WM_CAPTURECHANGED:s->hover = s->pressed = false;ReleaseCapture();InvalidateRect(hwnd, nullptr, TRUE);break;
    case WM_LBUTTONDOWN:s->pressed = true;InvalidateRect(hwnd, nullptr, TRUE);break;
    case WM_LBUTTONUP:if (s->pressed) {
        s->pressed = false;InvalidateRect(hwnd, nullptr, TRUE);
        SendMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM((WORD)id, BN_CLICKED), (LPARAM)hwnd);
    }break;
    case WM_PAINT: {
        PAINTSTRUCT ps;HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;GetClientRect(hwnd, &rc);wchar_t t[64];GetWindowTextW(hwnd, t, 64);
        DrawHackerButton(hdc, rc, t, s->hover, s->pressed, IsWindowEnabled(hwnd));
        EndPaint(hwnd, &ps);return 0;
    }
    case WM_ERASEBKGND:return 1;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// ── SPLASH WINDOW ─────────────────────────────────────────────────────────────
LRESULT CALLBACK SplashWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HWND hs = CreateWindowExW(0, L"STATIC", L"Checking License Status...",
            WS_CHILD | WS_VISIBLE | SS_CENTER, 0, 68, 380, 18, hwnd,
            (HMENU)(UINT_PTR)IDC_STATIC_SPLASH, nullptr, nullptr);
        SendMessageW(hs, WM_SETFONT, (WPARAM)g_fontMono, TRUE);return 0;
    }
    case WM_LICENSE_OK:
        ShowWindow(hwnd, SW_HIDE);ShowWindow(g_hWnd, SW_SHOW);UpdateWindow(g_hWnd);
        ApplyTierAndStatus();return 0;
    case WM_LICENSE_NONE:
        ShowWindow(hwnd, SW_HIDE);ShowWindow(g_hKeyWnd, SW_SHOW);UpdateWindow(g_hKeyWnd);return 0;
    case WM_CTLCOLORSTATIC: { HDC hdc = (HDC)wp;SetBkMode(hdc, TRANSPARENT);SetTextColor(hdc, CLR_DIM);return(LRESULT)g_brBg; }
    case WM_ERASEBKGND: { RECT rc;GetClientRect(hwnd, &rc);FillRect((HDC)wp, &rc, g_brBg);return 1; }
    case WM_PAINT: {
        PAINTSTRUCT ps;HDC hdc = BeginPaint(hwnd, &ps);int W = 380;
        RECT tr = { 0,10,W,52 };DrawGradientText(hdc, L"KryptiK Loader", tr, g_fontTitle);
        HPEN sep = CreatePen(PS_SOLID, 2, CLR_SEP);HPEN op = (HPEN)SelectObject(hdc, sep);
        MoveToEx(hdc, 0, 60, nullptr);LineTo(hdc, W, 60);SelectObject(hdc, op);DeleteObject(sep);
        EndPaint(hwnd, &ps);return 0;
    }
    case WM_NCHITTEST: {
        POINT pt = { GET_X_LPARAM(lp),GET_Y_LPARAM(lp) };ScreenToClient(hwnd, &pt);
        if (pt.y < 52)return HTCAPTION;return HTCLIENT;
    }
    case WM_DESTROY:PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── KEY WINDOW ────────────────────────────────────────────────────────────────
LRESULT CALLBACK KeyWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        int W = 420, m = 28;
        HWND hSub = CreateWindowExW(0, L"STATIC", L"License Activation", WS_CHILD | WS_VISIBLE | SS_CENTER, 0, 58, W, 16, hwnd, nullptr, nullptr, nullptr);
        SendMessageW(hSub, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);
        HWND hL = CreateWindowExW(0, L"STATIC", L"License Key", WS_CHILD | WS_VISIBLE | SS_LEFT, m, 96, 200, 14, hwnd, nullptr, nullptr, nullptr);
        SendMessageW(hL, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);
        g_hEditKey = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL, m, 114, W - m * 2, 26, hwnd, (HMENU)(UINT_PTR)IDC_EDIT_KEY, nullptr, nullptr);
        SendMessageW(g_hEditKey, WM_SETFONT, (WPARAM)g_fontMono, TRUE);
        g_hKeyMsg = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_CENTER, m, 148, W - m * 2, 15, hwnd, (HMENU)(UINT_PTR)IDC_STATIC_KEYMSG, nullptr, nullptr);
        SendMessageW(g_hKeyMsg, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);
        g_hBtnActivate = CreateWindowExW(0, L"BUTTON", L"Login", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, m, 170, W - m * 2, 30, hwnd, (HMENU)(UINT_PTR)IDC_BTN_ACTIVATE, nullptr, nullptr);
        SendMessageW(g_hBtnActivate, WM_SETFONT, (WPARAM)g_fontMono, TRUE);
        SetWindowSubclass(g_hBtnActivate, BtnProc, IDC_BTN_ACTIVATE, 0);
        HWND hF = CreateWindowExW(0, L"STATIC", L"By C7.Gabe & Python", WS_CHILD | WS_VISIBLE | SS_CENTER | SS_NOPREFIX, 0, 212, W, 13, hwnd, nullptr, nullptr, nullptr);
        SendMessageW(hF, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);return 0;
    }
    case WM_COMMAND:
        if ((LOWORD(wp) == IDC_BTN_ACTIVATE && HIWORD(wp) == BN_CLICKED) ||
            (LOWORD(wp) == IDC_EDIT_KEY && HIWORD(wp) == EN_CHANGE && GetAsyncKeyState(VK_RETURN) < 0)) {
            if (g_activating)return 0;wchar_t buf[256] = {};GetWindowTextW(g_hEditKey, buf, 256);
            std::thread(DoActivate, std::wstring(buf)).detach();
        }return 0;
    case WM_KEYDOWN:if (wp == VK_RETURN && !g_activating) { wchar_t buf[256] = {};GetWindowTextW(g_hEditKey, buf, 256);std::thread(DoActivate, std::wstring(buf)).detach(); }return 0;
    case WM_CTLCOLORSTATIC: { HDC hdc = (HDC)wp;SetBkMode(hdc, TRANSPARENT);int id = GetDlgCtrlID((HWND)lp);SetTextColor(hdc, id == IDC_STATIC_KEYMSG ? CLR_RED : CLR_DIM);return(LRESULT)g_brBg; }
    case WM_CTLCOLOREDIT: { HDC hdc = (HDC)wp;SetBkColor(hdc, CLR_PANEL);SetTextColor(hdc, CLR_WHITE);return(LRESULT)g_brPanel; }
    case WM_CTLCOLORBTN:return(LRESULT)g_brBg;
    case WM_ERASEBKGND: { RECT rc;GetClientRect(hwnd, &rc);FillRect((HDC)wp, &rc, g_brBg);return 1; }
    case WM_PAINT: {
        PAINTSTRUCT ps;HDC hdc = BeginPaint(hwnd, &ps);int W = 420;
        HPEN xp = CreatePen(PS_SOLID, 2, CLR_DIM);HPEN ox = (HPEN)SelectObject(hdc, xp);
        MoveToEx(hdc, W - 21, 9, nullptr);LineTo(hdc, W - 9, 21);MoveToEx(hdc, W - 9, 9, nullptr);LineTo(hdc, W - 21, 21);
        SelectObject(hdc, ox);DeleteObject(xp);
        RECT tr = { 0,10,W - 30,52 };DrawGradientText(hdc, L"KryptiK Loader | Login", tr, g_fontTitle);
        HPEN sp = CreatePen(PS_SOLID, 1, CLR_SEP);HPEN os = (HPEN)SelectObject(hdc, sp);
        MoveToEx(hdc, 0, 79, nullptr);LineTo(hdc, W, 79);SelectObject(hdc, os);DeleteObject(sp);
        RECT er = { 27,113,W - 27,141 };HPEN ep = CreatePen(PS_SOLID, 1, RGB(55, 55, 55));HPEN oe = (HPEN)SelectObject(hdc, ep);
        HBRUSH nb = (HBRUSH)GetStockObject(NULL_BRUSH);HBRUSH ob = (HBRUSH)SelectObject(hdc, nb);
        Rectangle(hdc, er.left, er.top, er.right, er.bottom);SelectObject(hdc, oe);SelectObject(hdc, ob);DeleteObject(ep);
        EndPaint(hwnd, &ps);return 0;
    }
    case WM_NCHITTEST: {
        POINT pt = { GET_X_LPARAM(lp),GET_Y_LPARAM(lp) };ScreenToClient(hwnd, &pt);
        RECT rc;GetClientRect(hwnd, &rc);if (pt.x >= rc.right - 30 && pt.y < 30)return HTCLIENT;if (pt.y < 30)return HTCAPTION;return HTCLIENT;
    }
    case WM_LBUTTONDOWN: { RECT rc;GetClientRect(hwnd, &rc);if (GET_Y_LPARAM(lp) < 30 && GET_X_LPARAM(lp) >= rc.right - 30)PostMessageW(hwnd, WM_CLOSE, 0, 0);return 0; }
    case WM_DESTROY:PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── MAIN WINDOW ───────────────────────────────────────────────────────────────
LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEACTIVATE:
        // WM_MOUSEACTIVATE fires before the window is brought to foreground.
        // If we are not already the foreground window and the click lands on
        // the client area (where buttons live), activate but eat the click so
        // background-clicks never fire the inject button.
        // Title-bar hits (HTCAPTION) pass through so dragging still works.
        if (GetForegroundWindow() != hwnd && LOWORD(lp) == HTCLIENT)
            return MA_ACTIVATEANDEAT;
        return MA_ACTIVATE;

    case WM_CREATE: {
        g_hWnd = hwnd;
        const int W = 480, m = 16;
        // All info rows (TARGET / SUBSCRIPTION / CHEAT STATUS) are drawn in
        // WM_PAINT — no child windows needed for them.

        // Injection status text — single line, updates dynamically
        g_hInjStatus = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_CENTER,
            m, 232, W - m * 2, 16, hwnd, (HMENU)(UINT_PTR)IDC_STATIC_INJSTATUS, nullptr, nullptr);
        SendMessageW(g_hInjStatus, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);

        // Inject button
        g_hBtnInject = CreateWindowExW(0, L"BUTTON", L"Launch && Inject",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            m, 256, W - m * 2, 40, hwnd, (HMENU)(UINT_PTR)IDC_BTN_INJECT, nullptr, nullptr);
        SendMessageW(g_hBtnInject, WM_SETFONT, (WPARAM)g_fontMono, TRUE);
        SetWindowSubclass(g_hBtnInject, BtnProc, IDC_BTN_INJECT, 0);

        // Footer
        HWND hF = CreateWindowExW(0, L"STATIC", L"By C7.Gabe & Python",
            WS_CHILD | WS_VISIBLE | SS_CENTER | SS_NOPREFIX, 0, 308, W, 13, hwnd, nullptr, nullptr, nullptr);
        SendMessageW(hF, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);

        SetTimer(hwnd, IDC_TIMER_POLL, 1000, nullptr);return 0;
    }
    case WM_TIMER:if (wp == IDC_TIMER_POLL)UpdateCS2Status(IsCS2Running());return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_BTN_INJECT && HIWORD(wp) == BN_CLICKED) {
            if (g_injecting)return 0;if (g_sessionToken.empty()) { SetInjStatus(L"Not authenticated.");return 0; }
            std::thread(DoInject).detach();
        }return 0;
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wp;SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, CLR_DIM);   // footer + inj status in dim
        return(LRESULT)g_brBg;
    }
    case WM_CTLCOLORBTN:return(LRESULT)g_brBg;
    case WM_ERASEBKGND: { RECT rc;GetClientRect(hwnd, &rc);FillRect((HDC)wp, &rc, g_brBg);return 1; }
    case WM_PAINT: {
        PAINTSTRUCT ps;HDC hdc = BeginPaint(hwnd, &ps);
        const int W = 480, m = 16;

        // ── Background ───────────────────────────────────────────────────────
        RECT full;GetClientRect(hwnd, &full);
        HBRUSH bgBr = CreateSolidBrush(CLR_BG);FillRect(hdc, &full, bgBr);DeleteObject(bgBr);

        // ── Green square icon ─────────────────────────────────────────────────
        HBRUSH greenBr = CreateSolidBrush(CLR_GREEN);
        HBRUSH oldBr = (HBRUSH)SelectObject(hdc, greenBr);
        HPEN noPen = (HPEN)SelectObject(hdc, GetStockObject(NULL_PEN));
        Rectangle(hdc, m, 14, m + 10, 24);
        SelectObject(hdc, oldBr);SelectObject(hdc, noPen);DeleteObject(greenBr);

        // ── Title text ────────────────────────────────────────────────────────
        RECT tr = { m + 14,8,W - 60,48 };DrawGradientText(hdc, L"KryptiK Loader  |  CS2", tr, g_fontTitle);

        // ── Window controls (─ ×) ─────────────────────────────────────────────
        HPEN ctrlPen = CreatePen(PS_SOLID, 2, CLR_DIM);
        HPEN oldPen = (HPEN)SelectObject(hdc, ctrlPen);
        // × close
        MoveToEx(hdc, W - 22, 10, nullptr);LineTo(hdc, W - 10, 22);
        MoveToEx(hdc, W - 10, 10, nullptr);LineTo(hdc, W - 22, 22);
        // ─ minimize
        MoveToEx(hdc, W - 52, 16, nullptr);LineTo(hdc, W - 36, 16);
        SelectObject(hdc, oldPen);DeleteObject(ctrlPen);

        // ── Separator under title bar ─────────────────────────────────────────
        HPEN sepPen = CreatePen(PS_SOLID, 1, CLR_SEP);
        oldPen = (HPEN)SelectObject(hdc, sepPen);
        MoveToEx(hdc, 0, 54, nullptr);LineTo(hdc, W, 54);
        SelectObject(hdc, oldPen);DeleteObject(sepPen);

        // ── Helper lambdas for card drawing ──────────────────────────────────
        const int CARD_X = m, CARD_W = W - m * 2, CARD_R = 8, CARD_H = 42;

        auto drawCard = [&](int cy) {
            HBRUSH cb = CreateSolidBrush(CLR_CARD);
            HPEN   cp = CreatePen(PS_SOLID, 1, CLR_CARDBDR);
            HPEN  op2 = (HPEN)SelectObject(hdc, cp);
            HBRUSH ob2 = (HBRUSH)SelectObject(hdc, cb);
            RoundRect(hdc, CARD_X, cy, CARD_X + CARD_W, cy + CARD_H, CARD_R, CARD_R);
            SelectObject(hdc, op2);SelectObject(hdc, ob2);
            DeleteObject(cb);DeleteObject(cp);
            };

        auto drawLabel = [&](const wchar_t* txt, int cy) {
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, CLR_DIM);
            SelectObject(hdc, g_fontSmall);
            RECT lr = { CARD_X + 14,cy + 13,CARD_X + 130,cy + CARD_H - 6 };
            DrawTextW(hdc, txt, -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            };

        auto drawValue = [&](const wchar_t* txt, COLORREF col, int cy) {
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, col);
            SelectObject(hdc, g_fontMono);
            RECT vr = { CARD_X + 130,cy + 6,CARD_X + CARD_W - 12,cy + CARD_H - 6 };
            DrawTextW(hdc, txt, -1, &vr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            };

        // ── Card 1: PRODUCT ───────────────────────────────────────────────────
        const int C1 = 66;drawCard(C1);drawLabel(L"PRODUCT", C1);
        drawValue(L"KryptiK  \u2014  Counter Strike 2", CLR_WHITE, C1);

        // ── Card 2: SUBSCRIPTION ──────────────────────────────────────────────
        const int C2 = C1 + CARD_H + 6;drawCard(C2);drawLabel(L"SUBSCRIPTION", C2);
        {
            std::wstring tierStr = L"—";
            if (g_keyTier == L"lifetime")     tierStr = L"LIFETIME";
            else if (g_keyTier == L"3month")  tierStr = L"3 MONTH";
            else if (g_keyTier == L"month")   tierStr = L"MONTHLY";
            if (!g_keyExpiry.empty() && g_keyTier != L"lifetime")
                tierStr += L"   expires " + g_keyExpiry.substr(0, 10);
            drawValue(tierStr.c_str(), CLR_GREEN, C2);
        }

        // ── Card 3: CHEAT STATUS ──────────────────────────────────────────────
        const int C3 = C2 + CARD_H + 6;drawCard(C3);drawLabel(L"CHEAT STATUS", C3);
        {
            COLORREF sc = CLR_GREEN;
            std::wstring sv = L"UNDETECTED";
            if (g_serverStatus == L"updating") { sc = CLR_YELLOW;sv = L"UPDATING"; }
            else if (g_serverStatus == L"detected") { sc = CLR_RED;sv = L"DETECTED"; }
            // dot
            int dotX = CARD_X + CARD_W - 14 - 80, dotY = C3 + CARD_H / 2 - 4;
            // draw right-aligned: measure text first
            SelectObject(hdc, g_fontMono);SIZE tsz;
            GetTextExtentPoint32W(hdc, sv.c_str(), (int)sv.size(), &tsz);
            int totalW = 8 + 4 + tsz.cx; // dot+gap+text
            int startX = CARD_X + CARD_W - 12 - totalW;
            // dot circle
            HBRUSH db = CreateSolidBrush(sc);HPEN dp = CreatePen(PS_SOLID, 0, sc);
            HPEN odp = (HPEN)SelectObject(hdc, dp);HBRUSH odb = (HBRUSH)SelectObject(hdc, db);
            Ellipse(hdc, startX, C3 + (CARD_H - 8) / 2, startX + 8, C3 + (CARD_H - 8) / 2 + 8);
            SelectObject(hdc, odp);SelectObject(hdc, odb);DeleteObject(db);DeleteObject(dp);
            // status text
            SetBkMode(hdc, TRANSPARENT);SetTextColor(hdc, sc);
            RECT sr = { startX + 12,C3 + 6,CARD_X + CARD_W - 10,C3 + CARD_H - 6 };
            DrawTextW(hdc, sv.c_str(), -1, &sr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }

        EndPaint(hwnd, &ps);return 0;
    }
    case WM_NCHITTEST: {
        POINT pt = { GET_X_LPARAM(lp),GET_Y_LPARAM(lp) };ScreenToClient(hwnd, &pt);
        RECT rc;GetClientRect(hwnd, &rc);
        if (pt.x >= rc.right - 30 && pt.y < 32)return HTCLIENT;  // close zone
        if (pt.x >= rc.right - 60 && pt.y < 32)return HTCLIENT;  // minimize zone
        if (pt.y < 54)return HTCAPTION;
        return HTCLIENT;
    }
    case WM_LBUTTONDOWN: {
        RECT rc2;GetClientRect(hwnd, &rc2);int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (y < 54) {
            if (x >= rc2.right - 30)PostMessageW(hwnd, WM_CLOSE, 0, 0);
            else if (x >= rc2.right - 60)ShowWindow(hwnd, SW_MINIMIZE);
        }return 0;
    }
    case WM_DESTROY:KillTimer(hwnd, IDC_TIMER_POLL);PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── ENTRY POINT ───────────────────────────────────────────────────────────────
int WINAPI WinMain(_In_ HINSTANCE hInst, _In_opt_ HINSTANCE, _In_ LPSTR, _In_ int) {
    INITCOMMONCONTROLSEX icc = { sizeof(icc),ICC_WIN95_CLASSES };InitCommonControlsEx(&icc);
    g_fontTitle = CreateFontW(24, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_fontMono = CreateFontW(13, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, 0, 0, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Consolas");
    g_fontSmall = CreateFontW(12, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, 0, 0, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_brBg = CreateSolidBrush(CLR_BG);g_brCard = CreateSolidBrush(CLR_CARD);g_brPanel = CreateSolidBrush(CLR_PANEL);
    g_brGreen = CreateSolidBrush(CLR_GREEN);g_brRed = CreateSolidBrush(CLR_RED);
    g_brBtn = CreateSolidBrush(CLR_BTN_BG);

    auto regClass = [&](const wchar_t* cls, WNDPROC proc) {
        WNDCLASSEXW w = { sizeof(w) };w.lpfnWndProc = proc;w.hInstance = hInst;w.lpszClassName = cls;
        w.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);w.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        w.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));w.hIconSm = LoadIconW(hInst, MAKEINTRESOURCEW(1));
        RegisterClassExW(&w);};
    regClass(L"KryptiKSplash", SplashWndProc);
    regClass(L"KryptiKKeyWnd", KeyWndProc);
    regClass(L"KryptiKLoaderGUI", MainWndProc);

    // Window sizes
    const int SW = 480, SH = 330;   // main window
    const int KW = 420, KH = 240;   // key window
    const int SPW = 380, SPH = 110; // splash

    // Center on screen helper
    int scrW = GetSystemMetrics(SM_CXSCREEN);
    int scrH = GetSystemMetrics(SM_CYSCREEN);
    auto cx = [&](int w) {return(scrW - w) / 2;};
    auto cy = [&](int h) {return(scrH - h) / 2;};

    g_hSplashWnd = CreateWindowExW(WS_EX_APPWINDOW, L"KryptiKSplash", L"KryptiK Loader",
        WS_POPUP, cx(SPW), cy(SPH), SPW, SPH, nullptr, nullptr, hInst, nullptr);
    g_hKeyWnd = CreateWindowExW(WS_EX_APPWINDOW, L"KryptiKKeyWnd", L"KryptiK Loader",
        WS_POPUP, cx(KW), cy(KH), KW, KH, nullptr, nullptr, hInst, nullptr);
    g_hWnd = CreateWindowExW(WS_EX_APPWINDOW, L"KryptiKLoaderGUI", L"KryptiK Loader",
        WS_POPUP, cx(SW), cy(SH), SW, SH, nullptr, nullptr, hInst, nullptr);

    // DWM rounded corners (Windows 11+)
    auto applyRound = [](HWND h) {
        DWORD pref = 2; // DWMWCP_ROUND
        DwmSetWindowAttribute(h, 33, &pref, sizeof(pref)); // DWMWA_WINDOW_CORNER_PREFERENCE=33
        };
    applyRound(g_hSplashWnd);applyRound(g_hKeyWnd);applyRound(g_hWnd);

    ShowWindow(g_hSplashWnd, SW_SHOW);UpdateWindow(g_hSplashWnd);
    std::thread(DoCheckLicense).detach();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg);DispatchMessageW(&msg); }
    DeleteObject(g_fontTitle);DeleteObject(g_fontMono);DeleteObject(g_fontSmall);
    DeleteObject(g_brBg);DeleteObject(g_brCard);DeleteObject(g_brPanel);
    DeleteObject(g_brGreen);DeleteObject(g_brRed);DeleteObject(g_brBtn);
    return(int)msg.wParam;
}
