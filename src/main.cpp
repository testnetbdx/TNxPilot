// TNxPilot v1.0.0 — native C++17 / Win32 file explorer (private use).
// Speed-first, File-Pilot-style architecture, original code:
//  * single small EXE, static CRT (/MT), no runtime dependency
//  * virtual ListView (LVS_OWNERDATA) — only visible rows are drawn,
//    so a 100k-file folder still scrolls smoothly
//  * directory enumeration on worker threads (FindFirstFileExW);
//    the UI thread never blocks on disk
//  * SHGetFileInfoW system image list + lazy icon cache
//  * threaded fuzzy/flattened search with streaming results
//  * no network, no telemetry, no account — fully offline
//
// Build on Windows:  build.bat   (cl.exe, MSVC)
// This file intentionally uses only Win32 + GDI+ (image preview).
#pragma comment(linker, "\"/manifestdependency:type='win32' \
    name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
    processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <gdiplus.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "config.h"
#include "dialogs.h"
#include "fileops.h"
#include "util.h"

using namespace tnx;
using namespace Gdiplus;

// ------------------------------------------------------------------ ids
enum : int {
    IDC_TAB = 100, IDC_LIST_A, IDC_LIST_B, IDC_TREE, IDC_SIDEBAR,
    IDC_INSPECTOR, IDC_GOTO, IDC_FILTER, IDC_VIEWBOX, IDC_STATUS,
    IDC_TOOLBAR, IDC_SPLIT_L, IDC_SPLIT_R,
    IDM_NEW_TAB = 1000, IDM_CLOSE_TAB, IDM_NEW_FOLDER, IDM_NEW_FILE,
    IDM_EXIT, IDM_DUAL, IDM_HIDDEN, IDM_THEME, IDM_GOTO, IDM_SEARCH,
    IDM_PALETTE, IDM_BATCH_RENAME, IDM_SETTINGS, IDM_ABOUT,
    IDM_BACK, IDM_FWD, IDM_UP, IDM_REFRESH,
    IDM_OPEN, IDM_TERMINAL, IDM_COPY_PATH, IDM_CUT, IDM_COPY,
    IDM_PASTE, IDM_DELETE, IDM_RENAME, IDM_PROPS, IDM_BOOKMARK,
    IDM_VIEW_DETAILS, IDM_VIEW_LIST, IDM_VIEW_ICONS, IDM_VIEW_TREE,
};

constexpr UINT WM_LIST_DONE = WM_USER + 10;
constexpr UINT WM_SIZE_DONE = WM_USER + 11;
constexpr UINT WM_SEARCH_HIT = WM_USER + 12;
constexpr UINT WM_SEARCH_DONE = WM_USER + 13;

// ------------------------------------------------------------------ state
struct TabState {
    std::wstring path;
    std::vector<std::wstring> history;
    size_t hist_index = 0;
    std::vector<Entry> entries;      // current view (sorted, filtered)
    std::wstring filter;             // fuzzy filter text
    std::wstring sort_key = L"name";
    bool sort_reverse = false;
    std::wstring view_mode = L"details";
    bool searching = false;          // showing flattened search results
    // Shared sequence: the list thread holds a copy, so deleting a tab
    // can never leave a thread dereferencing freed memory.
    std::shared_ptr<std::atomic<uint64_t>> seq =
        std::make_shared<std::atomic<uint64_t>>(0);
};

struct App {
    HINSTANCE hinst = nullptr;
    HWND hwnd = nullptr, tab = nullptr;
    HWND list_a = nullptr, list_b = nullptr; // dual panels
    HWND tree = nullptr, sidebar = nullptr, inspector = nullptr;
    HWND goto_edit = nullptr, filter_edit = nullptr, view_box = nullptr;
    HWND status = nullptr, split_l = nullptr, split_r = nullptr;
    HWND insp_title = nullptr, insp_info = nullptr, insp_text = nullptr;
    Config config{ DefaultBaseDir() };
    std::vector<std::unique_ptr<TabState>> tabs;
    int active_tab = 0;
    bool dual = false;
    bool dark = true;
    int sidebar_w = 230, inspector_w = 300;
    HIMAGELIST img_small = nullptr, img_large = nullptr;
    std::map<std::wstring, int> icon_cache; // ext -> image-list index
    std::vector<std::wstring> clip_paths;
    bool clip_cut = false;
    std::atomic<bool> stop_list{ false }, stop_size{ false }, stop_search{ false };
    HANDLE list_thread = nullptr, size_thread = nullptr, search_thread = nullptr;
    std::unique_ptr<Bitmap> insp_bitmap; // selected image preview (GDI+)
    std::vector<std::wstring> drag_paths; // internal drag source
    bool dragging = false;
};

static App g_app;

// Forward declarations for windows created in WndProc (defined later).
static HWND g_image_view = nullptr;
static HWND g_center_tree = nullptr;

// ------------------------------------------------------------------ helpers
static TabState* ActiveTab() {
    if (g_app.active_tab < 0 ||
        g_app.active_tab >= static_cast<int>(g_app.tabs.size())) return nullptr;
    return g_app.tabs[g_app.active_tab].get();
}

static HWND ActiveList() {
    // The focused pane is the active one; default to the left pane.
    HWND focus = GetFocus();
    if (g_app.dual && focus == g_app.list_b) return g_app.list_b;
    return g_app.list_a;
}

static std::wstring ActivePath() {
    TabState* t = ActiveTab();
    return t ? t->path : L"";
}

static void SetStatus(const std::wstring& text) {
    SendMessageW(g_app.status, SB_SETTEXTW, 0, (LPARAM)text.c_str());
}

// ------------------------------------------------------------------ icons
static int IconIndexFor(const Entry& e) {
    std::wstring key = e.is_dir ? L"<dir>" : e.ext;
    auto it = g_app.icon_cache.find(key);
    if (it != g_app.icon_cache.end()) return it->second;
    SHFILEINFOW sfi{};
    DWORD flags = SHGFI_SYSICONINDEX | SHGFI_SMALLICON |
                  (e.is_dir ? SHGFI_USEFILEATTRIBUTES : 0);
    DWORD attrs = e.is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    SHGetFileInfoW(e.path.c_str(), attrs, &sfi, sizeof(sfi), flags);
    int idx = sfi.iIcon;
    g_app.icon_cache[key] = idx;
    return idx;
}

static void InitImageLists() {
    SHFILEINFOW sfi{};
    HIMAGELIST small = (HIMAGELIST)SHGetFileInfoW(
        L"C:\\", 0, &sfi, sizeof(sfi),
        SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
    HIMAGELIST large = (HIMAGELIST)SHGetFileInfoW(
        L"C:\\", 0, &sfi, sizeof(sfi),
        SHGFI_SYSICONINDEX | SHGFI_LARGEICON);
    g_app.img_small = small;
    g_app.img_large = large;
}

// ------------------------------------------------------------------ listing
struct ListResult {
    uint64_t seq;
    std::vector<Entry> entries;
    std::wstring path;
};

static DWORD WINAPI ListThreadProc(LPVOID param) {
    struct Job {
        std::shared_ptr<std::atomic<uint64_t>> seq;
        uint64_t my_seq;
        std::wstring path, filter, sort_key;
        bool sort_reverse, show_hidden;
    };
    std::unique_ptr<Job> job(static_cast<Job*>(param));
    auto entries = ListDirectory(job->path, job->show_hidden);
    SortEntries(entries, job->sort_key, job->sort_reverse);
    if (!job->filter.empty()) {
        std::vector<std::pair<int, Entry>> scored;
        for (auto& e : entries) {
            int s = FuzzyScore(job->filter, e.name);
            if (s >= 0) scored.emplace_back(s, std::move(e));
        }
        std::sort(scored.begin(), scored.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        std::vector<Entry> filtered;
        for (auto& [s, e] : scored) filtered.push_back(std::move(e));
        entries = std::move(filtered);
    }
    if (job->my_seq != job->seq->load()) return 0; // superseded
    auto* res = new ListResult{ job->my_seq, std::move(entries), job->path };
    PostMessageW(g_app.hwnd, WM_LIST_DONE, 0, reinterpret_cast<LPARAM>(res));
    return 0;
}

static void StartListing(TabState* tab) {
    uint64_t my_seq = tab->seq->fetch_add(1) + 1;
    struct Job {
        std::shared_ptr<std::atomic<uint64_t>> seq;
        uint64_t my_seq;
        std::wstring path, filter, sort_key;
        bool sort_reverse, show_hidden;
    };
    auto* job = new Job{ tab->seq, my_seq, tab->path, tab->filter,
                         tab->sort_key, tab->sort_reverse,
                         g_app.config.settings.show_hidden };
    if (g_app.list_thread) {
        CloseHandle(g_app.list_thread);
        g_app.list_thread = nullptr;
    }
    g_app.list_thread = CreateThread(nullptr, 0, ListThreadProc, job, 0, nullptr);
}

static void Navigate(TabState* tab, const std::wstring& path, bool record = true) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES ||
        !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        MessageBoxW(g_app.hwnd, (L"Folder not found:\n" + path).c_str(),
                    L"TNxPilot", MB_ICONWARNING);
        return;
    }
    if (record) {
        tab->history.resize(tab->hist_index + 1);
        tab->history.push_back(path);
        tab->hist_index = tab->history.size() - 1;
    }
    tab->path = path;
    tab->searching = false;
    g_app.config.PushRecent(path);
    SetWindowTextW(g_app.goto_edit, path.c_str());
    StartListing(tab);
}

static void RefreshCurrentView() {
    TabState* tab = ActiveTab();
    if (!tab) return;
    // Re-apply the list to both panes (virtual ListViews read the vector).
    ListView_SetItemCountEx(g_app.list_a, static_cast<int>(tab->entries.size()),
                            LVSICF_NOSCROLL);
    if (g_app.dual && g_app.list_b)
        ListView_SetItemCountEx(g_app.list_b, static_cast<int>(tab->entries.size()),
                                LVSICF_NOSCROLL);
    InvalidateRect(g_app.list_a, nullptr, TRUE);
    if (g_app.dual && g_app.list_b) InvalidateRect(g_app.list_b, nullptr, TRUE);
    int64_t total = 0;
    for (const auto& e : tab->entries) if (!e.is_dir) total += e.size;
    wchar_t buf[512];
    swprintf(buf, 512, L"%ls  •  %zu item(s)  •  %ls",
             tab->path.c_str(), tab->entries.size(), FormatSize(total).c_str());
    SetStatus(buf);
    int idx = g_app.active_tab;
    std::wstring title = tab->path;
    size_t slash = title.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash + 1 < title.size())
        title = title.substr(slash + 1);
    TCITEMW ti{ TCIF_TEXT };
    ti.pszText = title.data();
    TabCtrl_SetItem(g_app.tab, idx, &ti);
}

// Folder size on a background thread (Inspector).
struct SizeJob { std::wstring path; };
static DWORD WINAPI SizeThreadProc(LPVOID param) {
    auto* job = static_cast<SizeJob*>(param);
    std::atomic<bool> stop{ false };
    int64_t size = FolderSize(job->path, stop);
    std::wstring* out = new std::wstring(
        L"Folder\nSize: " + FormatSize(size));
    PostMessageW(g_app.hwnd, WM_SIZE_DONE, 0, reinterpret_cast<LPARAM>(out));
    delete job;
    return 0;
}

// Flattened search, streaming results in batches.
struct SearchJob { std::wstring root, query; };
static DWORD WINAPI SearchThreadProc(LPVOID param) {
    auto* job = static_cast<SearchJob*>(param);
    std::atomic<bool> stop{ false };
    std::vector<Entry> batch;
    SearchFlattened(job->root, job->query, L"",
        [&](const Entry& e) {
            batch.push_back(e);
            if (batch.size() >= 100) {
                auto* copy = new std::vector<Entry>(std::move(batch));
                batch.clear();
                PostMessageW(g_app.hwnd, WM_SEARCH_HIT, 0,
                             reinterpret_cast<LPARAM>(copy));
            }
        }, stop, 1000);
    if (!batch.empty()) {
        auto* copy = new std::vector<Entry>(std::move(batch));
        PostMessageW(g_app.hwnd, WM_SEARCH_HIT, 0, reinterpret_cast<LPARAM>(copy));
    }
    PostMessageW(g_app.hwnd, WM_SEARCH_DONE, 0, 0);
    delete job;
    return 0;
}


// ------------------------------------------------------------------ prompt
// Small modal text prompt (new folder / rename), built in code.
static std::wstring PromptText(HWND owner, const wchar_t* title,
                               const wchar_t* label,
                               const std::wstring& def = L"") {
    struct Ctx { std::wstring result; bool ok = false; HWND edit = nullptr; };
    Ctx ctx;
    ctx.result = def;
    WNDCLASSW wc{};
    wc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
        Ctx* c = reinterpret_cast<Ctx*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        switch (m) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
            c = static_cast<Ctx*>(cs->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(c));
            CreateWindowExW(0, L"STATIC", cs->lpszName, WS_CHILD | WS_VISIBLE,
                            12, 12, 360, 18, h, nullptr,
                            GetModuleHandleW(nullptr), nullptr);
            c->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", c->result.c_str(),
                                      WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                      12, 36, 360, 24, h, nullptr,
                                      GetModuleHandleW(nullptr), nullptr);
            CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                            200, 72, 80, 26, h, (HMENU)IDOK,
                            GetModuleHandleW(nullptr), nullptr);
            CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE,
                            292, 72, 80, 26, h, (HMENU)IDCANCEL,
                            GetModuleHandleW(nullptr), nullptr);
            SetFocus(c->edit);
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(w) == IDOK) {
                int len = GetWindowTextLengthW(c->edit);
                std::wstring t(len, L'\0');
                GetWindowTextW(c->edit, t.data(), len + 1);
                c->result = t;
                c->ok = true;
                DestroyWindow(h);
            } else if (LOWORD(w) == IDCANCEL) {
                DestroyWindow(h);
            }
            return 0;
        case WM_CLOSE: DestroyWindow(h); return 0;
        }
        return DefWindowProcW(h, m, w, l);
    };
    wc.hInstance = g_app.hinst;
    wc.lpszClassName = L"TNxPrompt";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);
    RECT rc{};
    GetWindowRect(owner, &rc);
    HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME, L"TNxPrompt", title,
                             WS_POPUP | WS_CAPTION | WS_SYSMENU,
                             rc.left + 100, rc.top + 100, 400, 140,
                             owner, nullptr, g_app.hinst, &ctx);
    // NOTE: label is shown via lpszName trick above (window title reuse);
    // set the static text properly:
    SetWindowTextW(h, title);
    HWND label_wnd = FindWindowExW(h, nullptr, L"STATIC", nullptr);
    if (label_wnd) SetWindowTextW(label_wnd, label);
    ShowWindow(h, SW_SHOW);
    MSG msg;
    while (IsWindow(h) && GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return ctx.ok ? ctx.result : L"";
}

// ------------------------------------------------------------------ tabs
static void AddTab(const std::wstring& path) {
    auto tab = std::make_unique<TabState>();
    tab->path = path;
    tab->history = { path };
    tab->view_mode = g_app.config.settings.view_mode;
    tab->sort_key = g_app.config.settings.sort_key;
    tab->sort_reverse = g_app.config.settings.sort_reverse;
    TabState* raw = tab.get();
    g_app.tabs.push_back(std::move(tab));
    int idx = static_cast<int>(g_app.tabs.size()) - 1;
    TCITEMW ti{ TCIF_TEXT };
    std::wstring title = path;
    size_t slash = title.find_last_of(L"\\/");
    if (slash != std::wstring::npos) title = title.substr(slash + 1);
    ti.pszText = title.data();
    TabCtrl_InsertItem(g_app.tab, idx, &ti);
    TabCtrl_SetCurSel(g_app.tab, idx);
    g_app.active_tab = idx;
    Navigate(raw, path, false);
}

static void CloseTab(int idx) {
    if (g_app.tabs.size() <= 1 || idx < 0 ||
        idx >= static_cast<int>(g_app.tabs.size())) return;
    g_app.tabs.erase(g_app.tabs.begin() + idx);
    TabCtrl_DeleteItem(g_app.tab, idx);
    g_app.active_tab = TabCtrl_GetCurSel(g_app.tab);
    TabState* t = ActiveTab();
    if (t) StartListing(t);
}

// ------------------------------------------------------------------ inspector
static void UpdateInspector() {
    HWND list = ActiveList();
    int sel = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    if (sel < 0) {
        SetWindowTextW(g_app.insp_title, L"No selection");
        SetWindowTextW(g_app.insp_info, L"");
        SetWindowTextW(g_app.insp_text, L"");
        g_app.insp_bitmap.reset();
        InvalidateRect(g_app.inspector, nullptr, TRUE);
        return;
    }
    TabState* tab = ActiveTab();
    if (!tab || sel >= static_cast<int>(tab->entries.size())) return;
    const Entry& e = tab->entries[sel];
    SetWindowTextW(g_app.insp_title, e.name.c_str());
    g_app.insp_bitmap.reset();
    SetWindowTextW(g_app.insp_text, L"");
    if (e.is_dir) {
        SetWindowTextW(g_app.insp_info, L"Folder — calculating size…");
        auto* job = new SizeJob{ e.path };
        if (g_app.size_thread) CloseHandle(g_app.size_thread);
        g_app.size_thread = CreateThread(nullptr, 0, SizeThreadProc, job, 0, nullptr);
        return;
    }
    std::wstring info = L"Size: " + FormatSize(e.size) + L" (" +
                        std::to_wstring(e.size) + L" bytes)\nType: " +
                        e.TypeLabel() + L"\nModified: " + FormatFileTime(e.mtime) +
                        L"\nPath: " + e.path;
    SetWindowTextW(g_app.insp_info, info.c_str());
    std::wstring ext = e.ext;
    if (ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".gif" ||
        ext == L".bmp" || ext == L".webp" || ext == L".ico") {
        g_app.insp_bitmap = std::make_unique<Bitmap>(e.path.c_str());
        if (g_app.insp_bitmap->GetLastStatus() != Ok) g_app.insp_bitmap.reset();
        InvalidateRect(g_app.inspector, nullptr, TRUE);
    } else if (ext == L".txt" || ext == L".md" || ext == L".log" ||
               ext == L".json" || ext == L".ini" || ext == L".csv" ||
               ext == L".xml" || ext == L".py" || ext == L".cpp" ||
               ext == L".h" || ext == L".bat" || ext == L".ps1") {
        HANDLE h = CreateFileW(e.path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            char buf[32768] = {};
            DWORD read = 0;
            ReadFile(h, buf, sizeof(buf) - 1, &read, nullptr);
            CloseHandle(h);
            int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                           buf, static_cast<int>(read),
                                           nullptr, 0);
            if (wlen > 0) {
                std::wstring text(wlen, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, buf, static_cast<int>(read),
                                    text.data(), wlen);
                SetWindowTextW(g_app.insp_text, text.c_str());
            } else {
                SetWindowTextW(g_app.insp_text, L"(Binary / undecodable text)");
            }
        }
    }
}

// ------------------------------------------------------------------ selection helpers
static std::vector<Entry> SelectedEntries(HWND list) {
    std::vector<Entry> out;
    TabState* tab = ActiveTab();
    if (!tab) return out;
    int idx = -1;
    while ((idx = ListView_GetNextItem(list, idx, LVNI_SELECTED)) != -1) {
        if (idx < static_cast<int>(tab->entries.size()))
            out.push_back(tab->entries[idx]);
    }
    return out;
}

static void OpenEntry(const Entry& e) {
    if (e.is_dir) {
        TabState* t = ActiveTab();
        if (t) Navigate(t, e.path);
    } else {
        ShellExecuteW(g_app.hwnd, L"open", e.path.c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL);
    }
}

// ------------------------------------------------------------------ listview
static HWND CreateListView(HWND parent, int id) {
    HWND list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
        WS_CHILD | WS_VISIBLE | LVS_OWNERDATA | LVS_REPORT | LVS_SHOWSELALWAYS,
        0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, g_app.hinst, nullptr);
    ListView_SetExtendedListViewStyleEx(list,
        LVS_EX_DOUBLEBUFFER | LVS_EX_FULLROWSELECT | LVS_EX_HEADERDRAGDROP,
        LVS_EX_DOUBLEBUFFER | LVS_EX_FULLROWSELECT | LVS_EX_HEADERDRAGDROP);
    LVCOLUMNW col{ LVCF_TEXT | LVCF_WIDTH, 0, 320, const_cast<LPWSTR>(L"Name") };
    ListView_InsertColumn(list, 0, &col);
    col.cx = 110; col.pszText = const_cast<LPWSTR>(L"Size");
    ListView_InsertColumn(list, 1, &col);
    col.cx = 130; col.pszText = const_cast<LPWSTR>(L"Type");
    ListView_InsertColumn(list, 2, &col);
    col.cx = 150; col.pszText = const_cast<LPWSTR>(L"Modified");
    ListView_InsertColumn(list, 3, &col);
    ListView_SetImageList(list, g_app.img_small, LVSIL_SMALL);
    ListView_SetImageList(list, g_app.img_large, LVSIL_NORMAL);
    DragAcceptFiles(list, TRUE);
    return list;
}

static void SetListViewMode(HWND list, const std::wstring& mode) {
    DWORD style = GetWindowLongW(list, GWL_STYLE);
    style &= ~(LVS_TYPEMASK);
    if (mode == L"icons") style |= LVS_ICON;
    else if (mode == L"list") style |= LVS_LIST;
    else style |= LVS_REPORT; // details + tree fallback
    SetWindowLongW(list, GWL_STYLE, style);
    SetWindowPos(list, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
}

// ------------------------------------------------------------------ sidebar tree
// Sidebar paths are kept in a parallel map keyed by HTREEITEM.
static std::map<HTREEITEM, std::wstring> g_tree_paths;

static void BuildSidebar() {
    TreeView_DeleteAllItems(g_app.tree);
    g_tree_paths.clear();
    TVINSERTSTRUCTW ins{};
    ins.hParent = TVI_ROOT;
    ins.hInsertAfter = TVI_LAST;
    ins.item.mask = TVIF_TEXT;
    ins.item.pszText = const_cast<LPWSTR>(L"This PC");
    HTREEITEM pc = TreeView_InsertItem(g_app.tree, &ins);
    for (const auto& d : GetDrives()) {
        std::wstring label = d.label + L" (" + d.path + L") — " +
                             FormatSize(d.total - d.free) + L" / " +
                             FormatSize(d.total);
        TVINSERTSTRUCTW c{};
        c.hParent = pc;
        c.hInsertAfter = TVI_LAST;
        c.item.mask = TVIF_TEXT;
        c.item.pszText = const_cast<LPWSTR>(label.c_str());
        HTREEITEM it = TreeView_InsertItem(g_app.tree, &c);
        g_tree_paths[it] = d.path;
    }
    TreeView_Expand(g_app.tree, pc, TVE_EXPAND);
    ins.item.pszText = const_cast<LPWSTR>(L"Places");
    HTREEITEM places = TreeView_InsertItem(g_app.tree, &ins);
    for (const auto& [name, path] : SystemPaths()) {
        TVINSERTSTRUCTW c{};
        c.hParent = places;
        c.hInsertAfter = TVI_LAST;
        c.item.mask = TVIF_TEXT;
        c.item.pszText = const_cast<LPWSTR>(name.c_str());
        HTREEITEM it = TreeView_InsertItem(g_app.tree, &c);
        g_tree_paths[it] = path;
    }
    TreeView_Expand(g_app.tree, places, TVE_EXPAND);
    ins.item.pszText = const_cast<LPWSTR>(L"Bookmarks");
    HTREEITEM bm = TreeView_InsertItem(g_app.tree, &ins);
    for (const auto& b : g_app.config.bookmarks) {
        TVINSERTSTRUCTW c{};
        c.hParent = bm;
        c.hInsertAfter = TVI_LAST;
        c.item.mask = TVIF_TEXT;
        c.item.pszText = const_cast<LPWSTR>(b.c_str());
        HTREEITEM it = TreeView_InsertItem(g_app.tree, &c);
        g_tree_paths[it] = b;
    }
    ins.item.pszText = const_cast<LPWSTR>(L"Recents");
    HTREEITEM rc = TreeView_InsertItem(g_app.tree, &ins);
    for (const auto& r : g_app.config.recents) {
        TVINSERTSTRUCTW c{};
        c.hParent = rc;
        c.hInsertAfter = TVI_LAST;
        c.item.mask = TVIF_TEXT;
        c.item.pszText = const_cast<LPWSTR>(r.c_str());
        HTREEITEM it = TreeView_InsertItem(g_app.tree, &c);
        g_tree_paths[it] = r;
    }
}

// ------------------------------------------------------------------ layout
static void LayoutChildren() {
    RECT rc{};
    GetClientRect(g_app.hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    // Status bar at bottom (created by CreateStatusWindow, auto positions).
    SendMessageW(g_app.status, WM_SIZE, 0, 0);
    RECT sbr{};
    GetWindowRect(g_app.status, &sbr);
    int status_h = sbr.bottom - sbr.top;
    int toolbar_h = 34, tab_h = 28;
    SetWindowPos(g_app.goto_edit, nullptr, 8, 4, 420, 24, SWP_NOZORDER);
    SetWindowPos(g_app.filter_edit, nullptr, 436, 4, 220, 24, SWP_NOZORDER);
    SetWindowPos(g_app.view_box, nullptr, 664, 4, 110, 200, SWP_NOZORDER);
    int top = toolbar_h + tab_h;
    int body_h = H - top - status_h;
    SetWindowPos(g_app.tab, nullptr, 0, toolbar_h, W, tab_h, SWP_NOZORDER);
    int left_w = g_app.sidebar_w, right_w = g_app.inspector_w;
    SetWindowPos(g_app.sidebar, nullptr, 0, top, left_w, body_h, SWP_NOZORDER);
    SetWindowPos(g_app.tree, nullptr, 0, 0, left_w, body_h, SWP_NOZORDER);
    SetWindowPos(g_app.inspector, nullptr, W - right_w, top, right_w, body_h, SWP_NOZORDER);
    int center_x = left_w + 4, center_w = W - left_w - right_w - 8;
    if (g_app.dual && g_app.list_b) {
        int half = center_w / 2;
        SetWindowPos(g_app.list_a, nullptr, center_x, top, half - 2, body_h, SWP_NOZORDER);
        SetWindowPos(g_app.list_b, nullptr, center_x + half + 2, top, half - 2, body_h, SWP_NOZORDER);
        ShowWindow(g_app.list_b, SW_SHOW);
    } else {
        SetWindowPos(g_app.list_a, nullptr, center_x, top, center_w, body_h, SWP_NOZORDER);
        if (g_app.list_b) ShowWindow(g_app.list_b, SW_HIDE);
    }
    // Inspector children.
    RECT ir{};
    GetWindowRect(g_app.inspector, &ir);
    int iw = ir.right - ir.left;
    SetWindowPos(g_app.insp_title, nullptr, 8, 8, iw - 16, 20, SWP_NOZORDER);
    SetWindowPos(g_app.insp_info, nullptr, 8, 32, iw - 16, 64, SWP_NOZORDER);
    SetWindowPos(g_image_view, nullptr, 8, 104, iw - 16, 140, SWP_NOZORDER);
    SetWindowPos(g_app.insp_text, nullptr, 8, 252, iw - 16, body_h - 262, SWP_NOZORDER);
}

// ------------------------------------------------------------------ commands
static void DoPaste() {
    if (g_app.clip_paths.empty()) return;
    TabState* tab = ActiveTab();
    if (!tab) return;
    CopyJob job{ g_app.clip_paths, tab->path, g_app.clip_cut };
    RunCopyJob(g_app.hwnd, job);
    if (g_app.clip_cut) g_app.clip_paths.clear();
    StartListing(tab);
}

static std::vector<Command> BuildCommands() {
    TabState* tab = ActiveTab();
    return {
        { L"New Tab", L"Ctrl+T", [] { wchar_t home[MAX_PATH] = {}; SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, home); AddTab(home); } },
        { L"New Folder", L"Ctrl+Shift+N", [] {
            TabState* t = ActiveTab(); if (!t) return;
            std::wstring name = PromptText(g_app.hwnd, L"New Folder", L"Folder name:");
            if (!name.empty()) { CreateDirectoryW((t->path + L"\\" + name).c_str(), nullptr); StartListing(t); } } },
        { L"Go To…", L"Ctrl+L", [] { SetFocus(g_app.goto_edit); } },
        { L"Search in Folder…", L"Ctrl+F", [] { SetFocus(g_app.filter_edit); } },
        { L"Batch Rename…", L"", [] {
            auto sel = SelectedEntries(ActiveList());
            TabState* t = ActiveTab();
            if (t && ShowBatchRename(g_app.hwnd, sel.empty() ? t->entries : sel)) StartListing(t); } },
        { L"Toggle Dual Panel", L"Ctrl+D", [] {
            g_app.dual = !g_app.dual;
            g_app.config.settings.dual_panel = g_app.dual;
            LayoutChildren(); } },
        { L"Toggle Hidden Files", L"", [] {
            g_app.config.settings.show_hidden = !g_app.config.settings.show_hidden;
            TabState* t = ActiveTab(); if (t) StartListing(t); } },
        { L"Toggle Theme", L"", [] {
            g_app.dark = !g_app.dark;
            g_app.config.settings.theme = g_app.dark ? L"dark" : L"light";
            InvalidateRect(g_app.hwnd, nullptr, TRUE); } },
        { L"Settings…", L"", [] {
            if (ShowSettings(g_app.hwnd, g_app.config)) {
                g_app.dark = g_app.config.settings.theme == L"dark";
                TabState* t = ActiveTab(); if (t) StartListing(t);
                LayoutChildren();
            } } },
        { L"Refresh", L"F5", [] { TabState* t = ActiveTab(); if (t) StartListing(t); } },
        { L"Go Up", L"Alt+Up", [] {
            TabState* t = ActiveTab(); if (!t) return;
            std::wstring parent = t->path;
            size_t s = parent.find_last_of(L"\\/");
            if (s != std::wstring::npos && s > 0) Navigate(t, parent.substr(0, s)); } },
        { L"Bookmark Current Folder", L"", [] {
            TabState* t = ActiveTab();
            if (t && g_app.config.AddBookmark(t->path)) { g_app.config.Save(); BuildSidebar(); } } },
        { L"Open in Terminal", L"", [] {
            TabState* t = ActiveTab(); if (!t) return;
            std::wstring cmd = L"powershell.exe -NoExit -Command Set-Location '" + t->path + L"'";
            STARTUPINFOW si{ sizeof(si) };
            PROCESS_INFORMATION pi{};
            std::wstring cmdline = cmd;
            CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE,
                           0, nullptr, t->path.c_str(), &si, &pi);
            if (pi.hProcess) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); } } },
        { L"About TNxPilot", L"", [] {
            MessageBoxW(g_app.hwnd, L"TNxPilot 1.0.0\nFast, fully offline file explorer.\nPrivate use only.",
                        L"About TNxPilot", MB_ICONINFORMATION); } },
    };
}

static void ShowEntryContextMenu(HWND list, POINT pt) {
    auto entries = SelectedEntries(list);
    HMENU menu = CreatePopupMenu();
    TabState* tab = ActiveTab();
    if (!entries.empty()) {
        AppendMenuW(menu, MF_STRING, IDM_OPEN, L"&Open");
        AppendMenuW(menu, MF_STRING, IDM_TERMINAL, L"Open in &Terminal");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_CUT, L"Cu&t");
        AppendMenuW(menu, MF_STRING, IDM_COPY, L"&Copy");
        AppendMenuW(menu, MF_STRING, IDM_COPY_PATH, L"Copy as &Path");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_RENAME, L"&Rename (F2)");
        AppendMenuW(menu, MF_STRING, IDM_DELETE, L"&Delete");
        AppendMenuW(menu, MF_STRING, IDM_PROPS, L"&Properties");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, IDM_BATCH_RENAME, L"&Batch Rename…");
    } else {
        AppendMenuW(menu, MF_STRING, IDM_PASTE, L"&Paste");
        AppendMenuW(menu, MF_STRING, IDM_NEW_FOLDER, L"New &Folder");
        AppendMenuW(menu, MF_STRING, IDM_NEW_FILE, L"New &File");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_BOOKMARK, L"&Bookmark This Folder");
    AppendMenuW(menu, MF_STRING, IDM_REFRESH, L"&Refresh (F5)");
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                             pt.x, pt.y, 0, g_app.hwnd, nullptr);
    DestroyMenu(menu);
    if (cmd == 0 || !tab) return;
    switch (cmd) {
    case IDM_OPEN: if (!entries.empty()) OpenEntry(entries[0]); break;
    case IDM_TERMINAL: {
        std::wstring cmdline = L"powershell.exe -NoExit -Command Set-Location '" + tab->path + L"'";
        STARTUPINFOW si{ sizeof(si) };
        PROCESS_INFORMATION pi{};
        CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE,
                       0, nullptr, tab->path.c_str(), &si, &pi);
        if (pi.hProcess) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
        break;
    }
    case IDM_CUT: g_app.clip_paths.clear();
        for (const auto& e : entries) g_app.clip_paths.push_back(e.path);
        g_app.clip_cut = true; break;
    case IDM_COPY: g_app.clip_paths.clear();
        for (const auto& e : entries) g_app.clip_paths.push_back(e.path);
        g_app.clip_cut = false; break;
    case IDM_COPY_PATH: {
        std::wstring text;
        for (const auto& e : entries) { if (!text.empty()) text += L"\r\n"; text += e.path; }
        if (OpenClipboard(g_app.hwnd)) {
            EmptyClipboard();
            HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
            memcpy(GlobalLock(h), text.c_str(), (text.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(h);
            SetClipboardData(CF_UNICODETEXT, h);
            CloseClipboard();
        }
        break;
    }
    case IDM_RENAME:
        if (!entries.empty()) {
            std::wstring name = PromptText(g_app.hwnd, L"Rename", L"New name:", entries[0].name);
            if (!name.empty() && name != entries[0].name) {
                std::wstring dst = entries[0].path;
                size_t s = dst.find_last_of(L"\\/");
                dst = dst.substr(0, s + 1) + name;
                MoveFileExW(entries[0].path.c_str(), dst.c_str(), 0);
                StartListing(tab);
            }
        }
        break;
    case IDM_DELETE: {
        if (MessageBoxW(g_app.hwnd, L"Permanently delete the selected item(s)?",
                        L"Delete", MB_YESNO | MB_ICONWARNING) == IDYES) {
            std::vector<std::wstring> paths;
            for (const auto& e : entries) paths.push_back(e.path);
            SafeDelete(paths);
            StartListing(tab);
        }
        break;
    }
    case IDM_PROPS:
        if (!entries.empty()) {
            std::wstring info = L"Path: " + entries[0].path + L"\nType: " +
                                entries[0].TypeLabel() + L"\nSize: " +
                                FormatSize(entries[0].size) + L"\nModified: " +
                                FormatFileTime(entries[0].mtime);
            MessageBoxW(g_app.hwnd, info.c_str(), L"Properties", MB_ICONINFORMATION);
        }
        break;
    case IDM_BATCH_RENAME:
        if (ShowBatchRename(g_app.hwnd, entries.empty() ? tab->entries : entries))
            StartListing(tab);
        break;
    case IDM_PASTE: DoPaste(); break;
    case IDM_NEW_FOLDER: {
        std::wstring name = PromptText(g_app.hwnd, L"New Folder", L"Folder name:");
        if (!name.empty()) {
            CreateDirectoryW((tab->path + L"\\" + name).c_str(), nullptr);
            StartListing(tab);
        }
        break;
    }
    case IDM_NEW_FILE: {
        std::wstring name = PromptText(g_app.hwnd, L"New File", L"File name:");
        if (!name.empty()) {
            HANDLE h = CreateFileW((tab->path + L"\\" + name).c_str(),
                                   GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            StartListing(tab);
        }
        break;
    }
    case IDM_BOOKMARK:
        if (g_app.config.AddBookmark(tab->path)) { g_app.config.Save(); BuildSidebar(); }
        break;
    case IDM_REFRESH: StartListing(tab); break;
    }
}


// ------------------------------------------------------------------ image view
static std::map<HTREEITEM, std::wstring> g_center_tree_paths;

static LRESULT CALLBACK ImageViewProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(h, &ps);
        RECT rc{};
        GetClientRect(h, &rc);
        FillRect(dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        if (g_app.insp_bitmap) {
            Graphics g(dc);
            UINT iw = g_app.insp_bitmap->GetWidth();
            UINT ih = g_app.insp_bitmap->GetHeight();
            double scale = std::min((double)(rc.right - rc.left) / iw,
                                    (double)(rc.bottom - rc.top) / ih);
            int dw = static_cast<int>(iw * scale), dh = static_cast<int>(ih * scale);
            g.DrawImage(g_app.insp_bitmap.get(), (rc.right - dw) / 2,
                        (rc.bottom - dh) / 2, dw, dh);
        }
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ------------------------------------------------------------------ center tree (Tree view mode)
static void PopulateCenterTree(const std::wstring& root) {
    TreeView_DeleteAllItems(g_center_tree);
    g_center_tree_paths.clear();
    std::function<void(HTREEITEM, const std::wstring&, int)> add_level =
        [&](HTREEITEM parent, const std::wstring& dir, int depth) {
            if (depth > 1) return;
            for (const auto& e : ListDirectory(dir, false)) {
                if (!e.is_dir) continue;
                TVINSERTSTRUCTW ins{};
                ins.hParent = parent;
                ins.hInsertAfter = TVI_LAST;
                ins.item.mask = TVIF_TEXT | TVIF_CHILDREN;
                ins.item.pszText = const_cast<LPWSTR>(e.name.c_str());
                ins.item.cChildren = 1;
                HTREEITEM it = TreeView_InsertItem(g_center_tree, &ins);
                g_center_tree_paths[it] = e.path;
                if (depth == 0) add_level(it, e.path, depth + 1);
            }
        };
    add_level(TVI_ROOT, root, 0);
}

// ------------------------------------------------------------------ dark theme
static HBRUSH g_dark_brush = nullptr, g_dark_edit_brush = nullptr;

static LRESULT HandleCustomDraw(NMHDR* nm, HWND list) {
    auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(nm);
    if (!g_app.dark) return CDRF_DODEFAULT;
    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT:
        cd->clrText = RGB(224, 224, 224);
        cd->clrTextBk = (cd->nmcd.uItemState & CDIS_SELECTED)
            ? RGB(9, 71, 113) : RGB(30, 30, 30);
        return CDRF_NEWFONT;
    }
    return CDRF_DODEFAULT;
}

// ------------------------------------------------------------------ window proc
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE: {
        g_app.hwnd = hwnd;
        INITCOMMONCONTROLSEX icex{ sizeof(icex), ICC_LISTVIEW_CLASSES |
            ICC_TREEVIEW_CLASSES | ICC_TAB_CLASSES | ICC_BAR_CLASSES |
            ICC_PROGRESS_CLASS };
        InitCommonControlsEx(&icex);
        InitImageLists();
        g_dark_brush = CreateSolidBrush(RGB(30, 30, 30));
        g_dark_edit_brush = CreateSolidBrush(RGB(37, 37, 38));

        // Menu bar.
        HMENU menu = CreateMenu();
        HMENU m_file = CreatePopupMenu();
        AppendMenuW(m_file, MF_STRING, IDM_NEW_TAB, L"New Tab\tCtrl+T");
        AppendMenuW(m_file, MF_STRING, IDM_NEW_FOLDER, L"New Folder\tCtrl+Shift+N");
        AppendMenuW(m_file, MF_STRING, IDM_NEW_FILE, L"New File");
        AppendMenuW(m_file, MF_STRING, IDM_CLOSE_TAB, L"Close Tab\tCtrl+W");
        AppendMenuW(m_file, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m_file, MF_STRING, IDM_EXIT, L"Exit");
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)m_file, L"&File");
        HMENU m_view = CreatePopupMenu();
        AppendMenuW(m_view, MF_STRING, IDM_DUAL, L"Dual Panel\tCtrl+D");
        AppendMenuW(m_view, MF_STRING, IDM_HIDDEN, L"Show Hidden Files");
        AppendMenuW(m_view, MF_STRING, IDM_THEME, L"Toggle Dark / Light Theme");
        AppendMenuW(m_view, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m_view, MF_STRING, IDM_VIEW_DETAILS, L"Details View");
        AppendMenuW(m_view, MF_STRING, IDM_VIEW_LIST, L"List View");
        AppendMenuW(m_view, MF_STRING, IDM_VIEW_ICONS, L"Icons View");
        AppendMenuW(m_view, MF_STRING, IDM_VIEW_TREE, L"Tree View");
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)m_view, L"&View");
        HMENU m_go = CreatePopupMenu();
        AppendMenuW(m_go, MF_STRING, IDM_GOTO, L"Go To…\tCtrl+L");
        AppendMenuW(m_go, MF_STRING, IDM_SEARCH, L"Search in Folder…\tCtrl+F");
        AppendMenuW(m_go, MF_STRING, IDM_BACK, L"Back\tAlt+Left");
        AppendMenuW(m_go, MF_STRING, IDM_FWD, L"Forward\tAlt+Right");
        AppendMenuW(m_go, MF_STRING, IDM_UP, L"Up\tAlt+Up");
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)m_go, L"&Go");
        HMENU m_tools = CreatePopupMenu();
        AppendMenuW(m_tools, MF_STRING, IDM_PALETTE, L"Command Palette\tCtrl+Shift+P");
        AppendMenuW(m_tools, MF_STRING, IDM_BATCH_RENAME, L"Batch Rename…");
        AppendMenuW(m_tools, MF_STRING, IDM_SETTINGS, L"Settings…");
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)m_tools, L"&Tools");
        HMENU m_help = CreatePopupMenu();
        AppendMenuW(m_help, MF_STRING, IDM_ABOUT, L"About TNxPilot");
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)m_help, L"&Help");
        SetMenu(hwnd, menu);

        // Toolbar row: GoTo + filter + view + icon-size slider.
        g_app.goto_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_GOTO, g_app.hinst, nullptr);
        SHAutoComplete(g_app.goto_edit, SHACF_FILESYSTEM | SHACF_USETAB);
        g_app.filter_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_FILTER, g_app.hinst, nullptr);
        SendMessageW(g_app.filter_edit, EM_SETCUEBANNER, TRUE,
                     (LPARAM)L"Filter / fuzzy search…");
        g_app.view_box = CreateWindowExW(0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
            0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_VIEWBOX, g_app.hinst, nullptr);
        const wchar_t* views[] = { L"Details", L"List", L"Icons", L"Tree" };
        for (auto* v : views) SendMessageW(g_app.view_box, CB_ADDSTRING, 0, (LPARAM)v);
        SendMessageW(g_app.view_box, CB_SETCURSEL, 0, 0);
        HWND slider = CreateWindowExW(0, TRACKBAR_CLASSW, nullptr,
            WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
            782, 6, 110, 22, hwnd, (HMENU)201, g_app.hinst, nullptr);
        SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(24, 128));
        SendMessageW(slider, TBM_SETPOS, TRUE, g_app.config.settings.icon_size);

        // Tab control.
        g_app.tab = CreateWindowExW(0, WC_TABCONTROLW, nullptr,
            WS_CHILD | WS_VISIBLE | TCS_SINGLELINE,
            0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_TAB, g_app.hinst, nullptr);

        // Sidebar tree.
        g_app.sidebar = CreateWindowExW(0, WC_TREEVIEWW, nullptr,
            WS_CHILD | WS_VISIBLE | TVS_HASLINES | TVS_LINESATROOT |
            TVS_HASBUTTONS | TVS_SHOWSELALWAYS,
            0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_SIDEBAR, g_app.hinst, nullptr);
        g_app.tree = g_app.sidebar;

        // Center panes.
        g_app.list_a = CreateListView(hwnd, IDC_LIST_A);
        g_app.list_b = CreateListView(hwnd, IDC_LIST_B);
        ShowWindow(g_app.list_b, SW_HIDE);
        g_center_tree = CreateWindowExW(0, WC_TREEVIEWW, nullptr,
            WS_CHILD | TVS_HASLINES | TVS_LINESATROOT | TVS_HASBUTTONS |
            TVS_SHOWSELALWAYS,
            0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_TREE, g_app.hinst, nullptr);

        // Inspector (plain child window + controls).
        WNDCLASSW pwc{};
        pwc.lpfnWndProc = DefWindowProcW;
        pwc.hInstance = g_app.hinst;
        pwc.lpszClassName = L"TNxPanel";
        pwc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        RegisterClassW(&pwc);
        g_app.inspector = CreateWindowExW(0, L"TNxPanel", nullptr,
            WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, hwnd,
            (HMENU)(INT_PTR)IDC_INSPECTOR, g_app.hinst, nullptr);
        g_app.insp_title = CreateWindowExW(0, L"STATIC", L"No selection",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            0, 0, 10, 10, g_app.inspector, nullptr, g_app.hinst, nullptr);
        g_app.insp_info = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            0, 0, 10, 10, g_app.inspector, nullptr, g_app.hinst, nullptr);
        g_image_view = CreateWindowExW(0, L"TNxImageView", nullptr,
            WS_CHILD | WS_VISIBLE | WS_BORDER,
            0, 0, 10, 10, g_app.inspector, nullptr, g_app.hinst, nullptr);
        g_app.insp_text = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
            0, 0, 10, 10, g_app.inspector, nullptr, g_app.hinst, nullptr);

        // Status bar.
        g_app.status = CreateStatusWindowW(WS_CHILD | WS_VISIBLE, L"Ready",
                                           hwnd, IDC_STATUS);

        // Subclass GoTo edit: Enter navigates.
        SetWindowSubclass(g_app.goto_edit,
            [](HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) -> LRESULT {
                if (m == WM_KEYDOWN && w == VK_RETURN) {
                    wchar_t buf[MAX_PATH] = {};
                    GetWindowTextW(h, buf, MAX_PATH);
                    TabState* t = ActiveTab();
                    if (t && buf[0]) Navigate(t, buf);
                    return 0;
                }
                return DefSubclassProc(h, m, w, l);
            }, 0, 0);
        // Subclass filter edit: typing re-lists (debounced by the worker seq).
        SetWindowSubclass(g_app.filter_edit,
            [](HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) -> LRESULT {
                LRESULT r = DefSubclassProc(h, m, w, l);
                if (m == WM_KEYDOWN || m == WM_CHAR) {
                    TabState* t = ActiveTab();
                    if (t) {
                        wchar_t buf[512] = {};
                        GetWindowTextW(h, buf, 512);
                        t->filter = buf;
                        StartListing(t);
                    }
                }
                return r;
            }, 0, 0);

        // Config + session restore.
        g_app.config.Load();
        g_app.dark = g_app.config.settings.theme != L"light";
        g_app.dual = g_app.config.settings.dual_panel;
        BuildSidebar();
        std::vector<std::wstring> tabs;
        int active = 0;
        bool have_session = g_app.config.settings.restore_session &&
                            g_app.config.LoadSession(tabs, active);
        if (!have_session || tabs.empty()) {
            wchar_t home[MAX_PATH] = {};
            SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, home);
            tabs = { home };
        }
        for (const auto& p : tabs)
            if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES)
                AddTab(p);
        if (g_app.tabs.empty()) {
            wchar_t home[MAX_PATH] = {};
            SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, home);
            AddTab(home);
        }
        if (active >= 0 && active < static_cast<int>(g_app.tabs.size())) {
            TabCtrl_SetCurSel(g_app.tab, active);
            g_app.active_tab = active;
            StartListing(ActiveTab());
        }
        // Apply saved view mode to the combo.
        {
            const std::wstring& vm = g_app.config.settings.view_mode;
            int sel = vm == L"list" ? 1 : vm == L"icons" ? 2 : vm == L"tree" ? 3 : 0;
            SendMessageW(g_app.view_box, CB_SETCURSEL, sel, 0);
            TabState* t = ActiveTab();
            if (t) {
                t->view_mode = vm;
                SetListViewMode(g_app.list_a, vm);
                SetListViewMode(g_app.list_b, vm);
            }
        }
        LayoutChildren();
        return 0;
    }

    case WM_SIZE:
        LayoutChildren();
        return 0;

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC dc = (HDC)w;
        if (g_app.dark) {
            SetTextColor(dc, RGB(224, 224, 224));
            SetBkColor(dc, RGB(30, 30, 30));
            return (LRESULT)g_dark_brush;
        }
        break;
    }

    case WM_NOTIFY: {
        auto* nm = reinterpret_cast<NMHDR*>(l);
        HWND from = nm->hwndFrom;
        if (nm->code == LVN_GETDISPINFO &&
            (from == g_app.list_a || from == g_app.list_b)) {
            auto* di = reinterpret_cast<NMLVDISPINFOW*>(l);
            TabState* tab = ActiveTab();
            if (tab && di->item.iItem < static_cast<int>(tab->entries.size())) {
                const Entry& e = tab->entries[di->item.iItem];
                if (di->item.mask & LVIF_TEXT) {
                    switch (di->item.iSubItem) {
                    case 0:
                        wcsncpy_s(di->item.pszText, di->item.cchTextMax,
                                  e.name.c_str(), _TRUNCATE);
                        break;
                    case 1:
                        if (!e.is_dir) {
                            std::wstring s = FormatSize(e.size);
                            wcsncpy_s(di->item.pszText, di->item.cchTextMax,
                                      s.c_str(), _TRUNCATE);
                        } else {
                            di->item.pszText[0] = L'\0';
                        }
                        break;
                    case 2: {
                        std::wstring s = e.TypeLabel();
                        wcsncpy_s(di->item.pszText, di->item.cchTextMax,
                                  s.c_str(), _TRUNCATE);
                        break;
                    }
                    case 3: {
                        std::wstring s = FormatFileTime(e.mtime);
                        wcsncpy_s(di->item.pszText, di->item.cchTextMax,
                                  s.c_str(), _TRUNCATE);
                        break;
                    }
                    }
                }
                if (di->item.mask & LVIF_IMAGE)
                    di->item.iImage = IconIndexFor(e);
            }
            return 0;
        }
        if (nm->code == LVN_ODCACHEHINT &&
            (from == g_app.list_a || from == g_app.list_b)) {
            // Prefetch icons for the visible range (lazy cache warm-up).
            auto* hint = reinterpret_cast<NMLVCACHEHINT*>(l);
            TabState* tab = ActiveTab();
            if (tab) {
                for (int i = hint->iFrom; i <= hint->iTo &&
                     i < static_cast<int>(tab->entries.size()); ++i)
                    IconIndexFor(tab->entries[i]);
            }
            return 0;
        }
        if (nm->code == LVN_COLUMNCLICK &&
            (from == g_app.list_a || from == g_app.list_b)) {
            auto* col = reinterpret_cast<NMLISTVIEW*>(l);
            TabState* tab = ActiveTab();
            if (tab) {
                static const wchar_t* keys[] = { L"name", L"size", L"type", L"date" };
                std::wstring key = keys[col->iSubItem];
                if (tab->sort_key == key) tab->sort_reverse = !tab->sort_reverse;
                else { tab->sort_key = key; tab->sort_reverse = false; }
                g_app.config.settings.sort_key = tab->sort_key;
                g_app.config.settings.sort_reverse = tab->sort_reverse;
                StartListing(tab);
            }
            return 0;
        }
        if (nm->code == LVN_ITEMACTIVATE &&
            (from == g_app.list_a || from == g_app.list_b)) {
            auto* ia = reinterpret_cast<NMITEMACTIVATE*>(l);
            TabState* tab = ActiveTab();
            if (tab && ia->iItem >= 0 &&
                ia->iItem < static_cast<int>(tab->entries.size()))
                OpenEntry(tab->entries[ia->iItem]);
            return 0;
        }
        if (nm->code == LVN_ITEMCHANGED &&
            (from == g_app.list_a || from == g_app.list_b)) {
            UpdateInspector();
            return 0;
        }
        if (nm->code == LVN_BEGINDRAG &&
            (from == g_app.list_a || from == g_app.list_b)) {
            auto entries = SelectedEntries(from);
            g_app.drag_paths.clear();
            for (const auto& e : entries) g_app.drag_paths.push_back(e.path);
            if (!g_app.drag_paths.empty()) {
                g_app.dragging = true;
                SetCapture(from);
            }
            return 0;
        }
        if (nm->code == NM_CUSTOMDRAW &&
            (from == g_app.list_a || from == g_app.list_b)) {
            return HandleCustomDraw(nm, from);
        }
        if (nm->code == NM_RCLICK &&
            (from == g_app.list_a || from == g_app.list_b)) {
            POINT pt{};
            GetCursorPos(&pt);
            ShowEntryContextMenu(from, pt);
            return 0;
        }
        if (nm->code == TCN_SELCHANGE && from == g_app.tab) {
            g_app.active_tab = TabCtrl_GetCurSel(g_app.tab);
            TabState* t = ActiveTab();
            if (t) {
                SetWindowTextW(g_app.goto_edit, t->path.c_str());
                StartListing(t);
            }
            return 0;
        }
        if (nm->code == TVN_SELCHANGEDW && from == g_app.tree) {
            auto* sel = reinterpret_cast<NMTREEVIEWW*>(l);
            auto it = g_tree_paths.find(sel->itemNew.hItem);
            if (it != g_tree_paths.end()) {
                TabState* t = ActiveTab();
                if (t) Navigate(t, it->second);
            }
            return 0;
        }
        if (nm->code == TVN_SELCHANGEDW && from == g_center_tree) {
            auto* sel = reinterpret_cast<NMTREEVIEWW*>(l);
            auto it = g_center_tree_paths.find(sel->itemNew.hItem);
            if (it != g_center_tree_paths.end()) {
                TabState* t = ActiveTab();
                if (t) Navigate(t, it->second);
            }
            return 0;
        }
        if (nm->code == TVN_ITEMEXPANDINGW && from == g_center_tree) {
            auto* exp = reinterpret_cast<NMTREEVIEWW*>(l);
            if (exp->action == TVE_EXPAND) {
                auto it = g_center_tree_paths.find(exp->itemNew.hItem);
                if (it != g_center_tree_paths.end()) {
                    // Load one more level under the expanded node.
                    TVITEMW child{};
                    HTREEITEM h = TreeView_GetChild(g_center_tree, exp->itemNew.hItem);
                    if (h) TreeView_DeleteItem(g_center_tree, h);
                    for (const auto& e : ListDirectory(it->second, false)) {
                        if (!e.is_dir) continue;
                        TVINSERTSTRUCTW ins{};
                        ins.hParent = exp->itemNew.hItem;
                        ins.hInsertAfter = TVI_LAST;
                        ins.item.mask = TVIF_TEXT | TVIF_CHILDREN;
                        ins.item.pszText = const_cast<LPWSTR>(e.name.c_str());
                        ins.item.cChildren = 1;
                        HTREEITEM node = TreeView_InsertItem(g_center_tree, &ins);
                        g_center_tree_paths[node] = e.path;
                    }
                }
            }
            return 0;
        }
        break;
    }

    case WM_HSCROLL: {
        HWND slider = GetDlgItem(hwnd, 201);
        if (reinterpret_cast<HWND>(l) == slider) {
            int size = static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0));
            g_app.config.settings.icon_size = size;
            ListView_SetIconSpacing(g_app.list_a, size + 16, size + 24);
            ListView_SetIconSpacing(g_app.list_b, size + 16, size + 24);
        }
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(w);
        int code = HIWORD(w);
        if (reinterpret_cast<HWND>(l) == g_app.view_box && code == CBN_SELCHANGE) {
            int sel = static_cast<int>(SendMessageW(g_app.view_box, CB_GETCURSEL, 0, 0));
            static const wchar_t* modes[] = { L"details", L"list", L"icons", L"tree" };
            std::wstring mode = modes[sel < 0 ? 0 : sel];
            TabState* t = ActiveTab();
            if (t) t->view_mode = mode;
            g_app.config.settings.view_mode = mode;
            SetListViewMode(g_app.list_a, mode);
            SetListViewMode(g_app.list_b, mode);
            bool tree_mode = (mode == L"tree");
            ShowWindow(g_center_tree, tree_mode ? SW_SHOW : SW_HIDE);
            ShowWindow(g_app.list_a, tree_mode ? SW_HIDE : SW_SHOW);
            if (tree_mode && t) PopulateCenterTree(t->path);
            LayoutChildren();
            return 0;
        }
        switch (id) {
        case IDM_NEW_TAB: {
            wchar_t home[MAX_PATH] = {};
            SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, home);
            AddTab(home);
            break;
        }
        case IDM_CLOSE_TAB: CloseTab(g_app.active_tab); break;
        case IDM_EXIT: DestroyWindow(hwnd); break;
        case IDM_DUAL:
            g_app.dual = !g_app.dual;
            g_app.config.settings.dual_panel = g_app.dual;
            LayoutChildren();
            break;
        case IDM_HIDDEN:
            g_app.config.settings.show_hidden = !g_app.config.settings.show_hidden;
            if (ActiveTab()) StartListing(ActiveTab());
            break;
        case IDM_THEME:
            g_app.dark = !g_app.dark;
            g_app.config.settings.theme = g_app.dark ? L"dark" : L"light";
            InvalidateRect(hwnd, nullptr, TRUE);
            break;
        case IDM_GOTO: SetFocus(g_app.goto_edit); break;
        case IDM_SEARCH: SetFocus(g_app.filter_edit); break;
        case IDM_PALETTE: ShowCommandPalette(hwnd, BuildCommands()); break;
        case IDM_BATCH_RENAME: {
            TabState* t = ActiveTab();
            if (t) {
                auto sel = SelectedEntries(ActiveList());
                if (ShowBatchRename(hwnd, sel.empty() ? t->entries : sel))
                    StartListing(t);
            }
            break;
        }
        case IDM_SETTINGS:
            if (ShowSettings(hwnd, g_app.config)) {
                g_app.dark = g_app.config.settings.theme == L"dark";
                if (ActiveTab()) StartListing(ActiveTab());
                LayoutChildren();
            }
            break;
        case IDM_ABOUT:
            MessageBoxW(hwnd, L"TNxPilot 1.0.0\nFast, fully offline file explorer.\nPrivate use only.",
                        L"About TNxPilot", MB_ICONINFORMATION);
            break;
        case IDM_BACK: { TabState* t = ActiveTab();
            if (t && t->hist_index > 0) { t->hist_index--; t->path = t->history[t->hist_index]; StartListing(t); }
            break; }
        case IDM_FWD: { TabState* t = ActiveTab();
            if (t && t->hist_index + 1 < t->history.size()) { t->hist_index++; t->path = t->history[t->hist_index]; StartListing(t); }
            break; }
        case IDM_UP: { TabState* t = ActiveTab();
            if (t) { std::wstring p = t->path; size_t s = p.find_last_of(L"\\/");
                if (s != std::wstring::npos && s > 0) Navigate(t, p.substr(0, s)); }
            break; }
        case IDM_REFRESH: if (ActiveTab()) StartListing(ActiveTab()); break;
        case IDM_COPY: case IDM_CUT: {
            // If focus is in an edit box, let the edit do the copy/cut.
            HWND focus = GetFocus();
            if (focus == g_app.goto_edit || focus == g_app.filter_edit) {
                SendMessageW(focus, id == IDM_CUT ? WM_CUT : WM_COPY, 0, 0);
                break;
            }
            auto sel = SelectedEntries(ActiveList());
            g_app.clip_paths.clear();
            for (const auto& e : sel) g_app.clip_paths.push_back(e.path);
            g_app.clip_cut = (id == IDM_CUT);
            break;
        }
        case IDM_PASTE: {
            HWND focus = GetFocus();
            if (focus == g_app.goto_edit || focus == g_app.filter_edit) {
                SendMessageW(focus, WM_PASTE, 0, 0);
                break;
            }
            DoPaste();
            break;
        }
        case IDM_NEW_FOLDER: case IDM_NEW_FILE: {
            TabState* t = ActiveTab();
            if (!t) break;
            std::wstring name = PromptText(hwnd,
                id == IDM_NEW_FOLDER ? L"New Folder" : L"New File",
                id == IDM_NEW_FOLDER ? L"Folder name:" : L"File name:");
            if (name.empty()) break;
            std::wstring full = t->path + L"\\" + name;
            if (id == IDM_NEW_FOLDER) CreateDirectoryW(full.c_str(), nullptr);
            else { HANDLE h = CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr,
                                          CREATE_NEW, 0, nullptr);
                   if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
            StartListing(t);
            break;
        }
        case IDM_DELETE: {
            auto sel = SelectedEntries(ActiveList());
            TabState* t = ActiveTab();
            if (!sel.empty() && t &&
                MessageBoxW(hwnd, L"Permanently delete the selected item(s)?",
                            L"Delete", MB_YESNO | MB_ICONWARNING) == IDYES) {
                std::vector<std::wstring> paths;
                for (const auto& e : sel) paths.push_back(e.path);
                SafeDelete(paths);
                StartListing(t);
            }
            break;
        }
        case IDM_RENAME: {
            auto sel = SelectedEntries(ActiveList());
            TabState* t = ActiveTab();
            if (!sel.empty() && t) {
                std::wstring name = PromptText(hwnd, L"Rename", L"New name:", sel[0].name);
                if (!name.empty() && name != sel[0].name) {
                    std::wstring dst = sel[0].path;
                    size_t s = dst.find_last_of(L"\\/");
                    dst = dst.substr(0, s + 1) + name;
                    MoveFileExW(sel[0].path.c_str(), dst.c_str(), 0);
                    StartListing(t);
                }
            }
            break;
        }
        }
        return 0;
    }

    case WM_LBUTTONUP:
        if (g_app.dragging) {
            g_app.dragging = false;
            ReleaseCapture();
            POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
            ClientToScreen(g_app.hwnd, &pt);
            HWND target = WindowFromPoint(pt);
            if ((target == g_app.list_a || target == g_app.list_b) &&
                !g_app.drag_paths.empty()) {
                int ans = MessageBoxW(g_app.hwnd,
                    L"Copy the dragged item(s) here?\n(Yes = Copy, No = Move)",
                    L"TNxPilot", MB_YESNOCANCEL | MB_ICONQUESTION);
                if (ans == IDYES || ans == IDNO) {
                    TabState* t = ActiveTab();
                    if (t) {
                        CopyJob job{ g_app.drag_paths, t->path, ans == IDNO };
                        RunCopyJob(g_app.hwnd, job);
                        StartListing(t);
                    }
                }
            }
            g_app.drag_paths.clear();
        }
        return 0;

    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(w);
        UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        std::vector<std::wstring> files;
        for (UINT i = 0; i < count; ++i) {
            wchar_t buf[MAX_PATH] = {};
            DragQueryFileW(drop, i, buf, MAX_PATH);
            files.emplace_back(buf);
        }
        DragFinish(drop);
        TabState* t = ActiveTab();
        if (t && !files.empty()) {
            CopyJob job{ files, t->path, false };
            RunCopyJob(g_app.hwnd, job);
            StartListing(t);
        }
        return 0;
    }

    case WM_LIST_DONE: {
        auto* res = reinterpret_cast<ListResult*>(l);
        TabState* tab = ActiveTab();
        if (tab && res->seq == tab->seq->load() && res->path == tab->path) {
            tab->entries = std::move(res->entries);
            RefreshCurrentView();
        }
        delete res;
        return 0;
    }
    case WM_SIZE_DONE: {
        auto* text = reinterpret_cast<std::wstring*>(l);
        SetWindowTextW(g_app.insp_info, text->c_str());
        delete text;
        return 0;
    }
    case WM_SEARCH_HIT: {
        auto* batch = reinterpret_cast<std::vector<Entry>*>(l);
        TabState* tab = ActiveTab();
        if (tab) {
            for (auto& e : *batch) tab->entries.push_back(std::move(e));
            RefreshCurrentView();
        }
        delete batch;
        return 0;
    }
    case WM_SEARCH_DONE:
        SetStatus(L"Search finished.");
        return 0;

    case WM_DESTROY: {
        std::vector<std::wstring> paths;
        for (const auto& t : g_app.tabs) paths.push_back(t->path);
        g_app.config.SaveSession(paths, g_app.active_tab);
        g_app.config.Save();
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

// ------------------------------------------------------------------ WinMain
int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR, int nCmdShow) {
    g_app.hinst = hinst;
    g_app.config = Config(DefaultBaseDir());
    g_app.config.Load();

    // GDI+ for image previews (PNG/JPG/etc.).
    GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken = 0;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hinst;
    wc.lpszClassName = L"TNxPilotMain";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);

    WNDCLASSW ivc{};
    ivc.lpfnWndProc = ImageViewProc;
    ivc.hInstance = hinst;
    ivc.lpszClassName = L"TNxImageView";
    ivc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    ivc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassW(&ivc);

    HWND hwnd = CreateWindowExW(0, L"TNxPilotMain",
        L"TNxPilot 1.0.0 — Offline File Explorer",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800,
        nullptr, nullptr, hinst, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // Accelerators: Ctrl+T/W, Ctrl+Shift+P, Ctrl+L, Ctrl+D, Ctrl+F, F2, F5.
    ACCEL accels[] = {
        { FCONTROL | FVIRTKEY, 'T', IDM_NEW_TAB },
        { FCONTROL | FVIRTKEY, 'W', IDM_CLOSE_TAB },
        { FCONTROL | FSHIFT | FVIRTKEY, 'P', IDM_PALETTE },
        { FCONTROL | FVIRTKEY, 'L', IDM_GOTO },
        { FCONTROL | FVIRTKEY, 'D', IDM_DUAL },
        { FCONTROL | FVIRTKEY, 'F', IDM_SEARCH },
        { FCONTROL | FVIRTKEY, 'C', IDM_COPY },
        { FCONTROL | FVIRTKEY, 'X', IDM_CUT },
        { FCONTROL | FVIRTKEY, 'V', IDM_PASTE },
        { FVIRTKEY, VK_F2, IDM_RENAME },
        { FVIRTKEY, VK_F5, IDM_REFRESH },
        { FVIRTKEY, VK_DELETE, IDM_DELETE },
        { FALT | FVIRTKEY, VK_LEFT, IDM_BACK },
        { FALT | FVIRTKEY, VK_RIGHT, IDM_FWD },
        { FALT | FVIRTKEY, VK_UP, IDM_UP },
    };
    HACCEL haccel = CreateAcceleratorTableW(accels,
        static_cast<int>(sizeof(accels) / sizeof(accels[0])));

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!TranslateAcceleratorW(hwnd, haccel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    DestroyAcceleratorTable(haccel);
    GdiplusShutdown(gdiplusToken);
    return static_cast<int>(msg.wParam);
}
