// TNxPilot — util.h
// Platform-independent helpers: size/date formatting, fuzzy matching,
// batch-rename rules. No Win32, no Qt — compiled and unit-tested on
// Linux too (see tests/test_logic.cpp). All strings are UTF-16 wstring,
// matching the Win32 wide API surface.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

namespace tnx {

// ------------------------------------------------------------------ sizes
inline std::wstring FormatSize(int64_t n) {
    if (n < 0) return L"\u2014";
    const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    double v = static_cast<double>(n);
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    wchar_t buf[64];
    if (u == 0) swprintf(buf, 64, L"%lld B", static_cast<long long>(n));
    else swprintf(buf, 64, L"%.2f %ls", v, units[u]);
    return buf;
}

inline std::wstring FormatDate(int64_t unix_seconds) {
    if (unix_seconds <= 0) return L"\u2014";
    // Kept simple + locale-independent: YYYY-MM-DD HH:MM (UTC).
    // The UI layer shows local time via FileTimeToSystemTime instead;
    // this is only used by the headless logic test.
    int64_t days = unix_seconds / 86400;
    int64_t secs = unix_seconds % 86400;
    int y = 1970, m = 1, d = 1;
    // civil-from-days (Howard Hinnant's algorithm)
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = static_cast<unsigned>(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<int>(yoe) + static_cast<int>(era) * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    y += (m <= 2);
    wchar_t buf[64];
    swprintf(buf, 64, L"%04d-%02d-%02d %02d:%02d", y, m, d,
             static_cast<int>(secs / 3600), static_cast<int>((secs % 3600) / 60));
    return buf;
}

// ------------------------------------------------------------------ strings
inline std::wstring ToLower(std::wstring_view s) {
    std::wstring out(s);
    for (auto& c : out) c = static_cast<wchar_t>(towlower(c));
    return out;
}

inline bool StartsWithI(std::wstring_view s, std::wstring_view prefix) {
    if (prefix.size() > s.size()) return false;
    return ToLower(s.substr(0, prefix.size())) == ToLower(prefix);
}

// ------------------------------------------------------------------ fuzzy
// Subsequence fuzzy score: higher = better, -1 = no match.
// Exact substring beats subsequence; tight matches score more.
inline int FuzzyScore(std::wstring_view query, std::wstring_view name) {
    std::wstring q = ToLower(query), n = ToLower(name);
    if (q.empty()) return 0;
    size_t pos = n.find(q);
    if (pos != std::wstring::npos) return 1000 - static_cast<int>(pos);
    int score = 0;
    size_t at = 0;
    for (wchar_t ch : q) {
        size_t idx = n.find(ch, at);
        if (idx == std::wstring::npos) return -1;
        score += std::max(0, 20 - static_cast<int>(idx - at));
        at = idx + 1;
    }
    return score;
}

// ------------------------------------------------------------------ rename
struct RenameRule {
    // find_replace | prefix | suffix | numbering | date | remove
    std::wstring mode = L"find_replace";
    std::wstring find;
    std::wstring replace;
    std::wstring text;      // prefix / suffix text
    int start = 1;
    int digits = 3;
    std::wstring date_text; // pre-formatted date string, e.g. L"2026-11-08"
};

// Split "name.ext" -> stem + ext (ext includes the dot, empty for dirs).
inline void SplitName(const std::wstring& name, std::wstring& stem,
                      std::wstring& ext) {
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0) { stem = name; ext.clear(); }
    else { stem = name.substr(0, dot); ext = name.substr(dot); }
}

inline std::wstring ApplyRenameRule(const std::wstring& name, bool is_dir,
                                    const RenameRule& rule, int index) {
    std::wstring stem, ext;
    SplitName(name, stem, ext);
    std::wstring out = stem;
    if (rule.mode == L"find_replace") {
        if (!rule.find.empty()) {
            size_t p = 0;
            while ((p = out.find(rule.find, p)) != std::wstring::npos) {
                out.replace(p, rule.find.size(), rule.replace);
                p += rule.replace.size();
            }
        }
    } else if (rule.mode == L"prefix") {
        out = rule.text + stem;
    } else if (rule.mode == L"suffix") {
        out = stem + rule.text;
    } else if (rule.mode == L"numbering") {
        wchar_t buf[32];
        swprintf(buf, 32, L"%0*d", rule.digits, rule.start + index);
        out = stem + L"_" + buf;
    } else if (rule.mode == L"date") {
        out = stem + L"_" + rule.date_text;
    } else if (rule.mode == L"remove") {
        if (!rule.find.empty()) {
            size_t p = 0;
            while ((p = out.find(rule.find, p)) != std::wstring::npos)
                out.erase(p, rule.find.size());
        }
    }
    return out + (is_dir ? L"" : ext);
}

// Preview without touching disk. Returns (old, new) pairs.
inline std::vector<std::pair<std::wstring, std::wstring>> RenamePreview(
    const std::vector<std::pair<std::wstring, bool>>& entries,
    const RenameRule& rule) {
    std::vector<std::pair<std::wstring, std::wstring>> out;
    out.reserve(entries.size());
    for (size_t i = 0; i < entries.size(); ++i)
        out.emplace_back(entries[i].first,
                         ApplyRenameRule(entries[i].first, entries[i].second,
                                         rule, static_cast<int>(i)));
    return out;
}

} // namespace tnx
