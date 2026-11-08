// TNxPilot — dialogs.cpp
// Code-built modal dialogs. A tiny ModalDialog base runs a nested
// message loop; each dialog creates its child controls in OnCreate and
// handles commands in OnCommand.
#include "dialogs.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>

#include "util.h"

namespace tnx {

// ---------------------------------------------------------------- helpers
static HFONT g_font = nullptr;
HFONT DialogFont() {
    if (!g_font) {
        NONCLIENTMETRICSW ncm{ sizeof(ncm) };
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    }
    return g_font;
}

static void ApplyFontToChildren(HWND parent) {
    EnumChildWindows(parent, [](HWND child, LPARAM) -> BOOL {
        SendMessageW(child, WM_SETFONT, (WPARAM)DialogFont(), TRUE);
        return TRUE;
    }, 0);
}

HWND CreateLabel(HWND parent, int x, int y, int w, const wchar_t* text) {
    return CreateWindowExW(0, L"STATIC", text,
                           WS_CHILD | WS_VISIBLE | SS_LEFT,
                           x, y, w, 18, parent, nullptr,
                           GetModuleHandleW(nullptr), nullptr);
}

HWND CreateButton(HWND parent, int x, int y, int w, const wchar_t* text, int id) {
    return CreateWindowExW(0, L"BUTTON", text,
                           WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                           x, y, w, 26, parent, (HMENU)(INT_PTR)id,
                           GetModuleHandleW(nullptr), nullptr);
}

HWND CreateEdit(HWND parent, int x, int y, int w, const wchar_t* text) {
    return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text,
                           WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                           x, y, w, 24, parent, nullptr,
                           GetModuleHandleW(nullptr), nullptr);
}

std::wstring GetEditText(HWND edit) {
    int len = GetWindowTextLengthW(edit);
    std::wstring out(len, L'\0');
    GetWindowTextW(edit, out.data(), len + 1);
    return out;
}

void SetEditText(HWND edit, const std::wstring& text) {
    SetWindowTextW(edit, text.c_str());
}

// ---------------------------------------------------------------- base
class ModalDialog {
public:
    HWND hwnd = nullptr;
    int result = 0; // 1 = OK/apply, 0 = cancel
    virtual ~ModalDialog() = default;
    virtual void OnCreate() {}
    virtual void OnCommand(int id, UINT code) {
        if (code == BN_CLICKED && (id == IDOK || id == IDCANCEL))
            Close(id == IDOK);
    }
    virtual void OnTimer() {}
    virtual void OnSize() {}
    void Close(bool ok) { result = ok ? 1 : 0; DestroyWindow(hwnd); }
    int ShowModal(HWND owner, const wchar_t* title, int w, int h) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = &ModalDialog::WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"TNxPilotDialog";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        RegisterClassW(&wc);
        RECT rc{};
        GetWindowRect(owner, &rc);
        int x = rc.left + (rc.right - rc.left - w) / 2;
        int y = rc.top + (rc.bottom - rc.top - h) / 2;
        hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"TNxPilotDialog", title,
                               WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
                               x, y, w, h, owner, nullptr,
                               GetModuleHandleW(nullptr), this);
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
        MSG msg;
        while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0)) {
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) { Close(false); continue; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return result;
    }
    static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        ModalDialog* self = nullptr;
        if (m == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
            self = static_cast<ModalDialog*>(cs->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd = h;
        } else {
            self = reinterpret_cast<ModalDialog*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        }
        if (!self) return DefWindowProcW(h, m, w, l);
        switch (m) {
        case WM_CREATE: self->OnCreate(); ApplyFontToChildren(h); return 0;
        case WM_COMMAND:
            self->OnCommand(LOWORD(w), HIWORD(w));
            return 0;
        case WM_TIMER: self->OnTimer(); return 0;
        case WM_SIZE: self->OnSize(); return 0;
        case WM_CLOSE: self->Close(false); return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }
};

// ---------------------------------------------------------------- command palette
class PaletteDialog : public ModalDialog {
public:
    const std::vector<Command>& commands;
    std::vector<const Command*> visible;
    HWND edit = nullptr, list = nullptr;
    explicit PaletteDialog(const std::vector<Command>& c) : commands(c) {}
    void OnCreate() override {
        edit = CreateEdit(hwnd, 12, 12, 460);
        SendMessageW(edit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Type a command…");
        list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
                               WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY,
                               12, 44, 460, 260, hwnd, (HMENU)1001,
                               GetModuleHandleW(nullptr), nullptr);
        SetFocus(edit);
        Filter();
    }
    void Filter() {
        std::wstring q = GetEditText(edit);
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        visible.clear();
        for (const auto& c : commands) {
            if (q.empty() || FuzzyScore(q, c.name) >= 0) {
                visible.push_back(&c);
                std::wstring label = c.name + (c.hotkey.empty() ? L"" : L"    [" + c.hotkey + L"]");
                SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)label.c_str());
            }
        }
        if (!visible.empty()) SendMessageW(list, LB_SETCURSEL, 0, 0);
    }
    void RunSelected() {
        int sel = static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0));
        if (sel >= 0 && sel < static_cast<int>(visible.size())) {
            const Command* cmd = visible[sel];
            Close(true);
            cmd->run();
        }
    }
    void OnCommand(int id, UINT code) override {
        if (id == 1001 && code == LBN_DBLCLK) { // listbox double-click only
            RunSelected();
        } else if (code == EN_CHANGE) {
            Filter();
        }
    }
};

bool ShowCommandPalette(HWND owner, const std::vector<Command>& commands) {
    PaletteDialog dlg(commands);
    return dlg.ShowModal(owner, L"Command Palette — TNxPilot", 500, 360) == 1;
}

// ---------------------------------------------------------------- batch rename
class RenameDialog : public ModalDialog {
public:
    std::vector<Entry> entries;
    std::wstring dir;
    HWND mode = nullptr, find_edit = nullptr, replace_edit = nullptr;
    HWND start_spin = nullptr, list = nullptr, warn = nullptr;
    RenameDialog(const std::vector<Entry>& e, const std::wstring& d)
        : entries(e), dir(d) {}
    void OnCreate() override {
        CreateLabel(hwnd, 12, 12, 80, L"Mode:");
        mode = CreateWindowExW(0, L"COMBOBOX", nullptr,
                               WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                               96, 10, 200, 200, hwnd, (HMENU)1001,
                               GetModuleHandleW(nullptr), nullptr);
        const wchar_t* modes[] = { L"Find & Replace", L"Add Prefix", L"Add Suffix",
                                   L"Numbering", L"Add Date", L"Remove Text" };
        for (auto* m : modes) SendMessageW(mode, CB_ADDSTRING, 0, (LPARAM)m);
        SendMessageW(mode, CB_SETCURSEL, 0, 0);
        CreateLabel(hwnd, 12, 44, 80, L"Find / Text:");
        find_edit = CreateEdit(hwnd, 96, 42, 200);
        CreateLabel(hwnd, 12, 76, 80, L"Replace:");
        replace_edit = CreateEdit(hwnd, 96, 74, 200);
        CreateLabel(hwnd, 320, 12, 90, L"Start at:");
        start_spin = CreateWindowExW(0, L"EDIT", L"1",
                                     WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
                                     400, 10, 60, 24, hwnd, nullptr,
                                     GetModuleHandleW(nullptr), nullptr);
        list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
                               WS_CHILD | WS_VISIBLE | WS_VSCROLL,
                               12, 108, 560, 220, hwnd, (HMENU)1002,
                               GetModuleHandleW(nullptr), nullptr);
        warn = CreateLabel(hwnd, 12, 336, 560, L"");
        CreateButton(hwnd, 380, 362, 90, L"Apply", IDOK);
        CreateButton(hwnd, 482, 362, 90, L"Cancel", IDCANCEL);
        Refresh();
    }
    RenameRule CurrentRule() const {
        static const wchar_t* modes[] = { L"find_replace", L"prefix", L"suffix",
                                          L"numbering", L"date", L"remove" };
        int sel = static_cast<int>(SendMessageW(mode, CB_GETCURSEL, 0, 0));
        RenameRule r;
        r.mode = modes[sel < 0 ? 0 : sel];
        r.find = GetEditText(find_edit);
        r.replace = GetEditText(replace_edit);
        r.text = r.find;
        r.start = _wtoi(GetEditText(start_spin).c_str());
        r.digits = 3;
        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t buf[32];
        swprintf(buf, 32, L"%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
        r.date_text = buf;
        return r;
    }
    void Refresh() {
        std::vector<std::pair<std::wstring, bool>> src;
        for (const auto& e : entries) src.emplace_back(e.name, e.is_dir);
        auto preview = RenamePreview(src, CurrentRule());
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        std::vector<std::wstring> news;
        for (const auto& [old_name, new_name] : preview) {
            std::wstring line = old_name + L"  →  " + new_name;
            SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)line.c_str());
            news.push_back(new_name);
        }
        std::sort(news.begin(), news.end());
        bool dup = std::adjacent_find(news.begin(), news.end()) != news.end();
        int changed = 0;
        for (const auto& [o, n] : preview) if (o != n) ++changed;
        SetWindowTextW(warn, dup
            ? L"⚠ Duplicate new names — fix the rule before applying."
            : (std::to_wstring(changed) + L" file(s) will change.").c_str());
    }
    void OnCommand(int id, UINT code) override {
        if (id == IDOK && code == BN_CLICKED) {
            std::vector<std::pair<std::wstring, bool>> src;
            for (const auto& e : entries) src.emplace_back(e.name, e.is_dir);
            auto preview = RenamePreview(src, CurrentRule());
            int failed = 0;
            for (size_t i = 0; i < entries.size(); ++i) {
                if (preview[i].first == preview[i].second) continue;
                std::wstring dst = dir;
                if (!dst.empty() && dst.back() != L'\\') dst += L"\\";
                dst += preview[i].second;
                if (!MoveFileExW(entries[i].path.c_str(), dst.c_str(), 0)) ++failed;
            }
            if (failed > 0)
                MessageBoxW(hwnd, L"Some renames failed (name in use?).",
                            L"Batch Rename", MB_ICONWARNING);
            Close(true);
            return;
        }
        if (id == IDCANCEL && code == BN_CLICKED) { Close(false); return; }
        if (code == EN_CHANGE || code == CBN_SELCHANGE) Refresh(); // live preview
    }
};

bool ShowBatchRename(HWND owner, const std::vector<Entry>& entries) {
    if (entries.empty() || entries[0].path.empty()) return false;
    std::wstring dir = entries[0].path;
    size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir = dir.substr(0, slash);
    RenameDialog dlg(entries, dir);
    return dlg.ShowModal(owner, L"Batch Rename — TNxPilot", 600, 440) == 1;
}

// ---------------------------------------------------------------- settings
class SettingsDialog : public ModalDialog {
public:
    Config& cfg;
    HWND theme = nullptr, font_edit = nullptr, icon_edit = nullptr;
    HWND hidden = nullptr, restore = nullptr, dual = nullptr;
    explicit SettingsDialog(Config& c) : cfg(c) {}
    void OnCreate() override {
        CreateLabel(hwnd, 12, 12, 100, L"Theme:");
        theme = CreateWindowExW(0, L"COMBOBOX", nullptr,
                                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                                120, 10, 140, 120, hwnd, (HMENU)1001,
                                GetModuleHandleW(nullptr), nullptr);
        SendMessageW(theme, CB_ADDSTRING, 0, (LPARAM)L"dark");
        SendMessageW(theme, CB_ADDSTRING, 0, (LPARAM)L"light");
        SendMessageW(theme, CB_SETCURSEL,
                     cfg.settings.theme == L"light" ? 1 : 0, 0);
        CreateLabel(hwnd, 12, 44, 100, L"Font size:");
        font_edit = CreateEdit(hwnd, 120, 42, 60,
                               std::to_wstring(cfg.settings.font_size).c_str());
        CreateLabel(hwnd, 12, 76, 100, L"Icon size:");
        icon_edit = CreateEdit(hwnd, 120, 74, 60,
                               std::to_wstring(cfg.settings.icon_size).c_str());
        hidden = CreateWindowExW(0, L"BUTTON", L"Show hidden files",
                                 WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                 12, 108, 200, 22, hwnd, (HMENU)1002,
                                 GetModuleHandleW(nullptr), nullptr);
        restore = CreateWindowExW(0, L"BUTTON", L"Restore tabs on launch",
                                  WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                  12, 134, 200, 22, hwnd, (HMENU)1003,
                                  GetModuleHandleW(nullptr), nullptr);
        dual = CreateWindowExW(0, L"BUTTON", L"Dual panel by default",
                               WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                               12, 160, 200, 22, hwnd, (HMENU)1004,
                               GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hidden, BM_SETCHECK, cfg.settings.show_hidden ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(restore, BM_SETCHECK, cfg.settings.restore_session ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(dual, BM_SETCHECK, cfg.settings.dual_panel ? BST_CHECKED : BST_UNCHECKED, 0);
        CreateLabel(hwnd, 12, 196, 360,
                    L"Hotkeys: Ctrl+T tab · Ctrl+W close · Ctrl+Shift+P palette · Ctrl+L GoTo · F2 rename · F5 refresh");
        CreateButton(hwnd, 200, 230, 90, L"OK", IDOK);
        CreateButton(hwnd, 300, 230, 90, L"Cancel", IDCANCEL);
    }
    void OnCommand(int id, UINT code) override {
        if (id == IDOK && code == BN_CLICKED) {
            wchar_t buf[32] = {};
            GetWindowTextW(theme, buf, 32);
            cfg.settings.theme = buf;
            cfg.settings.font_size = _wtoi(GetEditText(font_edit).c_str());
            cfg.settings.icon_size = _wtoi(GetEditText(icon_edit).c_str());
            cfg.settings.show_hidden = SendMessageW(hidden, BM_GETCHECK, 0, 0) == BST_CHECKED;
            cfg.settings.restore_session = SendMessageW(restore, BM_GETCHECK, 0, 0) == BST_CHECKED;
            cfg.settings.dual_panel = SendMessageW(dual, BM_GETCHECK, 0, 0) == BST_CHECKED;
            cfg.Save();
            Close(true);
        } else if (id == IDCANCEL && code == BN_CLICKED) {
            Close(false);
        }
    }
};

bool ShowSettings(HWND owner, Config& config) {
    SettingsDialog dlg(config);
    return dlg.ShowModal(owner, L"Settings — TNxPilot", 420, 300) == 1;
}

// ---------------------------------------------------------------- copy progress
struct CopyState {
    HWND hwnd = nullptr;
    HWND bar = nullptr, label = nullptr, pause_btn = nullptr;
    std::atomic<bool> stop{ false };
    std::atomic<bool> pause{ false };
    std::atomic<int64_t> done{ 0 };
    std::atomic<int64_t> total{ 0 };
    bool finished = false;
    bool ok = false;
};

static DWORD WINAPI CopyThreadProc(LPVOID param) {
    auto* st = static_cast<CopyState*>(param);
    // Job data is stored on the state via thread-local globals set by
    // RunCopyJob before CreateThread (single job at a time by design).
    extern CopyJob g_copy_job;
    try {
        for (const auto& src : g_copy_job.sources) {
            if (st->stop.load()) break;
            std::wstring name = src;
            size_t slash = name.find_last_of(L"\\/");
            if (slash != std::wstring::npos) name = name.substr(slash + 1);
            std::wstring dst = g_copy_job.dest_dir;
            if (!dst.empty() && dst.back() != L'\\') dst += L"\\";
            dst += name;
            CopyWithProgress(src, dst,
                [&](int64_t d, int64_t t) {
                    st->done.store(d);
                    st->total.store(t);
                    if (st->hwnd) PostMessageW(st->hwnd, WM_USER + 1, 0, 0);
                },
                st->stop, st->pause);
            if (g_copy_job.move && !st->stop.load())
                SafeDelete({ src });
        }
        st->ok = !st->stop.load();
    } catch (const CopyCancelled&) {
        st->ok = false;
    } catch (...) {
        st->ok = false;
    }
    st->finished = true;
    if (st->hwnd) PostMessageW(st->hwnd, WM_USER + 2, 0, 0);
    return 0;
}

CopyJob g_copy_job; // single active copy job (see CopyThreadProc)

class CopyDialog : public ModalDialog {
public:
    CopyState state;
    DWORD start_tick = 0;
    void OnCreate() override {
        start_tick = GetTickCount();
        state.hwnd = hwnd;
        CreateLabel(hwnd, 12, 12, 460, L"Copying…");
        state.bar = CreateWindowExW(0, L"PROGRESS_CLASS", nullptr,
                                    WS_CHILD | WS_VISIBLE,
                                    12, 40, 460, 22, hwnd, nullptr,
                                    GetModuleHandleW(nullptr), nullptr);
        state.label = CreateLabel(hwnd, 12, 72, 460, L"Starting…");
        state.pause_btn = CreateButton(hwnd, 12, 104, 100, L"Pause", 1001);
        CreateButton(hwnd, 372, 104, 100, L"Cancel", IDCANCEL);
        SetTimer(hwnd, 1, 200, nullptr);
    }
    void OnCommand(int id, UINT code) override {
        if (id == 1001 && code == BN_CLICKED) {
            bool p = !state.pause.load();
            state.pause.store(p);
            SetWindowTextW(state.pause_btn, p ? L"Resume" : L"Pause");
        } else if (id == IDCANCEL && code == BN_CLICKED) {
            state.stop.store(true);
            EnableWindow(GetDlgItem(hwnd, IDCANCEL), FALSE);
        }
    }
    void OnTimer() override { Tick(); }
    void Tick() {
        int64_t done = state.done.load(), total = state.total.load();
        if (total > 0) {
            SendMessageW(state.bar, PBM_SETPOS,
                         static_cast<WPARAM>(done * 100 / total), 0);
            DWORD elapsed = GetTickCount() - start_tick;
            int64_t speed = elapsed > 0 ? done * 1000 / elapsed : 0;
            wchar_t buf[256];
            swprintf(buf, 256, L"%ls / %ls — %ls/s (%lld%%)",
                     FormatSize(done).c_str(), FormatSize(total).c_str(),
                     FormatSize(speed).c_str(),
                     static_cast<long long>(done * 100 / total));
            SetWindowTextW(state.label, buf);
        }
        if (state.finished) Close(state.ok);
    }
};

void RunCopyJob(HWND owner, const CopyJob& job) {
    g_copy_job = job;
    CopyDialog dlg;
    HANDLE thread = CreateThread(nullptr, 0, CopyThreadProc, &dlg.state, 0, nullptr);
    if (!thread) return;
    // Pump progress via timer: hook WM_TIMER by subclassing the dialog
    // proc is complex; instead the dialog polls in its message loop
    // through WM_TIMER handled in WndProc below (timer id 1).
    dlg.ShowModal(owner, job.move ? L"Moving… — TNxPilot" : L"Copying… — TNxPilot",
                  500, 180);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
}

} // namespace tnx
