# OptiCore Cleaner

A small, native Windows app that finds and frees wasted disk space: temp files, update leftovers, crash dumps, logs, caches and the Recycle Bin. It shows exactly what it removed and how much space you got back.

Built with plain C++ and the Win32 API. There's no installer, no runtime and no background service: just a single `.exe`.

## Three ways to clean

| Mode | What it does |
| --- | --- |
| **Quick Scan** | Checks everything Complete Cleanup would remove and shows how much space you can free, per item. **Nothing is deleted.** |
| **Complete Cleanup** | One click removes all **system junk** (the first table below). Browser caches, game shader caches and privacy items are left alone. |
| **Custom Cleanup** | A checklist of **everything** OptiCore can clean, including browser and game caches. Tick what you want, then click **Clean Selected**. |

## What it can clean

### System junk (Quick Scan and Complete Cleanup)

| Item | Location | Admin? |
| --- | --- | --- |
| Temporary files | `%TEMP%` | |
| Windows temporary files | `%WINDIR%\Temp` | ✔ |
| Prefetch | `%WINDIR%\Prefetch` | ✔ |
| Windows Update cache | `%WINDIR%\SoftwareDistribution\Download` | ✔ |
| Delivery Optimization files | `%WINDIR%\ServiceProfiles\NetworkService\...\DeliveryOptimization\Cache` | ✔ |
| Error reports | `WER\ReportArchive` and `WER\ReportQueue` (system and user) | |
| Crash dumps | `%WINDIR%\Minidump`, `%WINDIR%\MEMORY.DMP`, `%LOCALAPPDATA%\CrashDumps` | |
| Windows log files | `*.log` files in `%WINDIR%\Logs` (folders are kept) | ✔ |
| Thumbnail cache | `thumbcache_*.db` in `%LOCALAPPDATA%\Microsoft\Windows\Explorer` | |
| Store app temp files | `%LOCALAPPDATA%\Packages\*\AC\Temp` | |
| Recycle Bin | All drives | |

### Browsers & apps (Custom Cleanup only)

Cache files only. Your history, passwords, cookies and logins are never touched. Close the browser first for a full clean, because files that are in use are skipped.

| Item | What's cleared |
| --- | --- |
| Google Chrome, Microsoft Edge, Brave, Vivaldi | `Cache`, `Code Cache` and `GPUCache` for every profile |
| Opera / Opera GX | `Cache` |
| Firefox | `cache2` for every profile |
| Temporary Internet Files | `%LOCALAPPDATA%\Microsoft\Windows\INetCache` |
| Discord | `Cache`, `Code Cache` and `GPUCache` |

### Gaming (Custom Cleanup only)

Games stutter and load slower the first time they're launched while these caches are rebuilt, so they're never cleaned unless you tick them.

| Item | What's cleared |
| --- | --- |
| DirectX shader cache | `%LOCALAPPDATA%\D3DSCache` |
| NVIDIA shader cache | `%LOCALAPPDATA%\NVIDIA\DXCache` and `GLCache` |
| AMD shader cache | `%LOCALAPPDATA%\AMD\DxCache`, `DxcCache`, `GLCache` and `VkCache` |

### Privacy (Custom Cleanup only)

| Item | What's cleared |
| --- | --- |
| Recent files list | Shortcuts in `%APPDATA%\Microsoft\Windows\Recent` (pinned jump-list items are kept) |

## Safety

- The folders being cleaned are never deleted, only their contents.
- Junctions and symbolic links are never followed, so nothing outside a cleaned folder can be removed.
- Drive roots and relative paths are refused.
- Files that are in use are skipped and counted in the summary.
- The Windows Update service is paused while its cache is cleared, then restarted. If an update is in progress, that item is skipped.
- You can click **Stop** at any time. Closing the window mid-clean also stops safely at the next file.

> **Note:** Windows rebuilds Prefetch and the thumbnail cache automatically. Clearing them is safe, but the next boot and the first launch of your apps may be a little slower while they're rebuilt.

## Usage

1. Run `OptiCore Cleaner.exe`.
2. For a full clean of system folders, click **Restart as Administrator** (or right-click the exe and choose *Run as administrator*). Items marked *(admin)* are mostly skipped without it.
3. Pick a mode at the top and click the button at the bottom (**Scan**, **Clean Now** or **Clean Selected**).

## Building from source

Requirements:

- Windows 10 (version 1607 or later) or Windows 11
- Visual Studio 2022 (or newer) with the **Desktop development with C++** workload

Steps:

1. Open `Temp and Bin Cleaner V2.sln`.
2. Select the **Release** configuration and the **x64** platform.
3. Build the solution (`Ctrl+Shift+B`).

The executable is written to `x64\Release\OptiCore Cleaner.exe`.

From a Developer Command Prompt you can also run:

```
msbuild "Temp and Bin Cleaner V2.sln" /p:Configuration=Release /p:Platform=x64
```

## Project layout

| File | Purpose |
| --- | --- |
| `local.cpp` | The whole app: what can be cleaned (`g_categories`), the cleaning engine and the window |
| `app.manifest` | Enables modern controls and per-monitor DPI awareness |
| `Resource.rc` / `resource.h` | App icon resource |
| `Projects Icons/` | Icon source file |

To add something new to clean, add an entry to `g_categories` in `local.cpp`. It shows up in Custom Cleanup automatically, and in Quick Scan and Complete Cleanup too if its group is `GROUP_SYSTEM`.

## Credits

Made by **Ladro**: [github.com/ladroxd](https://github.com/ladroxd)

App icon: "data cleaning" by [Freepik](https://www.freepik.com) from [Flaticon](https://www.flaticon.com).
