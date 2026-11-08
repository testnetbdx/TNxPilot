// TNxPilot — config.h
// config.json / session.json persistence. Plain JSON, local files only,
// no network. The parser is intentionally minimal: it only reads files
// this app itself wrote (flat settings object + string arrays).
#pragma once

#include <string>
#include <vector>

namespace tnx {

struct Settings {
    std::wstring theme = L"dark";   // dark | light
    int font_size = 10;
    int icon_size = 48;
    std::wstring view_mode = L"details"; // details | list | icons | tree
    bool show_hidden = false;
    bool dual_panel = false;
    bool restore_session = true;
    std::wstring sort_key = L"name";
    bool sort_reverse = false;
};

// Minimal JSON string escaping for our own files.
std::wstring JsonEscape(const std::wstring& s);
std::wstring JsonUnescape(const std::wstring& s);

// Read one string value ("key": "value") from a flat JSON object body.
// Returns false when the key is absent.
bool JsonGetString(const std::wstring& body, const std::wstring& key,
                   std::wstring& out);
// Read one bool / int value.
bool JsonGetBool(const std::wstring& body, const std::wstring& key, bool& out);
bool JsonGetInt(const std::wstring& body, const std::wstring& key, int& out);
// Read a string array ("key": ["a", "b"]).
bool JsonGetStringArray(const std::wstring& body, const std::wstring& key,
                        std::vector<std::wstring>& out);

class Config {
public:
    explicit Config(const std::wstring& base_dir);
    bool Load();
    bool Save() const;
    bool LoadSession(std::vector<std::wstring>& tabs, int& active_tab) const;
    bool SaveSession(const std::vector<std::wstring>& tabs, int active_tab) const;

    void PushRecent(const std::wstring& path, size_t limit = 20);
    bool AddBookmark(const std::wstring& path);
    void RemoveBookmark(const std::wstring& path);

    std::wstring base_dir;
    std::wstring config_path;
    std::wstring session_path;
    Settings settings;
    std::vector<std::wstring> bookmarks;
    std::vector<std::wstring> recents;
};

std::wstring DefaultBaseDir(); // dir of the exe (frozen) / cwd fallback

} // namespace tnx
