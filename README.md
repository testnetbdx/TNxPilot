# TNxPilot v1.0.0 — Native Windows File Explorer (C++17 / Win32)

**Private use only. Never publish.** Original code, written from scratch —
inspired by the *idea* of a fast file manager, but no copied code, assets,
name or branding from any other product.

TNxPilot is a fast, fully offline Windows file explorer: no network calls,
no telemetry, no account, no cloud. All settings live in plain JSON files
next to the app (`config.json`, `session.json`).

## Why native C++ (not Python / Qt / Electron)

Speed was the requirement, so the whole app is native:

- **Single small EXE, static CRT (`/MT`)** — no Python, no Qt, no .NET,
  no Electron, no runtime to install. Startup is instant.
- **Virtual ListView (`LVS_OWNERDATA`)** — only visible rows are drawn,
  so a folder with 100,000 files still scrolls smoothly.
- **Threaded enumeration** (`FindFirstFileExW` / `FindNextFileW`) — the UI
  thread never blocks on disk; folder size is calculated on a background
  thread; search streams results as they arrive.
- **System image list + lazy icon cache** (`SHGetFileInfoW`).
- **No unnecessary copies** — `std::vector` + `reserve`, move semantics,
  `wstring_view` in hot paths.
- v1 uses standard Win32 controls (ListView, TreeView, Tab control) with
  double-buffering and a dark theme. A custom Direct2D renderer is a
  v2 idea — v1 prioritises stability + speed.

## Features (v1.0, all implemented)

- **Dark / light themes**, font-size and icon-size settings
- **Sidebar** — This PC drives with usage bars, Places, Bookmarks, Recents
- **Tabs** (Ctrl+T / Ctrl+W), **dual-panel split view** (Ctrl+D),
  session restore (tabs reopen on launch)
- **Views** — Details / List / Icons / Tree, sorting by name/date/size/type,
  icon-size slider, filter box with fuzzy search
- **GoTo bar** (Ctrl+L) with filesystem autocomplete (SHAutoComplete)
- **Flattened search** across subfolders, streaming results
- **File operations** — copy / move / delete / rename (F2), new folder/file,
  cut / copy / paste, drag-drop between panels + drop files from Explorer,
  copy progress dialog with speed, pause and cancel
- **Batch Rename** — find/replace, prefix/suffix, numbering, date,
  live preview, duplicate-name warning
- **Command Palette** (Ctrl+Shift+P) — searchable list of every command
- **Inspector** — text preview, image preview (GDI+: PNG/JPG/GIF/BMP/WebP),
  folder size, file properties
- **Context menu** — Open, Open in Terminal (PowerShell), Copy as Path,
  Properties, Cut/Copy/Paste/Delete/Rename, show-hidden toggle
- **Fully offline** — nothing leaves your PC

## Build on Windows

Requirements (one-time): Visual Studio Build Tools 2022 (MSVC) and
Inno Setup.

From an **x64 Native Tools Command Prompt**:

```bat
build.bat
```

What it does:
1. Compiles with `cl /O2 /MT /std:c++17` → `TNxPilot.exe`
   (single small EXE, static CRT — nothing else to install)
2. Compiles `installer-output\TNxPilot-Setup-v1.0.exe` with Inno Setup

Install with the setup EXE: Next → Next → Finish. It installs to
`Program Files\TNxPilot`, adds Start Menu + optional Desktop icon, and an
uninstaller. **No internet is needed at install time** — the EXE is
bundled inside the setup EXE.

Logic test (runs on Linux too, covers the platform-independent parts):

```bash
g++ -std=c++17 -I src tests/test_logic.cpp src/config.cpp -o test_logic
./test_logic
```

## SmartScreen note

The installer is unsigned, so Windows SmartScreen may warn on first run.
Click **More info → Run anyway**. This is normal for a private, unsigned
build. Never download a "TNxPilot" build from anywhere else.

## Honest limits (v1.0)

- **Windows-only.** The EXE + installer can only be built and tested on a
  real Windows PC with MSVC. On this Linux VM only the platform-independent
  logic (`util.h`, `config.cpp`) was compiled and tested — the Win32 UI
  code was manually reviewed, not compiled. No fake "build passed".
- Dual panel in v1 shows two views of the **same** tab folder
  (independent per-pane folders are a v2 item).
- Copy/move is single-stream (fast enough for normal use, not
  multi-threaded like a dedicated copier). Copy speed is limited by
  your disk, like every file manager.
- No FTP / cloud / archive-browsing yet. No ARM build yet (x64 only).
- Ctrl+C/X/V inside the GoTo/filter boxes goes to the edit box, not the
  file clipboard (handled explicitly).

## Project layout

```
src/
  main.cpp      WinMain, window, tabs, virtual ListView, sidebar,
                inspector, menus, drag-drop, accelerators
  fileops.h/.cpp  FindFirstFileExW enumeration, sort, flattened
                search, folder size, chunked copy (pause/cancel)
  config.h/.cpp   config.json / session.json (plain JSON, local only)
  dialogs.h/.cpp  Batch Rename, Command Palette, Settings,
                Copy Progress (code-built dialogs, no .rc)
  util.h          platform-independent: FormatSize, fuzzy match,
                batch-rename rules (unit-tested on Linux)
tests/
  test_logic.cpp  headless logic test (g++, Linux OK)
build.bat         Windows build: cl.exe (/O2 /MT) → Inno Setup
installer.iss     Inno Setup offline installer script
.github/workflows/build-windows.yml   MSVC build workflow (manual only)
```

---

# TNxPilot v1.0.0 — নেটিভ Windows ফাইল এক্সপ্লোরার (C++17 / Win32)

**শুধু ব্যক্তিগত ব্যবহারের জন্য। কোথাও পাবলিশ করবেন না।**

## Windows-এ বিল্ড করবেন যেভাবে

1. Visual Studio Build Tools 2022 আর Inno Setup ইনস্টল করুন (একবারই লাগবে)।
2. **x64 Native Tools Command Prompt** খুলে `build.bat` চালান।
3. তৈরি হবে:
   - `TNxPilot.exe` — একটাই ছোট EXE, আর কিছু ইনস্টল লাগবে না
   - `installer-output\TNxPilot-Setup-v1.0.exe` — অফলাইন ইনস্টলার
4. ইনস্টলার চালান: Next → Next → Finish। ইনস্টলের সময় ইন্টারনেট লাগবে না।

SmartScreen সতর্ক করলে **More info → Run anyway** চাপুন — প্রাইভেট,
আনসাইনড বিল্ডের ক্ষেত্রে এটা স্বাভাবিক।
