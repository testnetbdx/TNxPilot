// TNxPilot — dialogs.h
// Modal dialogs built in code (no .rc resources): Batch Rename,
// Command Palette, Settings, Copy Progress. All Win32, all offline.
#pragma once

#include <functional>
#include <string>
#include <vector>
#include <windows.h>

#include "config.h"
#include "fileops.h"

namespace tnx {

struct Command {
    std::wstring name;
    std::wstring hotkey;
    std::function<void()> run;
};

// Returns true if the user ran a command.
bool ShowCommandPalette(HWND owner, const std::vector<Command>& commands);

// Applies renames on disk. Returns true when anything was renamed.
bool ShowBatchRename(HWND owner, const std::vector<Entry>& entries);

// Returns true when settings changed (caller re-applies theme).
bool ShowSettings(HWND owner, Config& config);

// Copy/move job: spawns a worker thread, shows a progress window with
// speed + Pause/Cancel. Returns when the job finishes or is cancelled.
struct CopyJob {
    std::vector<std::wstring> sources;
    std::wstring dest_dir;
    bool move = false;
};
void RunCopyJob(HWND owner, const CopyJob& job);

// Small helpers shared by dialogs.
HWND CreateLabel(HWND parent, int x, int y, int w, const wchar_t* text);
HWND CreateButton(HWND parent, int x, int y, int w, const wchar_t* text, int id);
HWND CreateEdit(HWND parent, int x, int y, int w, const wchar_t* text = L"");
std::wstring GetEditText(HWND edit);
void SetEditText(HWND edit, const std::wstring& text);

} // namespace tnx
