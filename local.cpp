#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <winhttp.h>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <cwctype>
#include "resource.h"
#include "version.h"

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
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ole32.lib")

#define APP_NAME      L"OptiCore Cleaner"
#define APP_SUBTITLE  L"Simple 1-click cleaner by Ladro"
#define APP_VERSION   L"" APP_VERSION_STR
#define GITHUB_REPO   L"ladroxd/SmartCleaner"
#define REG_KEY       L"Software\\OptiCore Cleaner"
#define TASK_NAME     L"OptiCore Cleaner weekly cleanup"
#define RECENT_HOURS  24

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
#define IDC_EXCL_LIST    130
#define IDC_EXCL_ADD     131
#define IDC_EXCL_REMOVE  132

#define IDM_FILE_EXIT    202
#define IDM_EDIT_CLEAR   203
#define IDM_HELP_ABOUT   204
#define IDM_QUICK_SCAN   205
#define IDM_COMPLETE     206
#define IDM_CUSTOM       207
#define IDM_COPY_LOG     208
#define IDM_SAVE_LOG     209
#define IDM_KEEP_RECENT  210
#define IDM_EXCLUSIONS   211
#define IDM_SCHEDULE     212
#define IDM_THEME_SYSTEM 213
#define IDM_THEME_LIGHT  214
#define IDM_THEME_DARK   215
#define IDM_CHECK_UPDATES 216

#define WM_LOG_MESSAGE   (WM_USER + 1)
#define WM_JOB_DONE      (WM_USER + 2)
#define WM_MEASURED      (WM_USER + 3)
#define WM_UPDATE_RESULT (WM_USER + 4)

#define TIMER_PROGRESS   1

// ---------------------------------------------------------------------------
// What can be cleaned
// ---------------------------------------------------------------------------

struct PathRule {
    const wchar_t* path;            // may use %ENV% variables and one * folder wildcard
    const wchar_t* pattern = L"*";  // which files to remove
    bool keepFolders = false;       // remove matching files only, leave subfolders in place
};

enum Group { GROUP_SYSTEM = 1, GROUP_BROWSER, GROUP_GAMING, GROUP_PRIVACY, GROUP_ADVANCED };

enum : unsigned {
    F_ADMIN   = 1,  // system folder: mostly skipped without administrator rights
    F_RECENT  = 2,  // temp folder: files changed recently are kept when that option is on
    F_RECYCLE = 4,  // the Recycle Bin (no paths)
    F_DISM    = 8,  // Windows component store cleanup through DISM (no paths)
};

struct Category {
    const wchar_t* name;
    const wchar_t* description;
    Group group;
    std::vector<PathRule> paths;
    unsigned flags = 0;
    const wchar_t* service = nullptr;  // stopped while cleaning, then restarted
};

// Quick Scan and Complete Cleanup cover GROUP_SYSTEM. Custom Cleanup can pick anything.
static const Category g_categories[] = {
    { L"Temporary files", L"Your Temp folder", GROUP_SYSTEM,
        { { L"%TEMP%" } }, F_RECENT },
    { L"Windows temporary files", L"Windows\\Temp (admin)", GROUP_SYSTEM,
        { { L"%WINDIR%\\Temp" } }, F_ADMIN | F_RECENT },
    { L"All users' temp files", L"Temp folders of every account on this PC (admin)", GROUP_SYSTEM,
        { { L"%SystemDrive%\\Users\\*\\AppData\\Local\\Temp" } }, F_ADMIN | F_RECENT },
    { L"Prefetch", L"App launch traces, rebuilt by Windows (admin)", GROUP_SYSTEM,
        { { L"%WINDIR%\\Prefetch" } }, F_ADMIN },
    { L"Windows Update cache", L"Downloaded updates (admin)", GROUP_SYSTEM,
        { { L"%WINDIR%\\SoftwareDistribution\\Download" } }, F_ADMIN, L"wuauserv" },
    { L"Delivery Optimization files", L"Update files shared between PCs (admin)", GROUP_SYSTEM,
        { { L"%WINDIR%\\ServiceProfiles\\NetworkService\\AppData\\Local\\Microsoft\\Windows\\DeliveryOptimization\\Cache" } }, F_ADMIN },
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
        { { L"%WINDIR%\\Logs", L"*.log", true } }, F_ADMIN },
    { L"Thumbnail cache", L"Explorer thumbnails, rebuilt as needed", GROUP_SYSTEM,
        { { L"%LOCALAPPDATA%\\Microsoft\\Windows\\Explorer", L"thumbcache_*.db", true } } },
    { L"Store app temp files", L"Temp folders of Microsoft Store apps", GROUP_SYSTEM,
        { { L"%LOCALAPPDATA%\\Packages\\*\\AC\\Temp" } }, F_RECENT },
    { L"Recycle Bin", L"Deleted files on all drives", GROUP_SYSTEM,
        {}, F_RECYCLE },

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

    { L"Windows component cleanup", L"Old update files, via DISM (slow, admin)", GROUP_ADVANCED,
        {}, F_ADMIN | F_DISM },
};

static const int CATEGORY_COUNT = (int)ARRAYSIZE(g_categories);

enum Mode { MODE_QUICK, MODE_COMPLETE, MODE_CUSTOM };

static const wchar_t* const g_modeAction[] = { L"Scan", L"Clean Now", L"Clean Selected" };
static const wchar_t* const g_modeHint[] = {
    L"Finds how much space can be freed. Nothing is deleted.",
    L"Removes all system junk. Browser and game caches are left alone.",
    L"Tick what you want cleaned, then click Clean Selected.",
};

// One scan or clean. The UI thread creates it, a worker runs it, and the UI
// thread deletes it once the worker has posted WM_JOB_DONE.
struct Job {
    bool scanOnly   = false;
    bool quiet      = false;   // measuring sizes for the checklist: nothing is logged
    bool keepRecent = false;
    FILETIME cutoff = {};      // with keepRecent, temp files changed after this are kept
    std::vector<std::wstring> exclusions;
    std::vector<int> categories;
    std::vector<LONG64> expected = std::vector<LONG64>(CATEGORY_COUNT, -1);  // known sizes, for progress
    ULONGLONG freeBefore = 0;

    // Updated while it runs; the UI thread reads these for progress.
    std::atomic<bool>   cancel{false};
    std::atomic<int>    current{-1};
    std::atomic<int>    progress{0};  // 0..1000
    std::atomic<LONG>   files{0};     // deleted (or found, when scanning)
    std::atomic<LONG>   skipped{0};
    std::atomic<LONG>   kept{0};      // recent temp files left alone
    std::atomic<LONG64> bytes{0};     // freed (or found, when scanning)
    std::vector<LONG64> catBytes = std::vector<LONG64>(CATEGORY_COUNT, -1);  // per finished category

    // Worker-only bookkeeping
    bool ageFilter = false;
    double progBase = 0, progSpan = 0;
    LONG64 catStart = 0, catExpected = 0;
    std::vector<std::wstring> done;  // folders already handled in this run
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
static HWND g_hExclWnd    = NULL;
static HWND g_hExclList   = NULL;
static HWND g_hExclRemove = NULL;
static HMENU g_hOptionsMenu = NULL;
static HMENU g_hThemeMenu   = NULL;
static bool g_isAdmin     = false;
static UINT g_dpi         = 96;
static Mode g_mode        = MODE_QUICK;

static Job* g_job        = nullptr;  // the scan or clean shown in the log
static Job* g_measureJob = nullptr;  // sizes for the Custom Cleanup checklist
static std::vector<LONG64> g_knownBytes(CATEGORY_COUNT, -1);  // per category, -1 = unknown
static bool g_sizesFresh      = false;
static bool g_remeasure       = false;
static bool g_scanReady       = false;  // a Quick Scan found something: the button offers Clean Now
static bool g_closeAfterClean = false;
static bool g_listLoading     = false;
static bool g_checkingUpdates = false;
static int  g_progress        = 0;
static RECT g_progressRect = {}, g_logRect = {}, g_listRect = {};

// Settings, kept in HKCU\Software\OptiCore Cleaner
enum ThemeSetting { THEME_SYSTEM, THEME_LIGHT, THEME_DARK };
static bool g_keepRecent   = true;
static int  g_themeSetting = THEME_SYSTEM;
static std::vector<std::wstring> g_exclusions;
static std::vector<std::wstring> g_customSelection;  // category names
static bool g_hasCustomSelection = false;
static int  g_scheduled = -1;  // weekly task exists: -1 not checked yet

struct Palette { COLORREF bg, text, muted, accent, panel, border, hover; };
static const Palette kLight = { RGB(245, 246, 250), RGB(50, 50, 60), RGB(105, 108, 120), RGB(50, 120, 220),
                                RGB(255, 255, 255), RGB(205, 208, 216), RGB(225, 228, 236) };
static const Palette kDark  = { RGB(32, 32, 36), RGB(232, 232, 236), RGB(160, 162, 172), RGB(96, 165, 250),
                                RGB(43, 43, 48), RGB(72, 72, 80), RGB(62, 62, 70) };
static Palette g_pal = kLight;
static bool g_dark   = false;

static HBRUSH g_hBgBrush    = NULL;
static HBRUSH g_hPanelBrush = NULL;
static HBRUSH g_hHoverBrush = NULL;
static HFONT  g_hFontTitle  = NULL;
static HFONT  g_hFontNormal = NULL;
static HFONT  g_hFontBtn    = NULL;

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

// Without a window (automatic mode) lines just collect in g_pendingLog.
void AppendLog(const std::wstring& text) {
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        g_pendingLog += text;
        g_pendingLog += L"\r\n";
    }
    if (g_hWnd && !g_logPosted.exchange(true))
        PostMessage(g_hWnd, WM_LOG_MESSAGE, 0, 0);
}

static ULONGLONG FreeSpaceOnSystemDrive() {
    wchar_t root[MAX_PATH] = L"C:\\";
    ExpandEnvironmentStrings(L"%SystemDrive%\\", root, MAX_PATH);
    ULARGE_INTEGER freeBytes = {};
    GetDiskFreeSpaceEx(root, &freeBytes, NULL, NULL);
    return freeBytes.QuadPart;
}

// Runs a console tool without a window and returns its exit code.
static DWORD RunHidden(std::wstring cmdLine, DWORD timeoutMs = 30000) {
    STARTUPINFO si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcess(NULL, &cmdLine[0], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return (DWORD)-1;
    WaitForSingleObject(pi.hProcess, timeoutMs);
    DWORD code = (DWORD)-1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

// Quoted full path of a tool in System32, so a same-named file elsewhere is never run.
static std::wstring SystemTool(const wchar_t* exe) {
    wchar_t dir[MAX_PATH];
    GetSystemDirectory(dir, MAX_PATH);
    return std::wstring(L"\"") + dir + L"\\" + exe + L"\"";
}

static bool WriteUtf8File(const std::wstring& path, const std::wstring& text) {
    int len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), NULL, 0, NULL, NULL);
    std::string data = "\xEF\xBB\xBF";
    size_t offset = data.size();
    data.resize(offset + len);
    if (len > 0)
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &data[offset], len, NULL, NULL);

    HANDLE file = CreateFile(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(file, data.data(), (DWORD)data.size(), &written, NULL);
    CloseHandle(file);
    return ok && written == data.size();
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

static DWORD ReadSetting(const wchar_t* name, DWORD fallback) {
    DWORD value = 0, size = sizeof(value);
    if (RegGetValue(HKEY_CURRENT_USER, REG_KEY, name, RRF_RT_REG_DWORD, NULL, &value, &size) != ERROR_SUCCESS)
        return fallback;
    return value;
}

static void WriteSetting(const wchar_t* name, DWORD value) {
    RegSetKeyValue(HKEY_CURRENT_USER, REG_KEY, name, REG_DWORD, &value, sizeof(value));
}

static std::vector<std::wstring> ReadList(const wchar_t* name, bool* found = nullptr) {
    std::vector<std::wstring> items;
    if (found) *found = false;
    DWORD size = 0;
    if (RegGetValue(HKEY_CURRENT_USER, REG_KEY, name, RRF_RT_REG_MULTI_SZ, NULL, NULL, &size) != ERROR_SUCCESS)
        return items;
    std::vector<wchar_t> buf(size / sizeof(wchar_t) + 2, L'\0');
    if (RegGetValue(HKEY_CURRENT_USER, REG_KEY, name, RRF_RT_REG_MULTI_SZ, NULL, buf.data(), &size) != ERROR_SUCCESS)
        return items;
    if (found) *found = true;
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1)
        items.push_back(p);
    return items;
}

static void WriteList(const wchar_t* name, const std::vector<std::wstring>& items) {
    std::wstring data;
    for (const std::wstring& item : items) {
        data += item;
        data.push_back(L'\0');
    }
    data.push_back(L'\0');
    if (items.empty()) data.push_back(L'\0');
    RegSetKeyValue(HKEY_CURRENT_USER, REG_KEY, name, REG_MULTI_SZ, data.c_str(), (DWORD)(data.size() * sizeof(wchar_t)));
}

static void LoadSettings() {
    g_keepRecent   = ReadSetting(L"KeepRecentTemp", 1) != 0;
    g_themeSetting = (int)ReadSetting(L"Theme", THEME_SYSTEM);
    if (g_themeSetting > THEME_DARK) g_themeSetting = THEME_SYSTEM;
    g_exclusions      = ReadList(L"Exclusions");
    g_customSelection = ReadList(L"CustomSelection", &g_hasCustomSelection);
}

static std::vector<int> SystemCategories() {
    std::vector<int> result;
    for (int i = 0; i < CATEGORY_COUNT; ++i)
        if (g_categories[i].group == GROUP_SYSTEM)
            result.push_back(i);
    return result;
}

// Whether a category is ticked in Custom Cleanup: the saved selection, or system junk by default.
static bool IsCustomChecked(int index) {
    if (!g_hasCustomSelection)
        return g_categories[index].group == GROUP_SYSTEM;
    for (const std::wstring& name : g_customSelection)
        if (name == g_categories[index].name)
            return true;
    return false;
}

static std::vector<int> SavedCustomCategories() {
    std::vector<int> result;
    for (int i = 0; i < CATEGORY_COUNT; ++i)
        if (IsCustomChecked(i))
            result.push_back(i);
    return result;
}

// ---------------------------------------------------------------------------
// Cleaning (runs on a worker thread, or the main thread in automatic mode)
// ---------------------------------------------------------------------------

static void Log(const Job& job, const std::wstring& text) {
    if (!job.quiet) AppendLog(text);
}

// True when `path` is `folder` itself or anything inside it.
static bool IsUnder(const std::wstring& path, const std::wstring& folder) {
    if (folder.empty() || path.size() < folder.size()) return false;
    if (_wcsnicmp(path.c_str(), folder.c_str(), folder.size()) != 0) return false;
    return path.size() == folder.size() || path[folder.size()] == L'\\' || folder.back() == L'\\';
}

static bool IsExcluded(const Job& job, const std::wstring& path) {
    for (const std::wstring& folder : job.exclusions)
        if (IsUnder(path, folder))
            return true;
    return false;
}

static const FILETIME& Newer(const FILETIME& a, const FILETIME& b) {
    return CompareFileTime(&a, &b) >= 0 ? a : b;
}

static void UpdateProgress(Job& job) {
    if (job.catExpected <= 0) return;
    double done = (double)(job.bytes - job.catStart) / (double)job.catExpected;
    job.progress = (int)((job.progBase + job.progSpan * (done < 1.0 ? done : 1.0)) * 1000.0);
}

// Deletes (or, when scanning, counts) one file. `changed` is the newer of its
// creation and write times: extracted installer files keep old write times.
static void ProcessFile(Job& job, const std::wstring& path, DWORD attrs, ULONGLONG size, const FILETIME& changed) {
    if (job.ageFilter && CompareFileTime(&changed, &job.cutoff) > 0) {
        ++job.kept;
        return;
    }
    if (!job.scanOnly) {
        if (attrs & FILE_ATTRIBUTE_READONLY)
            SetFileAttributes(path.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
        if (!DeleteFile(path.c_str())) {
            ++job.skipped;
            Log(job, L"  Skipped: " + path);
            return;
        }
        Log(job, L"  Deleted: " + path);
    }
    ++job.files;
    job.bytes += (LONG64)size;
    UpdateProgress(job);
}

// Deletes everything inside `path` that matches `pattern` but leaves `path` itself in place.
void ClearDirectory(Job& job, const std::wstring& path, const wchar_t* pattern = L"*", bool keepFolders = false) {
    WIN32_FIND_DATA fd;
    HANDLE hFind = FindFirstFile((path + L"\\*").c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (job.cancel) break;

        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;

        std::wstring full = path + L"\\" + name;
        if (IsExcluded(job, full)) {
            Log(job, L"  Excluded: " + full);
            continue;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            // Never follow junctions/symlinks: their targets live outside this folder.
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                if (!job.scanOnly) {
                    ++job.skipped;
                    Log(job, L"  Skipped (link): " + full);
                }
                continue;
            }
            ClearDirectory(job, full, pattern, keepFolders);
            if (!keepFolders && !job.scanOnly) {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
                    SetFileAttributes(full.c_str(), fd.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
                RemoveDirectory(full.c_str());  // fails harmlessly while kept files remain
            }
        } else if (PathMatchSpec(name.c_str(), pattern)) {
            ProcessFile(job, full, fd.dwFileAttributes,
                ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow,
                Newer(fd.ftCreationTime, fd.ftLastWriteTime));
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

// Lowercase long form of a path, so %TEMP% (sometimes an 8.3 short path) and
// Users\*\AppData\Local\Temp are recognised as the same folder.
static std::wstring NormalizePath(const std::wstring& path) {
    wchar_t buf[MAX_PATH * 2];
    DWORD len = GetLongPathName(path.c_str(), buf, ARRAYSIZE(buf));
    std::wstring result = (len > 0 && len < ARRAYSIZE(buf)) ? std::wstring(buf) : path;
    if (!result.empty())
        CharLowerBuff(&result[0], (DWORD)result.size());
    return result;
}

// Cleans one folder (its contents) or one file.
void CleanLocation(Job& job, const std::wstring& path, const wchar_t* pattern = L"*", bool keepFolders = false) {
    if (job.cancel) return;

    // Only absolute paths, and never a drive root.
    if (path.length() <= 3 || path[1] != L':') return;
    if (IsExcluded(job, path)) {
        Log(job, L"  Excluded: " + path);
        return;
    }

    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesEx(path.c_str(), GetFileExInfoStandard, &info)) return;

    if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        ClearDirectory(job, path, pattern, keepFolders);
    else
        ProcessFile(job, path, info.dwFileAttributes,
            ((ULONGLONG)info.nFileSizeHigh << 32) | info.nFileSizeLow,
            Newer(info.ftCreationTime, info.ftLastWriteTime));
}

static void ProcessRecycleBin(Job& job) {
    SHQUERYRBINFO info = {};
    info.cbSize = sizeof(info);
    if (FAILED(SHQueryRecycleBin(NULL, &info)) || info.i64NumItems == 0) return;

    if (!job.scanOnly) {
        HRESULT hr = SHEmptyRecycleBin(NULL, NULL,
            SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND);
        if (FAILED(hr)) {
            Log(job, L"  Could not empty the Recycle Bin (access denied).");
            return;
        }
    }
    job.files += (LONG)info.i64NumItems;
    job.bytes += info.i64Size;
}

// DISM removes superseded Windows Update components. It can't be measured
// quickly, so the freed amount is the change in free space on the system drive.
static void RunComponentCleanup(Job& job) {
    if (job.scanOnly) return;
    if (!g_isAdmin) {
        Log(job, L"  Needs administrator, skipping.");
        return;
    }

    // A 32-bit build has to go through Sysnative to reach the 64-bit DISM.
    BOOL wow64 = FALSE;
    IsWow64Process(GetCurrentProcess(), &wow64);
    wchar_t dism[MAX_PATH];
    ExpandEnvironmentStrings(wow64 ? L"%WINDIR%\\Sysnative\\dism.exe" : L"%WINDIR%\\System32\\dism.exe",
        dism, MAX_PATH);
    std::wstring cmd = std::wstring(L"\"") + dism + L"\" /Online /Cleanup-Image /StartComponentCleanup";

    Log(job, L"  Running DISM. This can take 10 minutes or more...");
    ULONGLONG before = FreeSpaceOnSystemDrive();

    STARTUPINFO si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcess(NULL, &cmd[0], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        Log(job, L"  Could not start DISM.");
        return;
    }
    bool told = false;
    while (WaitForSingleObject(pi.hProcess, 500) == WAIT_TIMEOUT) {
        if (job.cancel && !told) {
            Log(job, L"  DISM can't be stopped safely. Waiting for it to finish...");
            told = true;
        }
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (code != 0) {
        wchar_t line[96];
        swprintf_s(line, L"  DISM finished with error code 0x%08lX.", code);
        Log(job, line);
    }

    ULONGLONG after = FreeSpaceOnSystemDrive();
    if (after > before)
        job.bytes += (LONG64)(after - before);
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

static void RunCategory(Job& job, int index) {
    const Category& cat = g_categories[index];
    LONG   files0 = job.files;
    LONG64 bytes0 = job.bytes;

    if (!job.scanOnly) {
        Log(job, L"");
        std::wstring header = std::wstring(L"Cleaning ") + cat.name;
        if ((cat.flags & F_ADMIN) && !g_isAdmin)
            header += L" (needs administrator for a full clean)";
        Log(job, header + L"...");
    }

    job.ageFilter = job.keepRecent && (cat.flags & F_RECENT);
    if (cat.flags & F_RECYCLE) {
        ProcessRecycleBin(job);
    } else if (cat.flags & F_DISM) {
        RunComponentCleanup(job);
    } else {
        bool restartService = false;
        if (cat.service && !job.scanOnly) {
            StopResult stop = StopServiceForCleaning(cat.service);
            if (stop == StopResult::Busy) {
                Log(job, L"  Windows Update is busy right now, skipping.");
                return;
            }
            restartService = stop == StopResult::Stopped;
        }
        for (const PathRule& rule : cat.paths) {
            for (const std::wstring& path : ExpandPath(rule.path)) {
                std::wstring key = NormalizePath(path) + L"|" + rule.pattern;
                if (std::find(job.done.begin(), job.done.end(), key) != job.done.end()) continue;
                job.done.push_back(key);
                CleanLocation(job, path, rule.pattern, rule.keepFolders);
            }
        }
        if (restartService)
            StartServiceByName(cat.service);
    }

    if (!job.cancel)
        job.catBytes[index] = job.bytes - bytes0;

    wchar_t line[256];
    swprintf_s(line, job.scanOnly ? L"  %ls: %ls (%ld files)" : L"  %ls: freed %ls (%ld files)",
        cat.name, FormatBytes((ULONGLONG)(job.bytes - bytes0)).c_str(), job.files - files0);
    Log(job, line);
}

void RunJob(Job& job) {
    // Progress is split between categories by their known sizes when every one
    // is known (with a small floor so tiny ones still move the bar), else evenly.
    size_t count = job.categories.size();
    bool allKnown = true;
    LONG64 total = 0;
    for (int index : job.categories) {
        if (job.expected[index] < 0) allKnown = false;
        else total += job.expected[index];
    }
    bool weighted = allKnown && total > 0;
    std::vector<double> weight(count, 1.0);
    double sum = 0;
    for (size_t k = 0; k < count; ++k) {
        if (weighted) weight[k] = (double)job.expected[job.categories[k]] + total / 50.0;
        sum += weight[k];
    }

    double base = 0;
    for (size_t k = 0; k < count; ++k) {
        if (job.cancel) break;
        int index = job.categories[k];
        job.current     = index;
        job.progBase    = base / sum;
        job.progSpan    = weight[k] / sum;
        job.catStart    = job.bytes;
        job.catExpected = weighted ? job.expected[index] : 0;
        job.progress    = (int)(job.progBase * 1000.0);
        RunCategory(job, index);
        if (job.quiet && !job.cancel && g_hWnd)
            PostMessage(g_hWnd, WM_MEASURED, index, (LPARAM)&job);
        base += weight[k];
    }
    job.progress = 1000;
}

DWORD WINAPI JobThread(LPVOID param) {
    Job* job = (Job*)param;
    RunJob(*job);
    PostMessage(g_hWnd, WM_JOB_DONE, 0, (LPARAM)job);
    return 0;
}

static void PrepareJob(Job& job) {
    job.keepRecent = g_keepRecent;
    job.exclusions = g_exclusions;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER t;
    t.LowPart  = now.dwLowDateTime;
    t.HighPart = now.dwHighDateTime;
    t.QuadPart -= (ULONGLONG)RECENT_HOURS * 3600 * 10000000;
    job.cutoff.dwLowDateTime  = t.LowPart;
    job.cutoff.dwHighDateTime = t.HighPart;
}

// Logs the end-of-run summary and returns a one-line version of it.
static std::wstring LogSummary(const Job& job) {
    std::wstring total = FormatBytes((ULONGLONG)job.bytes.load());
    wchar_t line[256];
    AppendLog(L"");
    AppendLog(L"============================");
    if (job.scanOnly) {
        AppendLog(job.cancel ? L"  Scan stopped." : L"  Scan complete!");
        swprintf_s(line, L"  Can be freed  : %ls", total.c_str());
        AppendLog(line);
        swprintf_s(line, L"  Files found   : %ld", job.files.load());
        AppendLog(line);
        if (job.kept) {
            swprintf_s(line, L"  Kept (recent) : %ld", job.kept.load());
            AppendLog(line);
        }
        AppendLog(L"============================");
        if (job.bytes > 0) {
            AppendLog(L"Click Clean Now to remove it.");
            swprintf_s(line, L"Found %ls you can free. Click Clean Now to remove it.", total.c_str());
        } else {
            swprintf_s(line, L"Nothing to clean right now.");
        }
    } else {
        AppendLog(job.cancel ? L"  Cleaning stopped." : L"  Cleaning complete!");
        swprintf_s(line, L"  Space freed   : %ls", total.c_str());
        AppendLog(line);
        swprintf_s(line, L"  Files deleted : %ld", job.files.load());
        AppendLog(line);
        swprintf_s(line, L"  Skipped       : %ld", job.skipped.load());
        AppendLog(line);
        if (job.kept) {
            swprintf_s(line, L"  Kept (recent) : %ld", job.kept.load());
            AppendLog(line);
        }
        if (job.freeBefore) {
            swprintf_s(line, L"  Free space    : %ls \x2192 %ls",
                FormatBytes(job.freeBefore).c_str(), FormatBytes(FreeSpaceOnSystemDrive()).c_str());
            AppendLog(line);
        }
        AppendLog(L"============================");
        swprintf_s(line, L"Freed %ls: %ld files deleted, %ld skipped (in use or protected).",
            total.c_str(), job.files.load(), job.skipped.load());
    }
    return line;
}

// ---------------------------------------------------------------------------
// Automatic mode (/auto): no window, used by the weekly scheduled task
// ---------------------------------------------------------------------------

static void ShowNotification(const std::wstring& text) {
    WNDCLASS wc = {};
    wc.lpfnWndProc   = DefWindowProc;
    wc.hInstance     = g_hInst;
    wc.lpszClassName = L"OptiCoreNotify";
    RegisterClass(&wc);
    HWND hWnd = CreateWindow(wc.lpszClassName, APP_NAME, 0, 0, 0, 0, 0, NULL, NULL, g_hInst, NULL);

    NOTIFYICONDATA nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = hWnd;
    nid.uID    = 1;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_INFO;
    nid.hIcon  = (HICON)LoadImage(g_hInst, MAKEINTRESOURCE(IDI_ICON1), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    nid.hBalloonIcon = (HICON)LoadImage(g_hInst, MAKEINTRESOURCE(IDI_ICON1), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    nid.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
    wcscpy_s(nid.szTip, APP_NAME);
    wcscpy_s(nid.szInfoTitle, L"Automatic cleanup finished");
    wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);

    if (Shell_NotifyIcon(NIM_ADD, &nid)) {
        // The notification goes away with the icon, so keep it up for a while.
        DWORD end = GetTickCount() + 10000;
        for (;;) {
            LONG left = (LONG)(end - GetTickCount());
            if (left <= 0) break;
            MsgWaitForMultipleObjects(0, NULL, FALSE, (DWORD)left, QS_ALLINPUT);
            MSG msg;
            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
                DispatchMessage(&msg);
        }
        Shell_NotifyIcon(NIM_DELETE, &nid);
    }
    DestroyWindow(hWnd);
    DestroyIcon(nid.hIcon);
    DestroyIcon(nid.hBalloonIcon);
}

static int RunAuto(bool custom, bool notify) {
    Job job;
    PrepareJob(job);
    job.categories = custom ? SavedCustomCategories() : SystemCategories();
    job.freeBefore = FreeSpaceOnSystemDrive();

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t line[160];
    swprintf_s(line, L"%ls %ls: automatic %ls, %04u-%02u-%02u %02u:%02u", APP_NAME, APP_VERSION,
        custom ? L"Custom Cleanup" : L"Complete Cleanup", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    AppendLog(line);
    if (!g_isAdmin)
        AppendLog(L"Note: not running as administrator, so system folders were mostly skipped.");

    RunJob(job);
    std::wstring summary = LogSummary(job);

    // Keep the log of the last automatic run.
    wchar_t dir[MAX_PATH];
    ExpandEnvironmentStrings(L"%LOCALAPPDATA%\\OptiCore Cleaner", dir, MAX_PATH);
    CreateDirectory(dir, NULL);
    std::wstring log;
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        log.swap(g_pendingLog);
    }
    WriteUtf8File(std::wstring(dir) + L"\\last-auto-clean.log", log);

    if (notify)
        ShowNotification(summary);
    return 0;
}

// ---------------------------------------------------------------------------
// Scheduled cleanup (Task Scheduler)
// ---------------------------------------------------------------------------

static std::wstring XmlEscape(const std::wstring& text) {
    std::wstring out;
    for (wchar_t c : text) {
        switch (c) {
        case L'&': out += L"&amp;";  break;
        case L'<': out += L"&lt;";   break;
        case L'>': out += L"&gt;";   break;
        case L'"': out += L"&quot;"; break;
        default:   out += c;
        }
    }
    return out;
}

static bool IsCleanupScheduled() {
    return RunHidden(SystemTool(L"schtasks.exe") + L" /Query /TN \"" TASK_NAME L"\"") == 0;
}

// Weekly on Sunday at 12:00, run as soon as possible if the PC was off. As
// administrator the task runs elevated so system folders get cleaned too.
static bool CreateScheduledTask() {
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileName(NULL, exe, MAX_PATH)) return false;
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t start[32];
    swprintf_s(start, L"%04u-%02u-%02uT12:00:00", now.wYear, now.wMonth, now.wDay);

    std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"  <RegistrationInfo><Description>Weekly Complete Cleanup by OptiCore Cleaner</Description></RegistrationInfo>\r\n"
        L"  <Triggers><CalendarTrigger><StartBoundary>" + std::wstring(start) + L"</StartBoundary>"
        L"<ScheduleByWeek><DaysOfWeek><Sunday /></DaysOfWeek><WeeksInterval>1</WeeksInterval></ScheduleByWeek>"
        L"</CalendarTrigger></Triggers>\r\n"
        L"  <Principals><Principal id=\"Author\"><LogonType>InteractiveToken</LogonType><RunLevel>" +
        std::wstring(g_isAdmin ? L"HighestAvailable" : L"LeastPrivilege") + L"</RunLevel></Principal></Principals>\r\n"
        L"  <Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
        L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
        L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
        L"<StartWhenAvailable>true</StartWhenAvailable>"
        L"<ExecutionTimeLimit>PT1H</ExecutionTimeLimit></Settings>\r\n"
        L"  <Actions Context=\"Author\"><Exec><Command>" + XmlEscape(exe) + L"</Command>"
        L"<Arguments>/auto</Arguments></Exec></Actions>\r\n"
        L"</Task>\r\n";

    wchar_t tempDir[MAX_PATH];
    GetTempPath(MAX_PATH, tempDir);
    std::wstring xmlPath = std::wstring(tempDir) + L"OptiCore-task.xml";
    HANDLE file = CreateFile(xmlPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;
    const wchar_t bom = 0xFEFF;
    DWORD written = 0;
    WriteFile(file, &bom, sizeof(bom), &written, NULL);
    WriteFile(file, xml.c_str(), (DWORD)(xml.size() * sizeof(wchar_t)), &written, NULL);
    CloseHandle(file);

    DWORD code = RunHidden(SystemTool(L"schtasks.exe") +
        L" /Create /TN \"" TASK_NAME L"\" /XML \"" + xmlPath + L"\" /F");
    DeleteFile(xmlPath.c_str());
    return code == 0;
}

static bool DeleteScheduledTask() {
    return RunHidden(SystemTool(L"schtasks.exe") + L" /Delete /TN \"" TASK_NAME L"\" /F") == 0;
}

// ---------------------------------------------------------------------------
// Update check (GitHub releases)
// ---------------------------------------------------------------------------

struct UpdateInfo {
    DWORD status = 0;  // HTTP status, 0 when the request failed
    std::wstring tag;
};

// Pulls a string value out of a JSON object; enough for GitHub's flat release fields.
static std::wstring JsonString(const std::string& json, const char* key) {
    std::string needle = std::string("\"") + key + "\"";
    size_t p = json.find(needle);
    if (p == std::string::npos) return L"";
    p = json.find(':', p + needle.size());
    if (p == std::string::npos) return L"";
    p = json.find('"', p);
    if (p == std::string::npos) return L"";
    size_t end = json.find('"', p + 1);
    if (end == std::string::npos) return L"";
    std::string value = json.substr(p + 1, end - p - 1);
    return std::wstring(value.begin(), value.end());
}

// Compares dotted version numbers, ignoring a leading "v".
static bool IsNewerVersion(const std::wstring& latest, const std::wstring& current) {
    auto parse = [](const std::wstring& s) {
        std::vector<int> parts;
        size_t i = 0;
        while (i < s.size() && !iswdigit(s[i])) ++i;
        while (i < s.size() && iswdigit(s[i])) {
            int n = 0;
            while (i < s.size() && iswdigit(s[i])) n = n * 10 + (s[i++] - L'0');
            parts.push_back(n);
            if (i < s.size() && s[i] == L'.') ++i;
        }
        return parts;
    };
    std::vector<int> a = parse(latest), b = parse(current);
    if (a.empty()) return false;
    for (size_t i = 0; i < (std::max)(a.size(), b.size()); ++i) {
        int x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
        if (x != y) return x > y;
    }
    return false;
}

static void FetchLatestRelease(UpdateInfo& info) {
    HINTERNET session = WinHttpOpen(L"OptiCore-Cleaner/" APP_VERSION, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return;
    WinHttpSetTimeouts(session, 5000, 5000, 10000, 10000);
    HINTERNET conn = WinHttpConnect(session, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", L"/repos/" GITHUB_REPO L"/releases/latest",
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : NULL;

    if (req && WinHttpSendRequest(req, L"Accept: application/vnd.github+json\r\n", (DWORD)-1L,
                                  WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, NULL)) {
        DWORD size = sizeof(info.status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &info.status, &size, WINHTTP_NO_HEADER_INDEX);
        std::string body;
        DWORD available = 0;
        while (WinHttpQueryDataAvailable(req, &available) && available > 0 && body.size() < (1 << 20)) {
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(req, &chunk[0], available, &read) || read == 0) break;
            body.append(chunk, 0, read);
        }
        info.tag = JsonString(body, "tag_name");
    }
    if (req)  WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
}

DWORD WINAPI UpdateThread(LPVOID) {
    UpdateInfo* info = new UpdateInfo();
    FetchLatestRelease(*info);
    if (!PostMessage(g_hWnd, WM_UPDATE_RESULT, 0, (LPARAM)info))
        delete info;
    return 0;
}

// ---------------------------------------------------------------------------
// Theme (light / dark)
// ---------------------------------------------------------------------------

static bool WantDarkMode() {
    if (g_themeSetting != THEME_SYSTEM) return g_themeSetting == THEME_DARK;
    DWORD light = 1, size = sizeof(light);
    RegGetValue(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &light, &size);
    return light == 0;
}

static DWORD WindowsBuild() {
    typedef LONG (WINAPI* RtlGetVersionFn)(OSVERSIONINFOW*);
    auto getVersion = (RtlGetVersionFn)GetProcAddress(GetModuleHandle(L"ntdll.dll"), "RtlGetVersion");
    OSVERSIONINFOW v = { sizeof(v) };
    return getVersion && getVersion(&v) == 0 ? v.dwBuildNumber : 0;
}

// Popup menus only turn dark through undocumented uxtheme exports
// (SetPreferredAppMode #135 and FlushMenuThemes #136, Windows 10 1903+).
static void SetMenusDark(bool dark) {
    if (WindowsBuild() < 18362) return;
    static HMODULE uxtheme = LoadLibraryEx(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!uxtheme) return;
    typedef int  (WINAPI* SetPreferredAppModeFn)(int);
    typedef void (WINAPI* FlushMenuThemesFn)();
    auto setMode = (SetPreferredAppModeFn)GetProcAddress(uxtheme, MAKEINTRESOURCEA(135));
    auto flush   = (FlushMenuThemesFn)GetProcAddress(uxtheme, MAKEINTRESOURCEA(136));
    if (setMode) setMode(dark ? 2 /* ForceDark */ : 3 /* ForceLight */);
    if (flush) flush();
}

static void ThemeTitleBar(HWND hWnd) {
    BOOL dark = g_dark;
    if (FAILED(DwmSetWindowAttribute(hWnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark))))
        DwmSetWindowAttribute(hWnd, 19, &dark, sizeof(dark));  // Windows 10 before 20H1
}

static void ThemeControl(HWND hCtrl) {
    if (hCtrl) SetWindowTheme(hCtrl, g_dark ? L"DarkMode_Explorer" : NULL, NULL);
}

static void ThemeExclusionsWindow();

static void ApplyTheme() {
    g_dark = WantDarkMode();
    g_pal  = g_dark ? kDark : kLight;

    HBRUSH oldBg = g_hBgBrush, oldPanel = g_hPanelBrush, oldHover = g_hHoverBrush;
    g_hBgBrush    = CreateSolidBrush(g_pal.bg);
    g_hPanelBrush = CreateSolidBrush(g_pal.panel);
    g_hHoverBrush = CreateSolidBrush(g_pal.hover);
    if (oldBg)    DeleteObject(oldBg);
    if (oldPanel) DeleteObject(oldPanel);
    if (oldHover) DeleteObject(oldHover);

    SetMenusDark(g_dark);
    ThemeTitleBar(g_hWnd);
    HWND controls[] = { g_hLog, g_hList, g_hSelAll, g_hSelNone, g_hBtn, g_hAdminBtn,
                        g_hModeBtn[0], g_hModeBtn[1], g_hModeBtn[2] };
    for (HWND hCtrl : controls)
        ThemeControl(hCtrl);
    ListView_SetBkColor(g_hList, g_pal.panel);
    ListView_SetTextBkColor(g_hList, g_pal.panel);
    ListView_SetTextColor(g_hList, g_pal.text);
    ThemeExclusionsWindow();

    DrawMenuBar(g_hWnd);
    RedrawWindow(g_hWnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

// Undocumented messages Windows sends so an app can paint its own menu bar;
// the only way to get a dark menu bar.
#define WM_UAHDRAWMENU      0x0091
#define WM_UAHDRAWMENUITEM  0x0092

struct UAHMENU { HMENU hmenu; HDC hdc; DWORD dwFlags; };
union UAHMENUITEMMETRICS {
    struct { DWORD cx, cy; } rgsizeBar[2];
    struct { DWORD cx, cy; } rgsizePopup[4];
};
struct UAHMENUPOPUPMETRICS { DWORD rgcx[4]; DWORD fUpdateMaxWidths : 2; };
struct UAHMENUITEM { int iPosition; UAHMENUITEMMETRICS umim; UAHMENUPOPUPMETRICS umpm; };
struct UAHDRAWMENUITEM { DRAWITEMSTRUCT dis; UAHMENU um; UAHMENUITEM umi; };

static void PaintMenuBarBackground(HWND hWnd, HDC hdc) {
    MENUBARINFO mbi = { sizeof(mbi) };
    if (!GetMenuBarInfo(hWnd, OBJID_MENU, 0, &mbi)) return;
    RECT rcWindow;
    GetWindowRect(hWnd, &rcWindow);
    RECT rc = mbi.rcBar;
    OffsetRect(&rc, -rcWindow.left, -rcWindow.top);
    FillRect(hdc, &rc, g_hBgBrush);
}

static void PaintMenuBarItem(const UAHDRAWMENUITEM* item) {
    wchar_t text[64] = {};
    MENUITEMINFO mii = { sizeof(mii) };
    mii.fMask      = MIIM_STRING;
    mii.dwTypeData = text;
    mii.cch        = ARRAYSIZE(text) - 1;
    GetMenuItemInfo(item->um.hmenu, item->umi.iPosition, TRUE, &mii);

    UINT state = item->dis.itemState;
    HDC hdc = item->um.hdc;
    RECT rc = item->dis.rcItem;
    FillRect(hdc, &rc, (state & (ODS_HOTLIGHT | ODS_SELECTED)) ? g_hHoverBrush : g_hBgBrush);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, (state & (ODS_INACTIVE | ODS_GRAYED | ODS_DISABLED)) ? g_pal.muted : g_pal.text);
    UINT format = DT_CENTER | DT_SINGLELINE | DT_VCENTER;
    if (state & ODS_NOACCEL) format |= DT_HIDEPREFIX;
    DrawText(hdc, text, -1, &rc, format);
}

// Covers the light line Windows draws between the menu bar and the client area.
static void PaintMenuBarLine(HWND hWnd) {
    RECT rcClient, rcWindow;
    GetClientRect(hWnd, &rcClient);
    MapWindowPoints(hWnd, NULL, (POINT*)&rcClient, 2);
    GetWindowRect(hWnd, &rcWindow);
    OffsetRect(&rcClient, -rcWindow.left, -rcWindow.top);
    RECT line = rcClient;
    line.bottom = line.top;
    line.top--;
    HDC hdc = GetWindowDC(hWnd);
    FillRect(hdc, &line, g_hBgBrush);
    ReleaseDC(hWnd, hdc);
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

// Shows a task dialog. With `action`, it also has that button and returns true when it's clicked.
static bool ShowMessage(HWND owner, const wchar_t* instruction, const std::wstring& content,
                        const wchar_t* action = nullptr, PCWSTR icon = MAKEINTRESOURCE(IDI_ICON1)) {
    TASKDIALOG_BUTTON button = { IDOK, action };
    TASKDIALOGCONFIG tdc = {};
    tdc.cbSize             = sizeof(tdc);
    tdc.hwndParent         = owner;
    tdc.hInstance          = icon == MAKEINTRESOURCE(IDI_ICON1) ? g_hInst : NULL;
    tdc.dwFlags            = TDF_ALLOW_DIALOG_CANCELLATION;
    tdc.pszWindowTitle     = APP_NAME;
    tdc.pszMainIcon        = icon;
    tdc.pszMainInstruction = instruction;
    tdc.pszContent         = content.c_str();
    if (action) {
        tdc.cButtons        = 1;
        tdc.pButtons        = &button;
        tdc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    } else {
        tdc.dwCommonButtons = TDCBF_OK_BUTTON;
    }
    int clicked = 0;
    TaskDialogIndirect(&tdc, &clicked, NULL, NULL);
    return action && clicked == IDOK;
}

static int RowOfCategory(int index) {
    LVFINDINFO fi = {};
    fi.flags  = LVFI_PARAM;
    fi.lParam = index;
    return ListView_FindItem(g_hList, -1, &fi);
}

static int CategoryOfRow(int row) {
    LVITEM item = {};
    item.mask  = LVIF_PARAM;
    item.iItem = row;
    ListView_GetItem(g_hList, &item);
    return (int)item.lParam;
}

static void SetSizeText(int index) {
    const Category& cat = g_categories[index];
    LONG64 size = g_knownBytes[index];
    std::wstring text;
    if (cat.flags & F_DISM)
        text = L"\x2014";  // can't be measured quickly
    else if (size < 0)
        text = g_measureJob ? L"\x2026" : L"";
    else if (size == 0)
        text = ((cat.flags & F_ADMIN) && !g_isAdmin) ? L"needs admin" : L"empty";
    else
        text = FormatBytes((ULONGLONG)size);
    ListView_SetItemText(g_hList, RowOfCategory(index), 2, (LPWSTR)text.c_str());
}

static void RefreshSizes() {
    for (int i = 0; i < CATEGORY_COUNT; ++i)
        SetSizeText(i);
}

static void FillCategoryList() {
    const struct { Group id; const wchar_t* header; } groups[] = {
        { GROUP_SYSTEM,   L"System junk (Complete Cleanup)" },
        { GROUP_BROWSER,  L"Browsers & apps" },
        { GROUP_GAMING,   L"Gaming" },
        { GROUP_PRIVACY,  L"Privacy" },
        { GROUP_ADVANCED, L"Advanced" },
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
    col.mask = LVCF_WIDTH | LVCF_FMT;
    col.fmt  = LVCFMT_LEFT;
    ListView_InsertColumn(g_hList, 0, &col);
    ListView_InsertColumn(g_hList, 1, &col);
    col.fmt = LVCFMT_RIGHT;
    ListView_InsertColumn(g_hList, 2, &col);

    g_listLoading = true;
    for (int i = 0; i < CATEGORY_COUNT; ++i) {
        LVITEM item = {};
        item.mask     = LVIF_TEXT | LVIF_PARAM | LVIF_GROUPID;
        item.iItem    = i;
        item.pszText  = (LPWSTR)g_categories[i].name;
        item.lParam   = i;
        item.iGroupId = g_categories[i].group;
        int row = ListView_InsertItem(g_hList, &item);
        ListView_SetItemText(g_hList, row, 1, (LPWSTR)g_categories[i].description);
        ListView_SetCheckState(g_hList, row, IsCustomChecked(i));
    }
    g_listLoading = false;
    RefreshSizes();
}

static std::vector<int> CheckedCategories() {
    std::vector<int> result;
    int count = ListView_GetItemCount(g_hList);
    for (int row = 0; row < count; ++row)
        if (ListView_GetCheckState(g_hList, row))
            result.push_back(CategoryOfRow(row));
    std::sort(result.begin(), result.end());
    return result;
}

static void SaveCustomSelection() {
    g_customSelection.clear();
    for (int index : CheckedCategories())
        g_customSelection.push_back(g_categories[index].name);
    g_hasCustomSelection = true;
    WriteList(L"CustomSelection", g_customSelection);
}

static void UpdateSelectionStatus() {
    if (g_job || g_mode != MODE_CUSTOM) return;
    std::vector<int> checked = CheckedCategories();
    LONG64 total = 0;
    bool unknown = false;
    for (int index : checked) {
        if (g_knownBytes[index] < 0) unknown = true;
        else total += g_knownBytes[index];
    }
    wchar_t line[160];
    int count = (int)checked.size();
    if (count == 0)
        swprintf_s(line, L"%ls", g_modeHint[MODE_CUSTOM]);
    else if (g_measureJob)
        swprintf_s(line, L"%d selected. Measuring sizes...", count);
    else if (total == 0 && unknown)
        swprintf_s(line, L"%d selected. Click Clean Selected.", count);
    else
        swprintf_s(line, L"%d selected, %ls%ls to free. Click Clean Selected.", count,
            unknown ? L"over " : L"", FormatBytes((ULONGLONG)total).c_str());
    SetWindowText(g_hStatus, line);
}

static void SetAllChecks(BOOL checked) {
    g_listLoading = true;
    int count = ListView_GetItemCount(g_hList);
    for (int row = 0; row < count; ++row)
        ListView_SetCheckState(g_hList, row, checked);
    g_listLoading = false;
    SaveCustomSelection();
    UpdateSelectionStatus();
}

static void StartMeasure() {
    if (g_measureJob || g_job) return;
    Job* job = new Job();
    PrepareJob(*job);
    job->scanOnly = true;
    job->quiet    = true;
    for (int i = 0; i < CATEGORY_COUNT; ++i)
        if (!(g_categories[i].flags & F_DISM))
            job->categories.push_back(i);

    g_measureJob = job;
    HANDLE hThread = CreateThread(NULL, 0, JobThread, job, 0, NULL);
    if (hThread) {
        CloseHandle(hThread);
    } else {
        g_measureJob = nullptr;
        delete job;
    }
    RefreshSizes();
}

// Settings that change what gets cleaned make the checklist sizes stale.
static void InvalidateSizes() {
    g_sizesFresh = false;
    std::fill(g_knownBytes.begin(), g_knownBytes.end(), -1);
    if (g_measureJob) {
        g_measureJob->cancel = true;
        g_remeasure = true;
    } else if (g_mode == MODE_CUSTOM && !g_job) {
        StartMeasure();
        UpdateSelectionStatus();
    }
}

// Shows either the Custom Cleanup checklist or the log in the content area.
static void ShowChecklist(bool show) {
    ShowWindow(g_hList,    show ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hSelAll,  show ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hSelNone, show ? SW_SHOW : SW_HIDE);
    ShowWindow(g_hLog,     show ? SW_HIDE : SW_SHOW);
    InvalidateRect(g_hWnd, NULL, TRUE);  // the frame moves with the content
}

static const wchar_t* ActionLabel() {
    return g_mode == MODE_QUICK && g_scanReady ? L"Clean Now" : g_modeAction[g_mode];
}

static void SetMode(Mode mode) {
    if (g_job) return;
    g_mode = mode;
    g_scanReady = false;
    for (int i = 0; i < 3; ++i)
        SendMessage(g_hModeBtn[i], BM_SETCHECK, i == mode ? BST_CHECKED : BST_UNCHECKED, 0);
    SetWindowText(g_hBtn, ActionLabel());
    SetWindowText(g_hStatus, g_modeHint[mode]);
    ShowChecklist(mode == MODE_CUSTOM);
    if (mode == MODE_CUSTOM) {
        if (!g_sizesFresh) StartMeasure();
        UpdateSelectionStatus();
    }
}

static void SetRunningUi(bool running) {
    for (HWND hMode : g_hModeBtn)
        EnableWindow(hMode, !running);
    if (g_hAdminBtn) EnableWindow(g_hAdminBtn, !running);
    EnableWindow(g_hBtn, TRUE);
    SetWindowText(g_hBtn, running ? L"Stop" : ActionLabel());
}

static void UpdateRunStatus() {
    if (!g_job || g_job->cancel) return;
    int current = g_job->current;
    std::wstring text = g_job->scanOnly ? L"Scanning" : L"Cleaning";
    if (current >= 0) {
        text += L" ";
        text += g_categories[current].name;
    }
    text += L"... " + FormatBytes((ULONGLONG)g_job->bytes.load()) + (g_job->scanOnly ? L" found" : L" freed");

    wchar_t old[256] = {};
    GetWindowText(g_hStatus, old, ARRAYSIZE(old));
    if (text != old)
        SetWindowText(g_hStatus, text.c_str());

    int progress = g_job->progress;
    if (progress != g_progress) {
        g_progress = progress;
        InvalidateRect(g_hWnd, &g_progressRect, TRUE);
    }
}

static void StartRun() {
    if (g_job) return;
    if (g_mode == MODE_QUICK && g_scanReady)
        SetMode(MODE_COMPLETE);

    std::vector<int> categories = g_mode == MODE_CUSTOM ? CheckedCategories() : SystemCategories();
    if (categories.empty()) {
        SetWindowText(g_hStatus, L"Tick at least one item to clean.");
        return;
    }
    if (g_measureJob)
        g_measureJob->cancel = true;  // the checklist sizes aren't needed while this runs

    Job* job = new Job();
    PrepareJob(*job);
    job->scanOnly   = g_mode == MODE_QUICK;
    job->categories = categories;
    job->expected   = g_knownBytes;
    job->freeBefore = FreeSpaceOnSystemDrive();

    g_job = job;
    g_progress = 0;
    SetRunningUi(true);
    ShowChecklist(false);
    SetWindowText(g_hLog, L"");
    SetWindowText(g_hStatus, job->scanOnly ? L"Scanning..." : L"Cleaning...");

    if (job->scanOnly) {
        AppendLog(L"Scanning... nothing will be deleted.");
        AppendLog(L"Checking everything Complete Cleanup would remove:");
        AppendLog(L"");
    } else {
        AppendLog(L"Cleaning... please wait.");
    }
    if (!g_isAdmin)
        AppendLog(L"Note: not running as administrator, so system folders "
                  L"marked (admin) will be mostly skipped.");
    if (job->keepRecent) {
        for (int index : categories) {
            if (g_categories[index].flags & F_RECENT) {
                AppendLog(L"Temp files changed in the last 24 hours are kept (see Options).");
                break;
            }
        }
    }
    if (!g_exclusions.empty())
        AppendLog(L"Excluded folders are never touched (see Options > Exclusions).");

    HANDLE hThread = CreateThread(NULL, 0, JobThread, job, 0, NULL);
    if (hThread) {
        CloseHandle(hThread);
        SetTimer(g_hWnd, TIMER_PROGRESS, 150, NULL);
    } else {
        g_job = nullptr;
        delete job;
        SetRunningUi(false);
        AppendLog(L"Failed to start.");
        SetWindowText(g_hStatus, L"Failed to start.");
    }
}

static void FinishRun(const Job& job) {
    KillTimer(g_hWnd, TIMER_PROGRESS);

    if (job.scanOnly) {
        for (int i = 0; i < CATEGORY_COUNT; ++i)
            if (job.catBytes[i] >= 0)
                g_knownBytes[i] = job.catBytes[i];
    } else {
        for (int index : job.categories)
            g_knownBytes[index] = -1;
        g_sizesFresh = false;
    }

    std::wstring status = LogSummary(job);
    if (!job.scanOnly && g_mode == MODE_CUSTOM)
        AppendLog(L"Click Custom Cleanup to go back to your selection.");
    g_scanReady = job.scanOnly && !job.cancel && job.bytes > 0;
    SetRunningUi(false);
    SetWindowText(g_hStatus, status.c_str());
    InvalidateRect(g_hWnd, &g_progressRect, TRUE);
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
        L"Version " APP_VERSION L"\nby Ladro\n\n"
        L"Scans and cleans temp files, caches, logs,\n"
        L"crash dumps, and the Recycle Bin.\n\n"
        L"GitHub: <a href=\"https://github.com/ladroxd\">github.com/ladroxd</a>";
    tdc.pfCallback         = AboutCallback;
    TaskDialogIndirect(&tdc, NULL, NULL, NULL);
}

static void CheckForUpdates() {
    if (g_checkingUpdates) return;
    HANDLE hThread = CreateThread(NULL, 0, UpdateThread, NULL, 0, NULL);
    if (hThread) {
        g_checkingUpdates = true;
        CloseHandle(hThread);
    }
}

static void ShowUpdateResult(HWND hWnd, const UpdateInfo& info) {
    if (info.status == 200 && IsNewerVersion(info.tag, APP_VERSION)) {
        if (ShowMessage(hWnd, (L"OptiCore Cleaner " + info.tag + L" is available").c_str(),
                L"You have version " APP_VERSION L".", L"Open download page"))
            ShellExecute(hWnd, L"open", L"https://github.com/" GITHUB_REPO L"/releases/latest",
                NULL, NULL, SW_SHOWNORMAL);
    } else if (info.status == 200) {
        ShowMessage(hWnd, L"You're up to date", L"You have the latest version (" APP_VERSION L").");
    } else if (info.status == 404) {
        ShowMessage(hWnd, L"No releases yet", L"No versions have been published on GitHub yet.");
    } else {
        ShowMessage(hWnd, L"Couldn't check for updates",
            L"Check your internet connection and try again.", nullptr, TD_WARNING_ICON);
    }
}

static void ToggleSchedule(HWND hWnd) {
    if (g_scheduled < 0) g_scheduled = IsCleanupScheduled();
    if (g_scheduled) {
        if (DeleteScheduledTask()) {
            g_scheduled = 0;
            if (!g_job) SetWindowText(g_hStatus, L"Weekly cleanup turned off.");
        } else {
            ShowMessage(hWnd, L"Couldn't turn off the weekly cleanup",
                L"If it was set up as administrator, restart as administrator and try again.",
                nullptr, TD_WARNING_ICON);
        }
        return;
    }

    std::wstring content =
        L"Every Sunday at 12:00, Complete Cleanup runs in the background and shows a "
        L"notification when it's done. If your PC is off then, it runs the next time you start it.";
    if (!g_isAdmin)
        content += L"\n\nOptiCore isn't running as administrator, so the scheduled cleanup will skip "
                   L"system folders. Restart as administrator first for a full clean.";
    content += L"\n\nIf you move OptiCore Cleaner.exe, schedule it again.";
    if (!ShowMessage(hWnd, L"Clean this PC automatically every week?", content, L"Schedule"))
        return;

    if (CreateScheduledTask()) {
        g_scheduled = 1;
        if (!g_job) SetWindowText(g_hStatus, L"Weekly cleanup scheduled for Sundays at 12:00.");
    } else {
        ShowMessage(hWnd, L"Couldn't schedule the cleanup",
            L"Task Scheduler refused the task. Try again as administrator.", nullptr, TD_WARNING_ICON);
    }
}

static std::wstring GetLogText() {
    int len = GetWindowTextLength(g_hLog);
    std::wstring text(len + 1, L'\0');
    GetWindowText(g_hLog, &text[0], len + 1);
    text.resize(len);
    return text;
}

static void CopyLog(HWND hWnd) {
    std::wstring text = GetLogText();
    if (text.empty() || !OpenClipboard(hWnd)) return;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem) {
        memcpy(GlobalLock(mem), text.c_str(), bytes);
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem))
            GlobalFree(mem);
    }
    CloseClipboard();
    if (!g_job) SetWindowText(g_hStatus, L"Log copied to the clipboard.");
}

static void SaveLog(HWND hWnd) {
    IFileSaveDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return;
    COMDLG_FILTERSPEC types[] = { { L"Text file", L"*.txt" } };
    dlg->SetFileTypes(1, types);
    dlg->SetDefaultExtension(L"txt");
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[64];
    swprintf_s(name, L"OptiCore log %04u-%02u-%02u.txt", st.wYear, st.wMonth, st.wDay);
    dlg->SetFileName(name);

    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->Show(hWnd)) && SUCCEEDED(dlg->GetResult(&item))) {
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            if (WriteUtf8File(path, GetLogText())) {
                if (!g_job) SetWindowText(g_hStatus, (std::wstring(L"Log saved to ") + path).c_str());
            } else {
                ShowMessage(hWnd, L"Couldn't save the log", path, nullptr, TD_ERROR_ICON);
            }
            CoTaskMemFree(path);
        }
        item->Release();
    }
    dlg->Release();
}

// ---------------------------------------------------------------------------
// Exclusions window
// ---------------------------------------------------------------------------

static bool PickFolder(HWND owner, std::wstring& folder) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return false;
    DWORD options = 0;
    dlg->GetOptions(&options);
    dlg->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Choose a folder OptiCore should never clean");

    bool picked = false;
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->Show(owner)) && SUCCEEDED(dlg->GetResult(&item))) {
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            folder = path;
            picked = true;
            CoTaskMemFree(path);
        }
        item->Release();
    }
    dlg->Release();
    return picked;
}

static void FillExclusionList() {
    SendMessage(g_hExclList, LB_RESETCONTENT, 0, 0);
    for (const std::wstring& folder : g_exclusions)
        SendMessage(g_hExclList, LB_ADDSTRING, 0, (LPARAM)folder.c_str());
    EnableWindow(g_hExclRemove, FALSE);
}

static void ThemeExclusionsWindow() {
    if (!g_hExclWnd) return;
    ThemeTitleBar(g_hExclWnd);
    for (HWND hChild = GetWindow(g_hExclWnd, GW_CHILD); hChild; hChild = GetWindow(hChild, GW_HWNDNEXT))
        ThemeControl(hChild);
    RedrawWindow(g_hExclWnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

LRESULT CALLBACK ExclusionsWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HWND hLabel, hAdd, hClose;
    switch (msg) {
    case WM_CREATE:
        hLabel = CreateWindow(L"STATIC",
            L"OptiCore never touches these folders or anything inside them.",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 0, 0, hWnd, NULL, NULL, NULL);
        g_hExclList = CreateWindow(L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            0, 0, 0, 0, hWnd, (HMENU)IDC_EXCL_LIST, NULL, NULL);
        hAdd = CreateWindow(L"BUTTON", L"Add folder...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 0, 0, hWnd, (HMENU)IDC_EXCL_ADD, NULL, NULL);
        g_hExclRemove = CreateWindow(L"BUTTON", L"Remove", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 0, 0, hWnd, (HMENU)IDC_EXCL_REMOVE, NULL, NULL);
        hClose = CreateWindow(L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 0, 0, hWnd, (HMENU)IDCANCEL, NULL, NULL);
        for (HWND hChild : { hLabel, g_hExclList, hAdd, g_hExclRemove, hClose })
            SendMessage(hChild, WM_SETFONT, (WPARAM)(hChild == hLabel || hChild == g_hExclList ? g_hFontNormal : g_hFontBtn), TRUE);
        FillExclusionList();
        return 0;

    case WM_SIZE: {
        int cw = LOWORD(lParam), ch = HIWORD(lParam);
        const int margin = S(16), btnH = S(32), gap = S(8);
        int btnY = ch - margin - btnH;
        SetWindowPos(hLabel, NULL, margin, margin, cw - margin * 2, S(20), SWP_NOZORDER);
        SetWindowPos(g_hExclList, NULL, margin + 1, margin + S(28) + 1,
            cw - margin * 2 - 2, btnY - gap - margin - S(28) - 2, SWP_NOZORDER);
        SetWindowPos(hAdd, NULL, margin, btnY, S(120), btnH, SWP_NOZORDER);
        SetWindowPos(g_hExclRemove, NULL, margin + S(120) + gap, btnY, S(90), btnH, SWP_NOZORDER);
        SetWindowPos(hClose, NULL, cw - margin - S(90), btnY, S(90), btnH, SWP_NOZORDER);
        InvalidateRect(hWnd, NULL, TRUE);
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT frame;
        GetWindowRect(g_hExclList, &frame);
        MapWindowPoints(NULL, hWnd, (POINT*)&frame, 2);
        InflateRect(&frame, 1, 1);
        HBRUSH border = CreateSolidBrush(g_pal.border);
        FrameRect(hdc, &frame, border);
        DeleteObject(border);
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_EXCL_ADD: {
            std::wstring folder;
            if (!PickFolder(hWnd, folder)) break;
            if (folder.size() > 3 && folder.back() == L'\\') folder.pop_back();
            bool known = false;
            for (const std::wstring& existing : g_exclusions)
                if (_wcsicmp(existing.c_str(), folder.c_str()) == 0) known = true;
            if (!known) {
                g_exclusions.push_back(folder);
                WriteList(L"Exclusions", g_exclusions);
                FillExclusionList();
                InvalidateSizes();
            }
            break;
        }
        case IDC_EXCL_REMOVE: {
            int sel = (int)SendMessage(g_hExclList, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < (int)g_exclusions.size()) {
                g_exclusions.erase(g_exclusions.begin() + sel);
                WriteList(L"Exclusions", g_exclusions);
                FillExclusionList();
                InvalidateSizes();
            }
            break;
        }
        case IDC_EXCL_LIST:
            if (HIWORD(wParam) == LBN_SELCHANGE)
                EnableWindow(g_hExclRemove, SendMessage(g_hExclList, LB_GETCURSEL, 0, 0) != LB_ERR);
            break;
        case IDOK:
        case IDCANCEL:
            SendMessage(hWnd, WM_CLOSE, 0, 0);
            break;
        }
        return 0;

    case WM_CTLCOLORLISTBOX:
        SetBkColor((HDC)wParam, g_pal.panel);
        SetTextColor((HDC)wParam, g_pal.text);
        return (LRESULT)g_hPanelBrush;

    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)wParam, g_pal.bg);
        SetTextColor((HDC)wParam, g_pal.text);
        return (LRESULT)g_hBgBrush;

    case WM_CTLCOLORBTN:
        return (LRESULT)g_hBgBrush;

    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hWnd, &rc);
        FillRect((HDC)wParam, &rc, g_hBgBrush);
        return 1;
    }

    case WM_CLOSE:
        // Re-enable the owner first so it gets the focus back.
        EnableWindow(g_hWnd, TRUE);
        DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY:
        g_hExclWnd = NULL;
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

static void ShowExclusions(HWND owner) {
    if (g_hExclWnd) {
        SetForegroundWindow(g_hExclWnd);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSEX wc    = {};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = ExclusionsWndProc;
        wc.hInstance     = g_hInst;
        wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = L"OptiCoreExclusions";
        registered = RegisterClassEx(&wc) != 0;
    }

    RECT rc;
    GetWindowRect(owner, &rc);
    int w = S(480), h = S(360);
    g_hExclWnd = CreateWindowEx(WS_EX_DLGMODALFRAME, L"OptiCoreExclusions", L"Exclusions",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
        rc.left + (rc.right - rc.left - w) / 2, rc.top + (rc.bottom - rc.top - h) / 2, w, h,
        owner, NULL, g_hInst, NULL);
    if (!g_hExclWnd) return;
    ThemeExclusionsWindow();
    EnableWindow(owner, FALSE);
    ShowWindow(g_hExclWnd, SW_SHOW);
}

// ---------------------------------------------------------------------------
// Main window
// ---------------------------------------------------------------------------

static void UpdateMenuChecks() {
    CheckMenuItem(g_hOptionsMenu, IDM_KEEP_RECENT, g_keepRecent ? MF_CHECKED : MF_UNCHECKED);
    if (g_scheduled < 0) g_scheduled = IsCleanupScheduled();
    CheckMenuItem(g_hOptionsMenu, IDM_SCHEDULE, g_scheduled ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuRadioItem(g_hThemeMenu, IDM_THEME_SYSTEM, IDM_THEME_DARK,
        IDM_THEME_SYSTEM + g_themeSetting, MF_BYCOMMAND);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_hWnd = hWnd;
        g_dpi  = GetDpiForWindow(hWnd);

        HMENU hMenuBar = CreateMenu();
        HMENU hFile    = CreatePopupMenu();
        HMENU hEdit    = CreatePopupMenu();
        HMENU hHelp    = CreatePopupMenu();
        g_hOptionsMenu = CreatePopupMenu();
        g_hThemeMenu   = CreatePopupMenu();

        AppendMenu(hFile, MF_STRING, IDM_QUICK_SCAN, L"&Quick Scan");
        AppendMenu(hFile, MF_STRING, IDM_COMPLETE,   L"&Complete Cleanup");
        AppendMenu(hFile, MF_STRING, IDM_CUSTOM,     L"C&ustom Cleanup...");
        AppendMenu(hFile, MF_SEPARATOR, 0, NULL);
        AppendMenu(hFile, MF_STRING, IDM_FILE_EXIT,  L"E&xit");

        AppendMenu(hEdit, MF_STRING, IDM_COPY_LOG,   L"&Copy Log");
        AppendMenu(hEdit, MF_STRING, IDM_SAVE_LOG,   L"&Save Log...");
        AppendMenu(hEdit, MF_SEPARATOR, 0, NULL);
        AppendMenu(hEdit, MF_STRING, IDM_EDIT_CLEAR, L"C&lear Log");

        AppendMenu(g_hThemeMenu, MF_STRING, IDM_THEME_SYSTEM, L"&Follow Windows");
        AppendMenu(g_hThemeMenu, MF_STRING, IDM_THEME_LIGHT,  L"&Light");
        AppendMenu(g_hThemeMenu, MF_STRING, IDM_THEME_DARK,   L"&Dark");

        AppendMenu(g_hOptionsMenu, MF_STRING, IDM_KEEP_RECENT, L"&Keep temp files from the last 24 hours");
        AppendMenu(g_hOptionsMenu, MF_STRING, IDM_EXCLUSIONS,  L"&Exclusions...");
        AppendMenu(g_hOptionsMenu, MF_SEPARATOR, 0, NULL);
        AppendMenu(g_hOptionsMenu, MF_STRING, IDM_SCHEDULE,    L"&Schedule weekly cleanup");
        AppendMenu(g_hOptionsMenu, MF_SEPARATOR, 0, NULL);
        AppendMenu(g_hOptionsMenu, MF_POPUP, (UINT_PTR)g_hThemeMenu, L"&Theme");

        AppendMenu(hHelp, MF_STRING, IDM_CHECK_UPDATES, L"Check for &Updates");
        AppendMenu(hHelp, MF_SEPARATOR, 0, NULL);
        AppendMenu(hHelp, MF_STRING, IDM_HELP_ABOUT,    L"&About");

        AppendMenu(hMenuBar, MF_POPUP, (UINT_PTR)hFile, L"&File");
        AppendMenu(hMenuBar, MF_POPUP, (UINT_PTR)hEdit, L"&Edit");
        AppendMenu(hMenuBar, MF_POPUP, (UINT_PTR)g_hOptionsMenu, L"&Options");
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

        // The log and checklist have no border of their own: WM_PAINT draws a
        // 1px frame that matches the theme.
        g_hLog = CreateWindow(L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL |
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            0, 0, 0, 0, hWnd, (HMENU)IDC_LOG_EDIT, NULL, NULL);
        // Lift the default ~32K character cap so long cleans keep logging.
        SendMessage(g_hLog, EM_SETLIMITTEXT, 0, 0);

        g_hList = CreateWindow(WC_LISTVIEW, L"",
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
        ApplyTheme();
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
            if (g_job) {
                g_job->cancel = true;
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
            if (!g_job)
                RestartAsAdmin(hWnd);
            break;
        case IDM_FILE_EXIT:
            SendMessage(hWnd, WM_CLOSE, 0, 0);
            break;
        case IDM_COPY_LOG:
            CopyLog(hWnd);
            break;
        case IDM_SAVE_LOG:
            SaveLog(hWnd);
            break;
        case IDM_EDIT_CLEAR:
            SetWindowText(g_hLog, L"");
            break;
        case IDM_KEEP_RECENT:
            g_keepRecent = !g_keepRecent;
            WriteSetting(L"KeepRecentTemp", g_keepRecent);
            InvalidateSizes();
            break;
        case IDM_EXCLUSIONS:
            ShowExclusions(hWnd);
            break;
        case IDM_SCHEDULE:
            ToggleSchedule(hWnd);
            break;
        case IDM_THEME_SYSTEM:
        case IDM_THEME_LIGHT:
        case IDM_THEME_DARK:
            g_themeSetting = LOWORD(wParam) - IDM_THEME_SYSTEM;
            WriteSetting(L"Theme", g_themeSetting);
            ApplyTheme();
            break;
        case IDM_CHECK_UPDATES:
            CheckForUpdates();
            break;
        case IDM_HELP_ABOUT:
            ShowAbout(hWnd);
            break;
        }
        break;

    case WM_NOTIFY: {
        NMHDR* hdr = (NMHDR*)lParam;
        if (hdr->hwndFrom == g_hList && hdr->code == LVN_ITEMCHANGED && !g_listLoading) {
            NMLISTVIEW* nm = (NMLISTVIEW*)lParam;
            if ((nm->uChanged & LVIF_STATE) && ((nm->uNewState ^ nm->uOldState) & LVIS_STATEIMAGEMASK)) {
                SaveCustomSelection();
                UpdateSelectionStatus();
            }
        }
        break;
    }

    case WM_INITMENUPOPUP:
        if ((HMENU)wParam == g_hOptionsMenu)
            UpdateMenuChecks();
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

    case WM_MEASURED: {
        Job* job = (Job*)lParam;
        if (job != g_measureJob) break;
        int index = (int)wParam;
        g_knownBytes[index] = job->catBytes[index];
        SetSizeText(index);
        UpdateSelectionStatus();
        break;
    }

    case WM_JOB_DONE: {
        std::unique_ptr<Job> job((Job*)lParam);
        if (job.get() == g_measureJob) {
            g_measureJob = nullptr;
            if (!job->cancel) g_sizesFresh = true;
            if (g_remeasure) {
                g_remeasure = false;
                if (g_mode == MODE_CUSTOM && !g_job && !g_closeAfterClean) StartMeasure();
            }
            RefreshSizes();
            UpdateSelectionStatus();
        } else {
            g_job = nullptr;
            if (!g_closeAfterClean) FinishRun(*job);
        }
        if (g_closeAfterClean && !g_job && !g_measureJob)
            DestroyWindow(hWnd);
        break;
    }

    case WM_UPDATE_RESULT: {
        std::unique_ptr<UpdateInfo> info((UpdateInfo*)lParam);
        g_checkingUpdates = false;
        ShowUpdateResult(hWnd, *info);
        break;
    }

    case WM_TIMER:
        if (wParam == TIMER_PROGRESS)
            UpdateRunStatus();
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
        const int statusH = S(20), progressH = S(4);
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

        // status line just above the buttons, progress bar above that
        int statusY = btnY - S(6) - statusH;
        SetWindowPos(g_hStatus, NULL, margin, statusY, cw - margin * 2, statusH, SWP_NOZORDER);
        int progressY = statusY - S(8) - progressH;
        SetRect(&g_progressRect, margin, progressY, cw - margin, progressY + progressH);

        // content area (log or checklist) fills the space in between
        int contentY = modeY + modeH + S(10);
        int contentW = cw - margin * 2;
        int contentH = progressY - S(8) - contentY;
        SetRect(&g_logRect, margin, contentY, margin + contentW, contentY + contentH);
        SetWindowPos(g_hLog, NULL, margin, contentY, contentW, contentH, SWP_NOZORDER);

        int listH = contentH - selH - S(8);
        SetRect(&g_listRect, margin, contentY, margin + contentW, contentY + listH);
        SetWindowPos(g_hList, NULL, margin, contentY, contentW, listH, SWP_NOZORDER);
        SetWindowPos(g_hSelAll,  NULL, margin, contentY + listH + S(8), selW, selH, SWP_NOZORDER);
        SetWindowPos(g_hSelNone, NULL, margin + selW + S(8), contentY + listH + S(8), selW, selH, SWP_NOZORDER);

        int innerW = contentW - GetSystemMetricsForDpi(SM_CXVSCROLL, g_dpi) - S(4);
        int sizeW  = S(90);
        int restW  = innerW - sizeW;
        ListView_SetColumnWidth(g_hList, 0, restW * 45 / 100);
        ListView_SetColumnWidth(g_hList, 1, restW - restW * 45 / 100);
        ListView_SetColumnWidth(g_hList, 2, sizeW);
        break;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT frame = IsWindowVisible(g_hList) ? g_listRect : g_logRect;
        InflateRect(&frame, 1, 1);
        HBRUSH border = CreateSolidBrush(g_pal.border);
        FrameRect(hdc, &frame, border);
        DeleteObject(border);

        if (g_job) {
            FillRect(hdc, &g_progressRect, g_hHoverBrush);
            RECT bar = g_progressRect;
            bar.right = bar.left + MulDiv(bar.right - bar.left, g_progress, 1000);
            HBRUSH fill = CreateSolidBrush(g_pal.accent);
            FillRect(hdc, &bar, fill);
            DeleteObject(fill);
        }
        EndPaint(hWnd, &ps);
        return 0;
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

    case WM_SETTINGCHANGE:
        if (lParam && wcscmp((LPCWSTR)lParam, L"ImmersiveColorSet") == 0 && g_themeSetting == THEME_SYSTEM)
            ApplyTheme();
        break;

    case WM_UAHDRAWMENU:
        if (!g_dark) break;
        PaintMenuBarBackground(hWnd, ((UAHMENU*)lParam)->hdc);
        return TRUE;

    case WM_UAHDRAWMENUITEM:
        if (!g_dark) break;
        PaintMenuBarItem((UAHDRAWMENUITEM*)lParam);
        return TRUE;

    case WM_NCPAINT:
    case WM_NCACTIVATE: {
        LRESULT result = DefWindowProc(hWnd, msg, wParam, lParam);
        if (g_dark) PaintMenuBarLine(hWnd);
        return result;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        HWND hCtrl = (HWND)lParam;
        if (hCtrl == g_hLog) {  // read-only edits ask for static colors
            SetBkColor(hdc, g_pal.panel);
            SetTextColor(hdc, g_pal.text);
            return (LRESULT)g_hPanelBrush;
        }
        SetBkColor(hdc, g_pal.bg);
        if (hCtrl == g_hTitleLbl)
            SetTextColor(hdc, g_pal.accent);
        else if (hCtrl == g_hSubtitle)
            SetTextColor(hdc, g_pal.muted);
        else
            SetTextColor(hdc, g_pal.text);
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
        mmi->ptMinTrackSize.x = S(480);
        mmi->ptMinTrackSize.y = S(460);
        break;
    }

    case WM_CLOSE:
        if (g_job || g_measureJob) {
            // Stop the workers at the next file; WM_JOB_DONE destroys the window.
            if (g_job) g_job->cancel = true;
            if (g_measureJob) g_measureJob->cancel = true;
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
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    LoadSettings();

    // /auto runs Complete Cleanup without a window (used by the weekly task),
    // /auto /custom runs the saved Custom Cleanup selection, /quiet skips the notification.
    bool autoMode = false, autoCustom = false, notify = true;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        const wchar_t* arg = argv[i];
        if (*arg == L'-') ++arg;
        else if (*arg == L'/') ++arg;
        if (_wcsicmp(arg, L"auto") == 0)        autoMode = true;
        else if (_wcsicmp(arg, L"custom") == 0) autoCustom = true;
        else if (_wcsicmp(arg, L"quiet") == 0)  notify = false;
    }
    LocalFree(argv);
    if (autoMode) {
        int code = RunAuto(autoCustom, notify);
        CoUninitialize();
        return code;
    }

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEX wc       = {};
    wc.cbSize           = sizeof(WNDCLASSEX);
    wc.style            = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc      = WndProc;
    wc.hInstance        = hInstance;
    wc.hCursor          = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName    = L"TempBinCleanerV2";
    wc.hIcon            = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1));
    wc.hIconSm          = (HICON)LoadImage(hInstance, MAKEINTRESOURCE(IDI_ICON1), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    RegisterClassEx(&wc);

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    const int winW = S(620), winH = S(580);

    g_hWnd = CreateWindowEx(0, L"TempBinCleanerV2",
        g_isAdmin ? APP_NAME L" (Administrator)" : APP_NAME,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        (screenW - winW) / 2, (screenH - winH) / 2,
        winW, winH, NULL, NULL, hInstance, NULL);

    ShowWindow(g_hWnd, nCmdShow);
    UpdateWindow(g_hWnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (g_hExclWnd && IsDialogMessage(g_hExclWnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    DeleteObject(g_hBgBrush);
    DeleteObject(g_hPanelBrush);
    DeleteObject(g_hHoverBrush);
    CoUninitialize();
    return (int)msg.wParam;
}
