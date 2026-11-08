// TNxPilot — config.cpp
// Plain-JSON persistence, std::fstream only. Compiles on any platform
// (the test harness builds this file on Linux).
#include "config.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace tnx {

std::wstring JsonEscape(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size() + 8);
    for (wchar_t c : s) {
        switch (c) {
        case L'"': out += L"\\\""; break;
        case L'\\': out += L"\\\\"; break;
        case L'\n': out += L"\\n"; break;
        case L'\r': out += L"\\r"; break;
        case L'\t': out += L"\\t"; break;
        default: out += c; break;
        }
    }
    return out;
}

std::wstring JsonUnescape(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'\\' && i + 1 < s.size()) {
            wchar_t n = s[++i];
            switch (n) {
            case L'n': out += L'\n'; break;
            case L'r': out += L'\r'; break;
            case L't': out += L'\t'; break;
            default: out += n; break;
            }
        } else {
            out += s[i];
        }
    }
    return out;
}

// Find `"key"` then parse the value that follows the ':'.
static const wchar_t* FindValue(const std::wstring& body,
                                const std::wstring& key) {
    std::wstring pat = L"\"" + key + L"\"";
    size_t pos = body.find(pat);
    if (pos == std::wstring::npos) return nullptr;
    pos = body.find(L':', pos + pat.size());
    if (pos == std::wstring::npos) return nullptr;
    ++pos;
    while (pos < body.size() && iswspace(body[pos])) ++pos;
    return pos < body.size() ? body.c_str() + pos : nullptr;
}

bool JsonGetString(const std::wstring& body, const std::wstring& key,
                   std::wstring& out) {
    const wchar_t* v = FindValue(body, key);
    if (!v || *v != L'"') return false;
    ++v;
    std::wstring raw;
    while (*v && *v != L'"') {
        if (*v == L'\\' && v[1]) raw += *v++;
        raw += *v++;
    }
    out = JsonUnescape(raw);
    return true;
}

bool JsonGetBool(const std::wstring& body, const std::wstring& key, bool& out) {
    const wchar_t* v = FindValue(body, key);
    if (!v) return false;
    if (wcsncmp(v, L"true", 4) == 0) { out = true; return true; }
    if (wcsncmp(v, L"false", 5) == 0) { out = false; return true; }
    return false;
}

bool JsonGetInt(const std::wstring& body, const std::wstring& key, int& out) {
    const wchar_t* v = FindValue(body, key);
    if (!v) return false;
    wchar_t* end = nullptr;
    long val = wcstol(v, &end, 10);
    if (end == v) return false;
    out = static_cast<int>(val);
    return true;
}

bool JsonGetStringArray(const std::wstring& body, const std::wstring& key,
                        std::vector<std::wstring>& out) {
    const wchar_t* v = FindValue(body, key);
    if (!v || *v != L'[') return false;
    ++v;
    out.clear();
    while (*v && *v != L']') {
        while (*v && (iswspace(*v) || *v == L',')) ++v;
        if (*v == L'"') {
            ++v;
            std::wstring raw;
            while (*v && *v != L'"') {
                if (*v == L'\\' && v[1]) raw += *v++;
                raw += *v++;
            }
            if (*v == L'"') ++v;
            out.push_back(JsonUnescape(raw));
        } else if (*v && *v != L']') {
            ++v;
        }
    }
    return true;
}

static bool ReadFileW(const std::wstring& path, std::wstring& out) {
#ifdef _WIN32
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string bytes((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
    // Our files are UTF-8. Convert (ASCII-superset fast path + BMP).
    out.clear();
    for (size_t i = 0; i < bytes.size();) {
        unsigned char c = static_cast<unsigned char>(bytes[i]);
        if (c < 0x80) { out += static_cast<wchar_t>(c); ++i; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < bytes.size()) {
            out += static_cast<wchar_t>(((c & 0x1F) << 6) |
                                        (bytes[i + 1] & 0x3F));
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < bytes.size()) {
            out += static_cast<wchar_t>(((c & 0x0F) << 12) |
                                        ((bytes[i + 1] & 0x3F) << 6) |
                                        (bytes[i + 2] & 0x3F));
            i += 3;
        } else {
            ++i; // skip malformed byte
        }
    }
    return true;
#else
    std::string narrow(path.begin(), path.end());
    std::wifstream in(narrow);
    if (!in) return false;
    std::wstring content((std::istreambuf_iterator<wchar_t>(in)),
                         std::istreambuf_iterator<wchar_t>());
    out = content;
    return true;
#endif
}

static bool WriteFileAtomic(const std::wstring& path, const std::wstring& content) {
#ifdef _WIN32
    std::wstring tmp = path + L".tmp";
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    // Write UTF-8.
    for (wchar_t wc : content) {
        unsigned cp = static_cast<unsigned>(wc);
        if (cp < 0x80) out.put(static_cast<char>(cp));
        else if (cp < 0x800) {
            out.put(static_cast<char>(0xC0 | (cp >> 6)));
            out.put(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.put(static_cast<char>(0xE0 | (cp >> 12)));
            out.put(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.put(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    out.close();
    if (!out) return false;
    return MoveFileExW(tmp.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::string narrow(path.begin(), path.end());
    std::string tmp = narrow + ".tmp";
    std::wofstream out(tmp, std::ios::trunc);
    if (!out) return false;
    out << content;
    out.close();
    if (!out) return false;
    return std::rename(tmp.c_str(), narrow.c_str()) == 0;
#endif
}

std::wstring DefaultBaseDir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf);
    size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
#else
    return L".";
#endif
}

Config::Config(const std::wstring& base_dir)
    : base_dir(base_dir),
      config_path(base_dir + L"/config.json"),
      session_path(base_dir + L"/session.json") {}

bool Config::Load() {
    std::wstring body;
    if (!ReadFileW(config_path, body)) return false;
    std::wstring s;
    bool b;
    int i;
    if (JsonGetString(body, L"theme", s)) settings.theme = s;
    if (JsonGetInt(body, L"font_size", i)) settings.font_size = i;
    if (JsonGetInt(body, L"icon_size", i)) settings.icon_size = i;
    if (JsonGetString(body, L"view_mode", s)) settings.view_mode = s;
    if (JsonGetBool(body, L"show_hidden", b)) settings.show_hidden = b;
    if (JsonGetBool(body, L"dual_panel", b)) settings.dual_panel = b;
    if (JsonGetBool(body, L"restore_session", b)) settings.restore_session = b;
    if (JsonGetString(body, L"sort_key", s)) settings.sort_key = s;
    if (JsonGetBool(body, L"sort_reverse", b)) settings.sort_reverse = b;
    JsonGetStringArray(body, L"bookmarks", bookmarks);
    JsonGetStringArray(body, L"recents", recents);
    return true;
}

bool Config::Save() const {
    std::wostringstream o;
    o << L"{\n  \"theme\": \"" << JsonEscape(settings.theme) << L"\",\n"
      << L"  \"font_size\": " << settings.font_size << L",\n"
      << L"  \"icon_size\": " << settings.icon_size << L",\n"
      << L"  \"view_mode\": \"" << JsonEscape(settings.view_mode) << L"\",\n"
      << L"  \"show_hidden\": " << (settings.show_hidden ? L"true" : L"false") << L",\n"
      << L"  \"dual_panel\": " << (settings.dual_panel ? L"true" : L"false") << L",\n"
      << L"  \"restore_session\": " << (settings.restore_session ? L"true" : L"false") << L",\n"
      << L"  \"sort_key\": \"" << JsonEscape(settings.sort_key) << L"\",\n"
      << L"  \"sort_reverse\": " << (settings.sort_reverse ? L"true" : L"false") << L",\n"
      << L"  \"bookmarks\": [";
    for (size_t k = 0; k < bookmarks.size(); ++k) {
        if (k) o << L", ";
        o << L"\"" << JsonEscape(bookmarks[k]) << L"\"";
    }
    o << L"],\n  \"recents\": [";
    for (size_t k = 0; k < recents.size(); ++k) {
        if (k) o << L", ";
        o << L"\"" << JsonEscape(recents[k]) << L"\"";
    }
    o << L"]\n}\n";
    return WriteFileAtomic(config_path, o.str());
}

bool Config::LoadSession(std::vector<std::wstring>& tabs, int& active_tab) const {
    std::wstring body;
    if (!ReadFileW(session_path, body)) return false;
    JsonGetStringArray(body, L"tabs", tabs);
    JsonGetInt(body, L"active_tab", active_tab);
    return true;
}

bool Config::SaveSession(const std::vector<std::wstring>& tabs, int active_tab) const {
    std::wostringstream o;
    o << L"{\n  \"active_tab\": " << active_tab << L",\n  \"tabs\": [";
    for (size_t k = 0; k < tabs.size(); ++k) {
        if (k) o << L", ";
        o << L"\"" << JsonEscape(tabs[k]) << L"\"";
    }
    o << L"]\n}\n";
    return WriteFileAtomic(session_path, o.str());
}

void Config::PushRecent(const std::wstring& path, size_t limit) {
    auto it = std::find(recents.begin(), recents.end(), path);
    if (it != recents.end()) recents.erase(it);
    recents.insert(recents.begin(), path);
    if (recents.size() > limit) recents.resize(limit);
}

bool Config::AddBookmark(const std::wstring& path) {
    if (std::find(bookmarks.begin(), bookmarks.end(), path) != bookmarks.end())
        return false;
    bookmarks.push_back(path);
    return true;
}

void Config::RemoveBookmark(const std::wstring& path) {
    auto it = std::find(bookmarks.begin(), bookmarks.end(), path);
    if (it != bookmarks.end()) bookmarks.erase(it);
}

} // namespace tnx
