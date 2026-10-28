#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>
#include <atomic>
#include <mutex>
#include "resource.h"

static bool IsRunningAsAdmin() {
    BOOL elevated = FALSE;
    HANDLE token = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION te{};
        DWORD size = sizeof(te);
        if (GetTokenInformation(token, TokenElevation, &te, sizeof(te), &size))
            elevated = te.TokenIsElevated;
        CloseHandle(token);
    }
    return elevated != FALSE;
}

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "comctl32.lib")

#define APP_NAME      L"OptiCore Cleaner"
#define APP_SUBTITLE  L"Simple 1-click cleaner by Ladro"

#define IDC_CLEAN_BTN    101
#define IDC_LOG_EDIT     102
#define IDC_STATUS_LBL   103
#define IDC_ICON_IMG     104
#define IDC_SUBTITLE_LBL 105
#define IDC_ADMIN_BTN    106
#define IDC_MODE_QUICK   110
#define IDC_MODE_COMPLETE 111
#define IDC_MODE_CUSTOM  112
#define IDC_LIST         120
#define IDC_SEL_ALL      121
#define IDC_SEL_NONE     122

#define IDM_FILE_EXIT    202
#define IDM_EDIT_CLEAR   203
#define IDM_HELP_ABOUT   204
#define IDM_QUICK_SCAN   205
#define IDM_COMPLETE     206
#define IDM_CUSTOM       207

#define WM_LOG_MESSAGE  (WM_USER + 1)
#define WM_CLEAN_DONE   (WM_USER + 2)

#define BG_COLOR  RGB(245, 246, 250)
#define ACCENT    RGB(50, 120, 220)

// ---------------------------------------------------------------------------
// What can be cleaned
// ---------------------------------------------------------------------------

struct PathRule {
    const wchar_t* path;            // may use %ENV% variables and one * folder wildcard
    const wchar_t* pattern = L"*";  // which files to remove
    bool keepFolders = false;       // remove matching files only, leave subfolders in place
};

enum Group { GROUP_SYSTEM = 1, GROUP_BROWSER, GROUP_GAMING, GROUP_PRIVACY };

struct Category {
    const wchar_t* name;
    const wchar_t* description;
    Group group;
    std::vector<PathRule> paths;
    bool needsAdmin = false;
    const wchar_t* service = nullptr;  // stopped while cleaning, then restarted
    bool recycleBin = false;
};

// Quick Scan and Complete Cleanup cover GROUP_SYSTEM. Custom Cleanup can pick anything.
static const Category g_categories[] = {
    { L"Temporary files", L"Your Temp folder", GROUP_SYSTEM,
        { { L"%TEMP%" } } },
    { L"Windows temporary files", L"Windows\\Temp (admin)", GROUP_SYSTEM,
        { { L"%WINDIR%\\Temp" } }, true },
    { L"Prefetch", L"App launch traces, rebuilt by Windows (admin)", GROUP_SYSTEM,
        { { L"%WINDIR%\\Prefetch" } }, true },
    { L"Windows Update cache", L"Downloaded updates (admin)", GROUP_SYSTEM,
        { { L"%WINDIR%\\SoftwareDistribution\\Download" } }, true, L"wuauserv" },
    { L"Delivery Optimization files", L"Update files shared between PCs (admin)", GROUP_SYSTEM,
        { { L"%WINDIR%\\ServiceProfiles\\NetworkService\\AppData\\Local\\Microsoft\\Windows\\DeliveryOptimization\\Cache" } }, true },
    { L"Error reports", L"Windows Error Reporting files", GROUP_SYSTEM,
        { { L"%PROGRAMDATA%\\Microsoft\\Windows\\WER\\ReportArchive" },
          { L"%PROGRAMDATA%\\Microsoft\\Windows\\WER\\ReportQueue" },
          { L"%LOCALAPPDATA%\\Microsoft\\Windows\\WER\\ReportArchive" },
          { L"%LOCALAPPDATA%\\Microsoft\\Windows\\WER\\ReportQueue" } } },
    { L"Crash dumps", L"Memory dumps left by crashes", GROUP_SYSTEM,
        { { L"%WINDIR%\\Minidump" },
          { L"%WINDIR%\\MEMORY.DMP" },
          { L"%LOCALAPPDATA%\\CrashDumps" } } },
    { L"Windows log files", L"Old .log files in Windows\\Logs (admin)", GROUP_SYSTEM,
        { { L"%WINDIR%\\Logs", L"*.log", true } }, true },
    { L"Thumbnail cache", L"Explorer thumbnails, rebuilt as needed", GROUP_SYSTEM,
        { { L"%LOCALAPPDATA%\\Microsoft\\Windows\\Explorer", L"thumbcache_*.db", true } } },
    { L"Store app temp files", L"Temp folders of Microsoft Store apps", GROUP_SYSTEM,
        { { L"%LOCALAPPDATA%\\Packages\\*\\AC\\Temp" } } },
    { L"Recycle Bin", L"Deleted files on all drives", GROUP_SYSTEM,
        {}, false, nullptr, true },

    { L"Google Chrome cache", L"Cached web pages and images", GROUP_BROWSER,
        { { L"%LOCALAPPDATA%\\Google\\Chrome\\User Data\\*\\Cache", L"*", true },
          { L"%LOCALAPPDATA%\\Google\\Chrome\\User Data\\*\\Code Cache", L"*", true },
          { L"%LOCALAPPDATA%\\Google\\Chrome\\User Data\\*\\GPUCache", L"*", true } } },
    { L"Microsoft Edge cache", L"Cached web pages and images", GROUP_BROWSER,
        { { L"%LOCALAPPDATA%\\Microsoft\\Edge\\User Data\\*\\Cache", L"*", true },
          { L"%LOCALAPPDATA%\\Microsoft\\Edge\\User Data\\*\\Code Cache", L"*", true },
          { L"%LOCALAPPDATA%\\Microsoft\\Edge\\User Data\\*\\GPUCache", L"*", true } } },
    { L"Brave cache", L"Cached web pages and images", GROUP_BROWSER,
        { { L"%LOCALAPPDATA%\\BraveSoftware\\Brave-Browser\\User Data\\*\\Cache", L"*", true },
          { L"%LOCALAPPDATA%\\BraveSoftware\\Brave-Browser\\User Data\\*\\Code Cache", L"*", true },
          { L"%LOCALAPPDATA%\\BraveSoftware\\Brave-Browser\\User Data\\*\\GPUCache", L"*", true } } },
    { L"Vivaldi cache", L"Cached web pages and images", GROUP_BROWSER,
        { { L"%LOCALAPPDATA%\\Vivaldi\\User Data\\*\\Cache", L"*", true },
          { L"%LOCALAPPDATA%\\Vivaldi\\User Data\\*\\Code Cache", L"*", true },
          { L"%LOCALAPPDATA%\\Vivaldi\\User Data\\*\\GPUCache", L"*", true } } },
    { L"Opera / Opera GX cache", L"Cached web pages and images", GROUP_BROWSER,
        { { L"%LOCALAPPDATA%\\Opera Software\\*\\Cache", L"*", true } } },
    { L"Firefox cache", L"Cached web pages and images", GROUP_BROWSER,
        { { L"%LOCALAPPDATA%\\Mozilla\\Firefox\\Profiles\\*\\cache2", L"*", true } } },
    { L"Temporary Internet Files", L"Legacy web cache used by Windows apps", GROUP_BROWSER,
        { { L"%LOCALAPPDATA%\\Microsoft\\Windows\\INetCache", L"*", true } } },
    { L"Discord cache", L"Cached images and media", GROUP_BROWSER,
        { { L"%APPDATA%\\discord\\Cache", L"*", true },
          { L"%APPDATA%\\discord\\Code Cache", L"*", true },
          { L"%APPDATA%\\discord\\GPUCache", L"*", true } } },

    // Games stutter while these are rebuilt, so they're never cleaned unless picked
    { L"DirectX shader cache", L"Games rebuild it, first launches stutter", GROUP_GAMING,
        { { L"%LOCALAPPDATA%\\D3DSCache" } } },
    { L"NVIDIA shader cache", L"Games rebuild it, first launches stutter", GROUP_GAMING,
        { { L"%LOCALAPPDATA%\\NVIDIA\\DXCache" },
          { L"%LOCALAPPDATA%\\NVIDIA\\GLCache" } } },
    { L"AMD shader cache", L"Games rebuild it, first launches stutter", GROUP_GAMING,
        { { L"%LOCALAPPDATA%\\AMD\\DxCache" },
          { L"%LOCALAPPDATA%\\AMD\\DxcCache" },
          { L"%LOCALAPPDATA%\\AMD\\GLCache" },
          { L"%LOCALAPPDATA%\\AMD\\VkCache" } } },

    { L"Recent files list", L"Shortcuts to recently opened files", GROUP_PRIVACY,
        { { L"%APPDATA%\\Microsoft\\Windows\\Recent", L"*.lnk", true } } },
};

static const int CATEGORY_COUNT = (int)ARRAYSIZE(g_categories);

enum Mode { MODE_QUICK, MODE_COMPLETE, MODE_CUSTOM };

static const wchar_t* const g_modeAction[] = { L"Scan", L"Clean Now", L"Clean Selected" };
static const wchar_t* const g_modeHint[] = {
    L"Finds how much space can be freed. Nothing is deleted.",
    L"Removes all system junk. Browser and game caches are left alone.",
    L"Tick what you want cleaned, then click Clean Selected.",
};

struct RunRequest {
    bool scanOnly;
    std::vector<int> categories;
};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static HINSTANCE g_hInst  = NULL;
static HWND g_hWnd        = NULL;
static HWND g_hLog        = NULL;
static HWND g_hList       = NULL;
static HWND g_hSelAll     = NULL;
static HWND g_hSelNone    = NULL;
static HWND g_hModeBtn[3] = {};
static HWND g_hBtn        = NULL;
static HWND g_hAdminBtn   = NULL;
static HWND g_hStatus     = NULL;
static HWND g_hIconCtrl   = NULL;
static HWND g_hTitleLbl   = NULL;
static HWND g_hSubtitle   = NULL;
static bool g_isAdmin     = false;
static UINT g_dpi         = 96;
static Mode g_mode        = MODE_QUICK;
static std::atomic<bool> g_cleaning(false);
static std::atomic<bool> g_cancel(false);
static bool g_scanOnly        = false;  // set before the worker starts, read-only while it runs
static bool g_closeAfterClean = false;
static LONG g_files   = 0;  // files deleted (or found, when scanning)
static LONG g_skipped = 0;
static LONG64 g_bytes = 0;  // bytes freed (or found, when scanning)

static HBRUSH g_hBgBrush = NULL;
static HFONT  g_hFontTitle = NULL;
static HFONT  g_hFontNormal = NULL;
static HFONT  g_hFontBtn = NULL;

// Log lines are batched here by the worker thread and flushed by the UI thread,
// so a big clean can't overflow the window's message queue.
static std::mutex g_logMutex;
static std::wstring g_pendingLog;
static std::atomic<bool> g_logPosted(false);

// Scale a 96-DPI pixel value to the window's current DPI.
static int S(int px) {
    return MulDiv(px, g_dpi, 96);
}

static std::wstring FormatBytes(ULONGLONG bytes) {
    const wchar_t* units[] = { L"bytes", L"KB", L"MB", L"GB", L"TB" };
    double value = (double)bytes;
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    wchar_t buf[32];
    if (unit == 0)
        swprintf_s(buf, L"%llu bytes", bytes);
    else
        swprintf_s(buf, L"%.1f %ls", value, units[unit]);
    return buf;
}

void AppendLog(const std::wstring& text) {
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        g_pendingLog += text;
        g_pendingLog += L"\r\n";
    }
    if (!g_logPosted.exchange(true))
        PostMessage(g_hWnd, WM_LOG_MESSAGE, 0, 0);
}

// ---------------------------------------------------------------------------
// Cleaning (runs on the worker thread)
// ---------------------------------------------------------------------------

// Deletes (or, when scanning, counts) one file.
static void ProcessFile(const std::wstring& path, DWORD attrs, ULONGLONG size) {
    if (g_scanOnly) {
        InterlockedIncrement(&g_files);
        InterlockedAdd64(&g_bytes, (LONG64)size);
        return;
    }
    if (attrs & FILE_ATTRIBUTE_READONLY)
        SetFileAttributes(path.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);

    if (DeleteFile(path.c_str())) {
        InterlockedIncrement(&g_files);
        InterlockedAdd64(&g_bytes, (LONG64)size);
        AppendLog(L"  Deleted: " + path);
    } else {
        InterlockedIncrement(&g_skipped);
        AppendLog(L"  Skipped: " + path);
    }
}

// Deletes everything inside `path` that matches `pattern` but leaves `path` itself in place.
void ClearDirectory(const std::wstring& path, const wchar_t* pattern = L"*", bool keepFolders = false) {
    WIN32_FIND_DATA fd;
    HANDLE hFind = FindFirstFile((path + L"\\*").c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (g_cancel) break;

        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;

        std::wstring full = path + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            // Never follow junctions/symlinks: their targets live outside this folder.
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                if (!g_scanOnly) {
                    InterlockedIncrement(&g_skipped);
                    AppendLog(L"  Skipped (link): " + full);
                }
                continue;
            }
            ClearDirectory(full, pattern, keepFolders);
            if (!keepFolders && !g_scanOnly) {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
                    SetFileAttributes(full.c_str(), fd.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
                RemoveDirectory(full.c_str());
            }
        } else if (PathMatchSpec(name.c_str(), pattern)) {
            ProcessFile(full, fd.dwFileAttributes,
                ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
        }
    } while (FindNextFile(hFind, &fd));

    FindClose(hFind);
}

// Expands %ENV% variables and one * folder wildcard (e.g. "...\User Data\*\Cache")
// into every matching path.
static std::vector<std::wstring> ExpandPath(const wchar_t* spec) {
    std::vector<std::wstring> result;
    wchar_t buf[MAX_PATH * 2];
    DWORD len = ExpandEnvironmentStrings(spec, buf, ARRAYSIZE(buf));
    if (len == 0 || len > ARRAYSIZE(buf)) return result;

    std::wstring path = buf;
    size_t star = path.find(L'*');
    if (star == std::wstring::npos) {
        result.push_back(path);
        return result;
    }

    size_t segStart = path.rfind(L'\\', star);
    size_t segEnd   = path.find(L'\\', star);
    if (segStart == std::wstring::npos) return result;
    std::wstring parent = path.substr(0, segStart);
    std::wstring rest   = segEnd == std::wstring::npos ? L"" : path.substr(segEnd);

    WIN32_FIND_DATA fd;
    HANDLE hFind = FindFirstFile(path.substr(0, segEnd).c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return result;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        result.push_back(parent + L"\\" + name + rest);
    } while (FindNextFile(hFind, &fd));
    FindClose(hFind);
    return result;
}

// Cleans one folder (its contents) or one file.
void CleanLocation(const std::wstring& path, const wchar_t* pattern = L"*", bool keepFolders = false) {
    if (g_cancel) return;

    // Only absolute paths, and never a drive root.
    if (path.length() <= 3 || path[1] != L':') return;

    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesEx(path.c_str(), GetFileExInfoStandard, &info)) return;

    if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        ClearDirectory(path, pattern, keepFolders);
    else
        ProcessFile(path, info.dwFileAttributes,
            ((ULONGLONG)info.nFileSizeHigh << 32) | info.nFileSizeLow);
}

static void ProcessRecycleBin() {
    SHQUERYRBINFO info = {};
    info.cbSize = sizeof(info);
    if (FAILED(SHQueryRecycleBin(NULL, &info)) || info.i64NumItems == 0) return;

    if (!g_scanOnly) {
        HRESULT hr = SHEmptyRecycleBin(NULL, NULL,
            SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND);
        if (FAILED(hr)) {
            AppendLog(L"  Could not empty the Recycle Bin (access denied).");
            return;
        }
    }
    InterlockedAdd(&g_files, (LONG)info.i64NumItems);
    InterlockedAdd64(&g_bytes, info.i64Size);
}

enum class StopResult { NotRunning, Stopped, Busy };

// Stops a service so its cache can be cleared. Without admin rights the service
// can't be opened, which is reported as NotRunning (its files are skipped anyway).
static StopResult StopServiceForCleaning(const wchar_t* name) {
    StopResult result = StopResult::NotRunning;
    SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) return result;

    SC_HANDLE svc = OpenService(scm, name, SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (svc) {
        SERVICE_STATUS st = {};
        if (QueryServiceStatus(svc, &st) && st.dwCurrentState != SERVICE_STOPPED) {
            result = StopResult::Busy;
            ControlService(svc, SERVICE_CONTROL_STOP, &st);
            for (int i = 0; i < 60 && st.dwCurrentState != SERVICE_STOPPED; ++i) {
                Sleep(250);
                if (!QueryServiceStatus(svc, &st)) break;
            }
            if (st.dwCurrentState == SERVICE_STOPPED)
                result = StopResult::Stopped;
        }
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return result;
}

static void StartServiceByName(const wchar_t* name) {
    SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) return;
    SC_HANDLE svc = OpenService(scm, name, SERVICE_START);
    if (svc) {
        StartService(svc, 0, NULL);
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
}

static void RunCategory(const Category& cat) {
    LONG   files0 = g_files;
    LONG64 bytes0 = g_bytes;

    if (!g_scanOnly) {
        AppendLog(L"");
        std::wstring header = std::wstring(L"Cleaning ") + cat.name;
        if (cat.needsAdmin && !g_isAdmin)
            header += L" (needs administrator for a full clean)";
        AppendLog(header + L"...");
    }

    if (cat.recycleBin) {
        ProcessRecycleBin();
    } else {
        bool restartService = false;
        if (cat.service && !g_scanOnly) {
            StopResult stop = StopServiceForCleaning(cat.service);
            if (stop == StopResult::Busy) {
                AppendLog(L"  Windows Update is busy right now, skipping.");
                return;
            }
            restartService = stop == StopResult::Stopped;
        }
        for (const PathRule& rule : cat.paths)
            for (const std::wstring& path : ExpandPath(rule.path))
                CleanLocation(path, rule.pattern, rule.keepFolders);
        if (restartService)
            StartServiceByName(cat.service);
    }

    wchar_t line[256];
    swprintf_s(line, g_scanOnly ? L"  %ls: %ls (%ld files)" : L"  %ls: freed %ls (%ld files)",
        cat.name, FormatBytes((ULONGLONG)(g_bytes - bytes0)).c_str(), g_files - files0);
    AppendLog(line);
}

DWORD WINAPI RunThread(LPVOID param) {
    std::unique_ptr<RunRequest> req((RunRequest*)param);
    g_files   = 0;
    g_skipped = 0;
    g_bytes   = 0;

    for (int index : req->categories) {
        if (g_cancel) break;
        RunCategory(g_categories[index]);
    }

    PostMessage(g_hWnd, WM_CLEAN_DONE, 0, 0);
    return 0;
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

static HFONT MakeFont(int height, int weight) {
    return CreateFont(S(height), 0, 0, 0, weight, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

// (Re)creates fonts and the title icon at the current DPI.
static void UpdateDpiResources() {
    HFONT oldTitle = g_hFontTitle, oldNormal = g_hFontNormal, oldBtn = g_hFontBtn;
    g_hFontTitle  = MakeFont(26, FW_BOLD);
    g_hFontNormal = MakeFont(14, FW_NORMAL);
    g_hFontBtn    = MakeFont(14, FW_SEMIBOLD);

    SendMessage(g_hTitleLbl, WM_SETFONT, (WPARAM)g_hFontTitle,  TRUE);
    SendMessage(g_hSubtitle, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
    SendMessage(g_hLog,      WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
    SendMessage(g_hList,     WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
    SendMessage(g_hSelAll,   WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
    SendMessage(g_hSelNone,  WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
    SendMessage(g_hStatus,   WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
    SendMessage(g_hBtn,      WM_SETFONT, (WPARAM)g_hFontBtn,    TRUE);
    for (HWND hMode : g_hModeBtn)
        SendMessage(hMode, WM_SETFONT, (WPARAM)g_hFontBtn, TRUE);
    if (g_hAdminBtn)
        SendMessage(g_hAdminBtn, WM_SETFONT, (WPARAM)g_hFontBtn, TRUE);

    if (oldTitle)  DeleteObject(oldTitle);
    if (oldNormal) DeleteObject(oldNormal);
    if (oldBtn)    DeleteObject(oldBtn);

    HICON hIco = (HICON)LoadImage(g_hInst, MAKEINTRESOURCE(IDI_ICON1),
        IMAGE_ICON, S(32), S(32), LR_DEFAULTCOLOR);
    HICON hOldIco = (HICON)SendMessage(g_hIconCtrl, STM_SETICON, (WPARAM)hIco, 0);
    if (hOldIco) DestroyIcon(hOldIco);
}

static SIZE MeasureText(HWND hCtrl, HFONT hFont, const wchar_t* text) {
    SIZE sz = {};
    HDC hdc = GetDC(hCtrl);
    HFONT hOld = (HFONT)SelectObject(hdc, hFont);
    GetTextExtentPoint32(hdc, text, (int)wcslen(text), &sz);
    SelectObject(hdc, hOld);
    ReleaseDC(hCtrl, hdc);
    return sz;
}

static void FillCategoryList() {
    const struct { Group id; const wchar_t* header; } groups[] = {
        { GROUP_SYSTEM,  L"System junk (Complete Cleanup)" },
        { GROUP_BROWSER, L"Browsers & apps" },
        { GROUP_GAMING,  L"Gaming" },
        { GROUP_PRIVACY, L"Privacy" },
    };
    ListView_EnableGroupView(g_hList, TRUE);
    for (const auto& g : groups) {
        LVGROUP group = {};
        group.cbSize    = sizeof(group);
        group.mask      = LVGF_HEADER | LVGF_GROUPID;
        group.pszHeader = (LPWSTR)g.header;
        group.iGroupId  = g.id;
        ListView_InsertGroup(g_hList, -1, &group);
    }

    LVCOLUMN col = {};
    col.mask = LVCF_WIDTH;
    ListView_InsertColumn(g_hList, 0, &col);
    ListView_InsertColumn(g_hList, 1, &col);

    for (int i = 0; i < CATEGORY_COUNT; ++i) {
        LVITEM item = {};
        item.mask     = LVIF_TEXT | LVIF_PARAM | LVIF_GROUPID;
        item.iItem    = i;
        item.pszText  = (LPWSTR)g_categories[i].name;
        item.lParam   = i;
        item.iGroupId = g_categories[i].group;
        int row = ListView_InsertItem(g_hList, &item);
        ListView_SetItemText(g_hList, row, 1, (LPWSTR)g_categories[i].description);
        ListView_SetCheckState(g_hList, row, g_categories[i].group == GROUP_SYSTEM);
    }
}

static void SetAllChecks(BOOL checked) {
    int count = ListView_GetItemCount(g_hList);
    for (int row = 0; row < count; ++row)
        ListView_SetCheckState(g_hList, row, checked);
}

// Shows either the Custom Cleanup checklist or the log in the content area.
static void ShowChecklist(bool show) {
    ShowWindow(g_hList,    show ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hSelAll,  show ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hSelNone, show ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hLog,     show ? SW_HIDE : SW_SHOW);
}

static void SetMode(Mode mode) {
    if (g_cleaning) return;
    g_mode = mode;
    for (int i = 0; i < 3; ++i)
        SendMessage(g_hModeBtn[i], BM_SETCHECK, i == mode ? BST_CHECKED : BST_UNCHECKED, 0);
    SetWindowText(g_hBtn, g_modeAction[mode]);
    SetWindowText(g_hStatus, g_modeHint[mode]);
    ShowChecklist(mode == MODE_CUSTOM);
}

static void SetRunningUi(bool running) {
    for (HWND hMode : g_hModeBtn)
        EnableWindow(hMode, !running);
    if (g_hAdminBtn) EnableWindow(g_hAdminBtn, !running);
    EnableWindow(g_hBtn, TRUE);
    SetWindowText(g_hBtn, running ? L"Stop" : g_modeAction[g_mode]);
}

static void StartRun() {
    if (g_cleaning) return;

    auto req = std::make_unique<RunRequest>();
    req->scanOnly = g_mode == MODE_QUICK;
    if (g_mode == MODE_CUSTOM) {
        int count = ListView_GetItemCount(g_hList);
        for (int row = 0; row < count; ++row) {
            if (!ListView_GetCheckState(g_hList, row)) continue;
            LVITEM item = {};
            item.mask  = LVIF_PARAM;
            item.iItem = row;
            ListView_GetItem(g_hList, &item);
            req->categories.push_back((int)item.lParam);
        }
        std::sort(req->categories.begin(), req->categories.end());
    } else {
        for (int i = 0; i < CATEGORY_COUNT; ++i)
            if (g_categories[i].group == GROUP_SYSTEM)
                req->categories.push_back(i);
    }
    if (req->categories.empty()) {
        SetWindowText(g_hStatus, L"Tick at least one item to clean.");
        return;
    }

    g_scanOnly = req->scanOnly;
    g_cleaning = true;
    g_cancel   = false;
    SetRunningUi(true);
    ShowChecklist(false);
    SetWindowText(g_hLog, L"");
    SetWindowText(g_hStatus, g_scanOnly ? L"Scanning..." : L"Cleaning...");

    if (g_scanOnly) {
        AppendLog(L"Scanning... nothing will be deleted.");
        AppendLog(L"Checking everything Complete Cleanup would remove:");
        AppendLog(L"");
    } else {
        AppendLog(L"Cleaning... please wait.");
    }
    if (!g_isAdmin)
        AppendLog(L"Note: not running as administrator, so system folders "
                  L"marked (admin) will be mostly skipped.");

    HANDLE hThread = CreateThread(NULL, 0, RunThread, req.get(), 0, NULL);
    if (hThread) {
        req.release();  // the worker owns it now
        CloseHandle(hThread);
    } else {
        g_cleaning = false;
        SetRunningUi(false);
        AppendLog(L"Failed to start.");
        SetWindowText(g_hStatus, L"Failed to start.");
    }
}

static void FinishRun() {
    g_cleaning = false;
    SetRunningUi(false);

    std::wstring total = FormatBytes((ULONGLONG)g_bytes);
    wchar_t line[256];
    AppendLog(L"");
    AppendLog(L"============================");
    if (g_scanOnly) {
        AppendLog(g_cancel ? L"  Scan stopped." : L"  Scan complete!");
        swprintf_s(line, L"  Can be freed  : %ls", total.c_str());
        AppendLog(line);
        swprintf_s(line, L"  Files found   : %ld", g_files);
        AppendLog(line);
        AppendLog(L"============================");
        AppendLog(L"Run Complete Cleanup to remove it.");

        swprintf_s(line, L"Found %ls you can free. Run Complete Cleanup to remove it.", total.c_str());
    } else {
        AppendLog(g_cancel ? L"  Cleaning stopped." : L"  Cleaning complete!");
        swprintf_s(line, L"  Space freed   : %ls", total.c_str());
        AppendLog(line);
        swprintf_s(line, L"  Files deleted : %ld", g_files);
        AppendLog(line);
        swprintf_s(line, L"  Skipped       : %ld", g_skipped);
        AppendLog(line);
        AppendLog(L"============================");
        if (g_mode == MODE_CUSTOM)
            AppendLog(L"Click Custom Cleanup to go back to your selection.");

        swprintf_s(line, L"Freed %ls: %ld files deleted, %ld skipped (in use or protected).",
            total.c_str(), g_files, g_skipped);
    }
    SetWindowText(g_hStatus, line);
}

static void RestartAsAdmin(HWND hWnd) {
    wchar_t exePath[MAX_PATH];
    if (!GetModuleFileName(NULL, exePath, MAX_PATH)) return;

    SHELLEXECUTEINFO sei = {};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = exePath;
    sei.hwnd   = hWnd;
    sei.nShow  = SW_SHOWNORMAL;
    if (ShellExecuteEx(&sei))
        DestroyWindow(hWnd);
    else if (GetLastError() != ERROR_CANCELLED)
        MessageBox(hWnd, L"Could not restart as administrator.",
            APP_NAME, MB_OK | MB_ICONERROR);
}

static HRESULT CALLBACK AboutCallback(HWND hWnd, UINT msg, WPARAM, LPARAM lParam, LONG_PTR) {
    if (msg == TDN_HYPERLINK_CLICKED)
        ShellExecute(hWnd, L"open", (LPCWSTR)lParam, NULL, NULL, SW_SHOWNORMAL);
    return S_OK;
}

static void ShowAbout(HWND hWnd) {
    TASKDIALOGCONFIG tdc = {};
    tdc.cbSize             = sizeof(tdc);
    tdc.hwndParent         = hWnd;
    tdc.hInstance          = g_hInst;
    tdc.dwFlags            = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION;
    tdc.dwCommonButtons    = TDCBF_OK_BUTTON;
    tdc.pszWindowTitle     = L"About " APP_NAME;
    tdc.pszMainIcon        = MAKEINTRESOURCE(IDI_ICON1);
    tdc.pszMainInstruction = APP_NAME;
    tdc.pszContent         =
        L"by Ladro\n\n"
        L"Scans and cleans temp files, caches, logs,\n"
        L"crash dumps, and the Recycle Bin.\n\n"
        L"GitHub: <a href=\"https://github.com/ladroxd\">github.com/ladroxd</a>";
    tdc.pfCallback         = AboutCallback;
    TaskDialogIndirect(&tdc, NULL, NULL, NULL);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_hWnd = hWnd;
        g_dpi  = GetDpiForWindow(hWnd);

        HMENU hMenuBar  = CreateMenu();
        HMENU hFile     = CreatePopupMenu();
        HMENU hEdit     = CreatePopupMenu();
        HMENU hHelp     = CreatePopupMenu();

        AppendMenu(hFile, MF_STRING, IDM_QUICK_SCAN, L"&Quick Scan");
        AppendMenu(hFile, MF_STRING, IDM_COMPLETE,   L"&Complete Cleanup");
        AppendMenu(hFile, MF_STRING, IDM_CUSTOM,     L"C&ustom Cleanup...");
        AppendMenu(hFile, MF_SEPARATOR, 0, NULL);
        AppendMenu(hFile, MF_STRING, IDM_FILE_EXIT,  L"E&xit");

        AppendMenu(hEdit, MF_STRING, IDM_EDIT_CLEAR, L"&Clear Log");

        AppendMenu(hHelp, MF_STRING, IDM_HELP_ABOUT, L"&About");

        AppendMenu(hMenuBar, MF_POPUP, (UINT_PTR)hFile, L"&File");
        AppendMenu(hMenuBar, MF_POPUP, (UINT_PTR)hEdit, L"&Edit");
        AppendMenu(hMenuBar, MF_POPUP, (UINT_PTR)hHelp, L"&Help");

        SetMenu(hWnd, hMenuBar);

        // All controls are positioned in WM_SIZE
        g_hIconCtrl = CreateWindow(L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_ICON | SS_REALSIZEIMAGE,
            0, 0, 0, 0, hWnd, (HMENU)IDC_ICON_IMG, NULL, NULL);

        g_hTitleLbl = CreateWindow(L"STATIC", APP_NAME,
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
            0, 0, 0, 0, hWnd, NULL, NULL, NULL);

        g_hSubtitle = CreateWindow(L"STATIC", APP_SUBTITLE,
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
            0, 0, 0, 0, hWnd, (HMENU)IDC_SUBTITLE_LBL, NULL, NULL);

        const wchar_t* modeLabels[] = { L"Quick Scan", L"Complete Cleanup", L"Custom Cleanup" };
        for (int i = 0; i < 3; ++i) {
            g_hModeBtn[i] = CreateWindow(L"BUTTON", modeLabels[i],
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON | BS_PUSHLIKE |
                (i == 0 ? WS_GROUP : 0),
                0, 0, 0, 0, hWnd, (HMENU)(INT_PTR)(IDC_MODE_QUICK + i), NULL, NULL);
        }

        g_hLog = CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL |
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            0, 0, 0, 0, hWnd, (HMENU)IDC_LOG_EDIT, NULL, NULL);
        // Lift the default ~32K character cap so long cleans keep logging.
        SendMessage(g_hLog, EM_SETLIMITTEXT, 0, 0);

        g_hList = CreateWindowEx(WS_EX_CLIENTEDGE, WC_LISTVIEW, L"",
            WS_CHILD | WS_TABSTOP | LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            0, 0, 0, 0, hWnd, (HMENU)IDC_LIST, NULL, NULL);
        ListView_SetExtendedListViewStyle(g_hList,
            LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        FillCategoryList();

        g_hSelAll = CreateWindow(L"BUTTON", L"Select all",
            WS_CHILD | BS_PUSHBUTTON | WS_TABSTOP,
            0, 0, 0, 0, hWnd, (HMENU)IDC_SEL_ALL, NULL, NULL);
        g_hSelNone = CreateWindow(L"BUTTON", L"Select none",
            WS_CHILD | BS_PUSHBUTTON | WS_TABSTOP,
            0, 0, 0, 0, hWnd, (HMENU)IDC_SEL_NONE, NULL, NULL);

        g_hStatus = CreateWindow(L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_CENTER | SS_NOPREFIX | SS_ENDELLIPSIS,
            0, 0, 0, 0, hWnd, (HMENU)IDC_STATUS_LBL, NULL, NULL);

        g_hBtn = CreateWindow(L"BUTTON", L"",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
            0, 0, 0, 0, hWnd, (HMENU)IDC_CLEAN_BTN, NULL, NULL);

        if (!g_isAdmin) {
            g_hAdminBtn = CreateWindow(L"BUTTON", L"Restart as Administrator",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                0, 0, 0, 0, hWnd, (HMENU)IDC_ADMIN_BTN, NULL, NULL);
            SendMessage(g_hAdminBtn, BCM_SETSHIELD, 0, TRUE);
        }

        UpdateDpiResources();
        SetMode(MODE_QUICK);
        break;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_MODE_QUICK:
        case IDC_MODE_COMPLETE:
        case IDC_MODE_CUSTOM:
            SetMode((Mode)(LOWORD(wParam) - IDC_MODE_QUICK));
            break;
        case IDC_CLEAN_BTN:
            if (g_cleaning) {
                g_cancel = true;
                EnableWindow(g_hBtn, FALSE);
                SetWindowText(g_hStatus, L"Stopping...");
            } else {
                StartRun();
            }
            break;
        case IDC_SEL_ALL:
            SetAllChecks(TRUE);
            break;
        case IDC_SEL_NONE:
            SetAllChecks(FALSE);
            break;
        case IDM_QUICK_SCAN:
            SetMode(MODE_QUICK);
            StartRun();
            break;
        case IDM_COMPLETE:
            SetMode(MODE_COMPLETE);
            StartRun();
            break;
        case IDM_CUSTOM:
            SetMode(MODE_CUSTOM);
            break;
        case IDC_ADMIN_BTN:
            if (!g_cleaning)
                RestartAsAdmin(hWnd);
            break;
        case IDM_FILE_EXIT:
            SendMessage(hWnd, WM_CLOSE, 0, 0);
            break;
        case IDM_EDIT_CLEAR:
            SetWindowText(g_hLog, L"");
            break;
        case IDM_HELP_ABOUT:
            ShowAbout(hWnd);
            break;
        }
        break;

    case WM_LOG_MESSAGE: {
        std::wstring text;
        {
            std::lock_guard<std::mutex> lock(g_logMutex);
            text.swap(g_pendingLog);
            g_logPosted = false;
        }
        if (text.empty()) break;

        int len = GetWindowTextLength(g_hLog);
        SendMessage(g_hLog, EM_SETSEL, len, len);
        SendMessage(g_hLog, EM_REPLACESEL, FALSE, (LPARAM)text.c_str());
        break;
    }

    case WM_CLEAN_DONE:
        if (g_closeAfterClean) {
            g_cleaning = false;
            DestroyWindow(hWnd);
            break;
        }
        FinishRun();
        break;

    case WM_SIZE: {
        int cw = LOWORD(lParam);
        int ch = HIWORD(lParam);
        const int margin = S(20);
        const int btnW = S(150), adminBtnW = S(190), btnH = S(36), btnGap = S(10);
        const int bottomPad = S(14);
        const int iconSz = S(32), gap = S(8);
        const int titleY = S(14), titleH = S(34);
        const int subY = S(54), subH = S(20);
        const int modeY = S(84), modeH = S(34), modeGap = S(8);
        const int statusH = S(20);
        const int selW = S(100), selH = S(28);

        // center the icon+gap+title group horizontally
        SIZE textSz = MeasureText(g_hTitleLbl, g_hFontTitle, APP_NAME);
        int groupW = iconSz + gap + textSz.cx;
        int startX = (cw - groupW) / 2;
        int iconY  = titleY + (titleH - iconSz) / 2;
        SetWindowPos(g_hIconCtrl,  NULL, startX, iconY, iconSz, iconSz, SWP_NOZORDER);
        SetWindowPos(g_hTitleLbl,  NULL, startX + iconSz + gap, titleY, textSz.cx + S(4), titleH, SWP_NOZORDER);

        // center subtitle
        SIZE subSz = MeasureText(g_hSubtitle, g_hFontNormal, APP_SUBTITLE);
        SetWindowPos(g_hSubtitle, NULL, (cw - subSz.cx) / 2, subY, subSz.cx + S(4), subH, SWP_NOZORDER);

        // three equal mode buttons across the full width
        int modeW = (cw - margin * 2 - modeGap * 2) / 3;
        for (int i = 0; i < 3; ++i)
            SetWindowPos(g_hModeBtn[i], NULL, margin + i * (modeW + modeGap), modeY, modeW, modeH, SWP_NOZORDER);

        // buttons pinned to bottom center, side by side when the admin button is shown
        int btnY = ch - bottomPad - btnH;
        int rowW = g_hAdminBtn ? btnW + btnGap + adminBtnW : btnW;
        int btnX = (cw - rowW) / 2;
        SetWindowPos(g_hBtn, NULL, btnX, btnY, btnW, btnH, SWP_NOZORDER);
        if (g_hAdminBtn)
            SetWindowPos(g_hAdminBtn, NULL, btnX + btnW + btnGap, btnY, adminBtnW, btnH, SWP_NOZORDER);

        // status line just above the buttons
        int statusY = btnY - S(6) - statusH;
        SetWindowPos(g_hStatus, NULL, margin, statusY, cw - margin * 2, statusH, SWP_NOZORDER);

        // content area (log or checklist) fills the space in between
        int contentY = modeY + modeH + S(10);
        int contentW = cw - margin * 2;
        int contentH = statusY - contentY - S(6);
        SetWindowPos(g_hLog, NULL, margin, contentY, contentW, contentH, SWP_NOZORDER);

        int listH = contentH - selH - S(6);
        SetWindowPos(g_hList, NULL, margin, contentY, contentW, listH, SWP_NOZORDER);
        SetWindowPos(g_hSelAll,  NULL, margin, contentY + listH + S(6), selW, selH, SWP_NOZORDER);
        SetWindowPos(g_hSelNone, NULL, margin + selW + S(8), contentY + listH + S(6), selW, selH, SWP_NOZORDER);

        int innerW = contentW - GetSystemMetricsForDpi(SM_CXVSCROLL, g_dpi) - S(6);
        ListView_SetColumnWidth(g_hList, 0, innerW * 45 / 100);
        ListView_SetColumnWidth(g_hList, 1, innerW - innerW * 45 / 100);
        break;
    }

    case WM_DPICHANGED: {
        g_dpi = HIWORD(wParam);
        UpdateDpiResources();
        RECT* rc = (RECT*)lParam;
        SetWindowPos(hWnd, NULL, rc->left, rc->top,
            rc->right - rc->left, rc->bottom - rc->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        SetBkColor(hdc, BG_COLOR);
        // Give the title a distinct color
        if ((HWND)lParam == g_hTitleLbl)
            SetTextColor(hdc, ACCENT);
        else
            SetTextColor(hdc, RGB(50, 50, 60));
        return (LRESULT)g_hBgBrush;
    }

    case WM_CTLCOLORBTN:
        // Fills the corners around themed buttons with our background
        return (LRESULT)g_hBgBrush;

    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hWnd, &rc);
        FillRect((HDC)wParam, &rc, g_hBgBrush);
        return 1;
    }

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = S(460);
        mmi->ptMinTrackSize.y = S(440);
        break;
    }

    case WM_CLOSE:
        if (g_cleaning) {
            // Stop the worker at the next file; WM_CLEAN_DONE destroys the window.
            g_cancel = true;
            g_closeAfterClean = true;
            ShowWindow(hWnd, SW_HIDE);
            return 0;
        }
        break;

    case WM_DESTROY:
        DestroyIcon((HICON)SendMessage(g_hIconCtrl, STM_GETICON, 0, 0));
        DeleteObject(g_hFontTitle);
        DeleteObject(g_hFontNormal);
        DeleteObject(g_hFontBtn);
        PostQuitMessage(0);
        break;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPSTR, _In_ int nCmdShow) {
    g_hInst   = hInstance;
    g_isAdmin = IsRunningAsAdmin();
    g_dpi     = GetDpiForSystem();

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);

    g_hBgBrush = CreateSolidBrush(BG_COLOR);

    WNDCLASSEX wc       = {};
    wc.cbSize           = sizeof(WNDCLASSEX);
    wc.lpfnWndProc      = WndProc;
    wc.hInstance        = hInstance;
    wc.hCursor          = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground    = g_hBgBrush;
    wc.lpszClassName    = L"TempBinCleanerV2";
    wc.hIcon            = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1));
    wc.hIconSm          = (HICON)LoadImage(hInstance, MAKEINTRESOURCE(IDI_ICON1), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    RegisterClassEx(&wc);

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    const int winW = S(580), winH = S(560);

    g_hWnd = CreateWindowEx(0, L"TempBinCleanerV2",
        g_isAdmin ? APP_NAME L" (Administrator)" : APP_NAME,
        WS_OVERLAPPEDWINDOW,
        (screenW - winW) / 2, (screenH - winH) / 2,
        winW, winH, NULL, NULL, hInstance, NULL);

    ShowWindow(g_hWnd, nCmdShow);
    UpdateWindow(g_hWnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    DeleteObject(g_hBgBrush);
    return (int)msg.wParam;
}
