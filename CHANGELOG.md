# Changelog

All notable changes to SmartCleaner are listed here, newest first.

## [Unreleased]

### Added

### Changed

### Fixed

## [3.5.0] - 2026-11-01

### Added

- App caches in Custom Cleanup, listed only for apps found on your PC. Apps: Claude, Spotify, Notion, Slack, Microsoft Teams, Telegram, OBS Studio, Adobe media cache and VLC. Games: Steam (every library), Epic Games, EA, Ubisoft Connect, Battle.net, GOG Galaxy and Riot Client. A new Developer tools group: npm, npx, Yarn, pnpm, Bun, node-gyp, Electron, pip, Poetry, uv, Cargo, Go, NuGet, Gradle, Composer, Claude Code, VS Code, Cursor and Windsurf.
- An app's cache is skipped while that app is running, and the log and summary say so.

### Changed

- The app is named SmartCleaner again, and the download is `SmartCleaner.exe`. Settings carry over from 3.0.0. A weekly cleanup set up in 3.0.0 keeps running the old file until you schedule it again.
- Caches a tool trusts to be complete are renamed in one step before being deleted, so stopping a clean or a file in use can't leave one half-deleted.

### Fixed

- Files in paths over 260 characters are now found and deleted instead of being silently left behind.
- Group headers in the Custom Cleanup checklist are now readable in dark mode.

## [3.0.0] - 2026-10-28

First release as OptiCore Cleaner (previously Temp and Bin Cleaner V2).

### Added

- Quick Scan, Complete Cleanup and Custom Cleanup modes; Quick Scan never deletes and offers Clean Now when it finds something.
- Custom Cleanup checklist grouped into System junk, Browsers & apps, Gaming, Privacy and Advanced, with sizes measured in the background, a selected total, and a remembered selection.
- New cleaning targets: Windows Update cache, Delivery Optimization files, error reports, crash dumps, Windows log files, thumbnail cache, Store app temp files, all users' temp files, Chrome, Edge, Brave, Vivaldi, Opera and Firefox caches, Temporary Internet Files, Discord cache, DirectX, NVIDIA and AMD shader caches, the recent files list, and Windows component cleanup through DISM.
- Options menu: keep temp files from the last 24 hours, exclusions, weekly scheduled cleanup through Task Scheduler, and a Light, Dark or Follow Windows theme.
- `/auto` mode (with `/custom` and `/quiet`) that runs without a window, saves a log and shows a notification.
- Progress bar, Stop button, and free space before and after in the summary.
- Restart as Administrator button, Copy Log, Save Log, Check for Updates, and the version in About and the file properties.
- Per-monitor DPI awareness and modern Windows controls.

### Changed

- Cleaning runs in the background, so the window stays responsive.
- Browser and game caches are no longer part of Complete Cleanup.
- The Windows Update service is paused while its cache is cleaned, then restarted.

### Fixed

- Cleaned folders themselves are no longer deleted, only their contents.
- Junctions and symbolic links are never followed, so nothing outside a cleaned folder can be removed.
- Read-only files are now deleted instead of skipped.
- Long cleans no longer stop logging or flood the window with messages.

[Unreleased]: https://github.com/ladroxd/SmartCleaner/compare/v3.5.0...HEAD
[3.5.0]: https://github.com/ladroxd/SmartCleaner/compare/v3.0.0...v3.5.0
[3.0.0]: https://github.com/ladroxd/SmartCleaner/releases/tag/v3.0.0
