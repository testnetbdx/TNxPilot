// TNxPilot — fileops.h
// Windows file-system operations. Enumeration uses FindFirstFileExW /
// FindNextFileW on a worker thread; the UI thread never blocks on disk.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "util.h"

namespace tnx {

struct Entry {
    std::wstring name;
    std::wstring path;   // full path
    bool is_dir = false;
    int64_t size = 0;    // files only; dirs report 0 until sized
    int64_t mtime = 0;   // FILETIME as 100ns ticks since 1601
    std::wstring ext;    // lower-case, with dot
    bool hidden = false;

    std::wstring TypeLabel() const {
        if (is_dir) return L"Folder";
        if (ext.empty()) return L"File";
        std::wstring up = ToLower(ext.substr(1));
        for (auto& c : up) c = static_cast<wchar_t>(towupper(c));
        return up + L" File";
    }
};

// One non-recursive listing. Never throws; unreadable dirs return empty.
std::vector<Entry> ListDirectory(const std::wstring& path, bool show_hidden);

// Directories first, then key (name/date/size/type). Stable, no copies
// of strings beyond the vector itself (move semantics throughout).
void SortEntries(std::vector<Entry>& entries, const std::wstring& key,
                 bool reverse);

// Recursive flattened name search. Streams results via callback so the
// UI can show matches as they arrive; stops when should_stop() is true.
void SearchFlattened(const std::wstring& root, const std::wstring& query,
                     const std::wstring& extensions,
                     const std::function<void(const Entry&)>& on_result,
                     const std::atomic<bool>& should_stop,
                     int max_results = 1000);

// Recursive folder byte total (background thread). Stops early on cancel.
int64_t FolderSize(const std::wstring& path,
                   const std::atomic<bool>& should_stop);

// Copy one file or a whole tree. progress(done,total) is throttled by
// the caller. Throws CopyCancelled on cancel; returns bytes copied.
struct CopyCancelled {};
int64_t CopyWithProgress(const std::wstring& src, const std::wstring& dst,
                         const std::function<void(int64_t, int64_t)>& progress,
                         const std::atomic<bool>& should_stop,
                         const std::atomic<bool>& should_pause);

// Permanent delete (no recycle bin). Returns error strings, empty = ok.
std::vector<std::wstring> SafeDelete(const std::vector<std::wstring>& paths);

// Drives: {path, label, total, free}.
struct DriveInfo { std::wstring path, label; int64_t total = 0, free = 0; };
std::vector<DriveInfo> GetDrives();

// Common locations that exist on this machine.
std::vector<std::pair<std::wstring, std::wstring>> SystemPaths();

// FILETIME (100ns since 1601) -> "YYYY-MM-DD HH:MM" local time.
std::wstring FormatFileTime(int64_t ft);
// FILETIME -> unix seconds (for the logic test path).
int64_t FileTimeToUnix(int64_t ft);

} // namespace tnx
