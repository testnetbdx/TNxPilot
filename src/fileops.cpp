// TNxPilot — fileops.cpp
// Win32-only implementation. Enumeration is deliberately thin:
// FindFirstFileExW with FIND_FIRST_EX_LARGE_FETCH, one WIN32_FIND_DATAW
// per item, no per-file extra stat calls — that is where Explorer-class
// tools lose their speed.
#include "fileops.h"

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <algorithm>

#pragma comment(lib, "shlwapi.lib")

namespace tnx {

static int64_t FtToI64(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return static_cast<int64_t>(u.QuadPart);
}

std::wstring FormatFileTime(int64_t ft) {
    FILETIME f;
    ULARGE_INTEGER u;
    u.QuadPart = static_cast<ULONGLONG>(ft);
    f.dwLowDateTime = u.LowPart;
    f.dwHighDateTime = u.HighPart;
    FILETIME local;
    if (!FileTimeToLocalFileTime(&f, &local)) return L"\u2014";
    SYSTEMTIME st;
    if (!FileTimeToSystemTime(&local, &st)) return L"\u2014";
    wchar_t buf[64];
    swprintf(buf, 64, L"%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth,
             st.wDay, st.wHour, st.wMinute);
    return buf;
}

int64_t FileTimeToUnix(int64_t ft) {
    // 11644473600 seconds between 1601 and 1970.
    return ft / 10000000 - 11644473600LL;
}

static std::wstring JoinPath(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    wchar_t last = dir.back();
    if (last == L'\\' || last == L'/') return dir + name;
    return dir + L"\\" + name;
}

std::vector<Entry> ListDirectory(const std::wstring& path, bool show_hidden) {
    std::vector<Entry> out;
    std::wstring pattern = JoinPath(path, L"*");
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd,
                                FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        bool hidden = (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0 ||
                      fd.cFileName[0] == L'.';
        if (hidden && !show_hidden) continue;
        Entry e;
        e.name = fd.cFileName;
        e.path = JoinPath(path, e.name);
        e.is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        ULARGE_INTEGER sz;
        sz.LowPart = fd.nFileSizeLow;
        sz.HighPart = fd.nFileSizeHigh;
        e.size = e.is_dir ? 0 : static_cast<int64_t>(sz.QuadPart);
        e.mtime = FtToI64(fd.ftLastWriteTime);
        e.hidden = hidden;
        const wchar_t* dot = wcsrchr(fd.cFileName, L'.');
        e.ext = dot ? ToLower(dot) : L"";
        out.push_back(std::move(e));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

void SortEntries(std::vector<Entry>& entries, const std::wstring& key,
                 bool reverse) {
    auto cmp = [&](const Entry& a, const Entry& b) {
        if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir; // dirs first
        if (key == L"date") return a.mtime < b.mtime;
        if (key == L"size") return a.size < b.size;
        if (key == L"type") {
            int t = _wcsicmp(a.TypeLabel().c_str(), b.TypeLabel().c_str());
            if (t != 0) return t < 0;
        }
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    };
    std::stable_sort(entries.begin(), entries.end(), cmp);
    if (reverse) std::reverse(entries.begin(), entries.end());
}

void SearchFlattened(const std::wstring& root, const std::wstring& query,
                     const std::wstring& extensions,
                     const std::function<void(const Entry&)>& on_result,
                     const std::atomic<bool>& should_stop,
                     int max_results) {
    static const wchar_t* kSkip[] = { L".git", L"__pycache__",
                                      L"node_modules", L".svn", L".idea" };
    std::wstring q = ToLower(query);
    std::vector<std::wstring> stack{ root };
    int found = 0;
    while (!stack.empty() && !should_stop && found < max_results) {
        std::wstring dir = std::move(stack.back());
        stack.pop_back();
        for (const Entry& e : ListDirectory(dir, false)) {
            if (should_stop) return;
            if (ToLower(e.name).find(q) != std::wstring::npos) {
                if (!extensions.empty() && !e.is_dir) {
                    if (extensions.find(e.ext) == std::wstring::npos) continue;
                }
                on_result(e);
                if (++found >= max_results) return;
            }
            if (e.is_dir) {
                bool skip = false;
                for (const wchar_t* s : kSkip)
                    if (_wcsicmp(e.name.c_str(), s) == 0) { skip = true; break; }
                if (!skip) stack.push_back(e.path);
            }
        }
    }
}

int64_t FolderSize(const std::wstring& path,
                   const std::atomic<bool>& should_stop) {
    int64_t total = 0;
    std::vector<std::wstring> stack{ path };
    while (!stack.empty() && !should_stop) {
        std::wstring dir = std::move(stack.back());
        stack.pop_back();
        for (const Entry& e : ListDirectory(dir, true)) {
            if (should_stop) return total;
            if (e.is_dir) stack.push_back(e.path);
            else total += e.size;
        }
    }
    return total;
}

static int64_t TreeByteTotal(const std::wstring& src) {
    if (GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES) return 0;
    if (!(GetFileAttributesW(src.c_str()) & FILE_ATTRIBUTE_DIRECTORY)) {
        WIN32_FILE_ATTRIBUTE_DATA ad{};
        if (GetFileAttributesExW(src.c_str(), GetFileExInfoStandard, &ad)) {
            ULARGE_INTEGER sz;
            sz.LowPart = ad.nFileSizeLow;
            sz.HighPart = ad.nFileSizeHigh;
            return static_cast<int64_t>(sz.QuadPart);
        }
        return 0;
    }
    int64_t total = 0;
    std::vector<std::wstring> stack{ src };
    while (!stack.empty()) {
        std::wstring dir = std::move(stack.back());
        stack.pop_back();
        for (const Entry& e : ListDirectory(dir, true)) {
            if (e.is_dir) stack.push_back(e.path);
            else total += e.size;
        }
    }
    return total;
}

static void WaitIfPaused(const std::atomic<bool>& should_stop,
                         const std::atomic<bool>& should_pause) {
    while (should_pause.load()) {
        if (should_stop.load()) throw CopyCancelled{};
        Sleep(100);
    }
}

static int64_t CopyFileChunked(
    const std::wstring& src, const std::wstring& dst, int64_t total,
    int64_t& done, const std::function<void(int64_t, int64_t)>& progress,
    const std::atomic<bool>& should_stop, const std::atomic<bool>& should_pause) {
    HANDLE in = CreateFileW(src.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (in == INVALID_HANDLE_VALUE) return done;
    HANDLE out = CreateFileW(dst.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) { CloseHandle(in); return done; }
    std::vector<char> buf(256 * 1024); // 256 KB chunks
    DWORD read = 0;
    while (ReadFile(in, buf.data(), static_cast<DWORD>(buf.size()), &read, nullptr) &&
           read > 0) {
        WaitIfPaused(should_stop, should_pause);
        if (should_stop.load()) { CloseHandle(in); CloseHandle(out); throw CopyCancelled{}; }
        DWORD written = 0;
        WriteFile(out, buf.data(), read, &written, nullptr);
        done += read;
        if (progress) progress(done, total);
    }
    CloseHandle(in);
    CloseHandle(out);
    // Preserve timestamps.
    HANDLE h = CreateFileW(dst.c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        WIN32_FILE_ATTRIBUTE_DATA ad{};
        if (GetFileAttributesExW(src.c_str(), GetFileExInfoStandard, &ad))
            SetFileTime(h, &ad.ftCreationTime, &ad.ftLastAccessTime,
                        &ad.ftLastWriteTime);
        CloseHandle(h);
    }
    return done;
}

int64_t CopyWithProgress(const std::wstring& src, const std::wstring& dst,
                         const std::function<void(int64_t, int64_t)>& progress,
                         const std::atomic<bool>& should_stop,
                         const std::atomic<bool>& should_pause) {
    int64_t total = TreeByteTotal(src);
    int64_t done = 0;
    DWORD attrs = GetFileAttributesW(src.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return 0;
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        CreateDirectoryW(dst.c_str(), nullptr);
        std::vector<std::wstring> stack_src{ src };
        std::vector<std::wstring> stack_dst{ dst };
        while (!stack_src.empty()) {
            if (should_stop.load()) throw CopyCancelled{};
            std::wstring sdir = std::move(stack_src.back()); stack_src.pop_back();
            std::wstring ddir = std::move(stack_dst.back()); stack_dst.pop_back();
            for (const Entry& e : ListDirectory(sdir, true)) {
                std::wstring s = JoinPath(sdir, e.name);
                std::wstring d = JoinPath(ddir, e.name);
                if (e.is_dir) {
                    CreateDirectoryW(d.c_str(), nullptr);
                    stack_src.push_back(s);
                    stack_dst.push_back(d);
                } else {
                    CopyFileChunked(s, d, total, done, progress,
                                    should_stop, should_pause);
                }
            }
        }
    } else {
        // Ensure destination directory exists.
        size_t slash = dst.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            CreateDirectoryW(dst.substr(0, slash).c_str(), nullptr);
        CopyFileChunked(src, dst, total, done, progress, should_stop, should_pause);
    }
    if (progress) progress(done, total);
    return done;
}

std::vector<std::wstring> SafeDelete(const std::vector<std::wstring>& paths) {
    std::vector<std::wstring> errors;
    for (const auto& p : paths) {
        DWORD attrs = GetFileAttributesW(p.c_str());
        bool ok = false;
        if (attrs != INVALID_FILE_ATTRIBUTES &&
            (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            // Recursive delete, deepest first.
            std::vector<std::wstring> stack{ p };
            std::vector<std::wstring> dirs;
            while (!stack.empty()) {
                std::wstring d = std::move(stack.back()); stack.pop_back();
                dirs.push_back(d);
                for (const Entry& e : ListDirectory(d, true)) {
                    if (e.is_dir) stack.push_back(e.path);
                    else DeleteFileW(e.path.c_str());
                }
            }
            ok = true;
            for (auto it = dirs.rbegin(); it != dirs.rend(); ++it)
                if (!RemoveDirectoryW(it->c_str())) ok = false;
        } else {
            ok = DeleteFileW(p.c_str()) != 0;
        }
        if (!ok) errors.push_back(p + L": delete failed");
    }
    return errors;
}

std::vector<DriveInfo> GetDrives() {
    std::vector<DriveInfo> out;
    wchar_t buf[256] = {};
    DWORD len = GetLogicalDriveStringsW(256, buf);
    for (wchar_t* p = buf; p < buf + len && *p; p += wcslen(p) + 1) {
        DriveInfo d;
        d.path = p;
        wchar_t label[MAX_PATH] = {};
        GetVolumeInformationW(p, label, MAX_PATH, nullptr, nullptr, nullptr,
                              nullptr, 0);
        d.label = label[0] ? label : L"Local Disk";
        ULARGE_INTEGER free_avail, total, free_total;
        if (GetDiskFreeSpaceExW(p, &free_avail, &total, &free_total)) {
            d.total = static_cast<int64_t>(total.QuadPart);
            d.free = static_cast<int64_t>(free_total.QuadPart);
        }
        out.push_back(std::move(d));
    }
    return out;
}

std::vector<std::pair<std::wstring, std::wstring>> SystemPaths() {
    std::vector<std::pair<std::wstring, std::wstring>> out;
    wchar_t home[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, home))) {
        const wchar_t* names[] = { L"Desktop", L"Documents", L"Downloads",
                                   L"Pictures", L"Music", L"Videos" };
        const int csidl[] = { CSIDL_DESKTOP, CSIDL_MYDOCUMENTS, -1 /*Downloads*/,
                              CSIDL_MYPICTURES, CSIDL_MYMUSIC, CSIDL_MYVIDEO };
        for (int i = 0; i < 6; ++i) {
            wchar_t path[MAX_PATH] = {};
            if (csidl[i] == -1) {
                wcscpy_s(path, home);
                wcscat_s(path, L"\\Downloads");
            } else if (FAILED(SHGetFolderPathW(nullptr, csidl[i], nullptr, 0, path))) {
                continue;
            }
            if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
                out.emplace_back(names[i], path);
        }
        out.emplace_back(L"Home", home);
    }
    return out;
}

} // namespace tnx
