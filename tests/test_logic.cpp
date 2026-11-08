// TNxPilot logic test — runs on Linux with plain g++.
// Covers the platform-independent parts (util.h + config.cpp):
// formatting, fuzzy matching, batch-rename preview, JSON config/session.
// The Win32 parts (fileops.cpp, dialogs.cpp, main.cpp) can only be
// verified by the MSVC build on Windows — this test does NOT claim
// to cover them.
//
// Build:  g++ -std=c++17 -I../src test_logic.cpp ../src/config.cpp -o test_logic
// Run:    ./test_logic
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <iostream>

#include "../src/config.h"
#include "../src/util.h"

namespace fs = std::filesystem;
using namespace tnx;

static int passed = 0, failed = 0;
#define CHECK(name, cond) do { \
    if (cond) { ++passed; std::printf("  PASS  %s\n", name); } \
    else { ++failed; std::printf("  FAIL  %s\n", name); } } while (0)

int main() {
    std::printf("TNxPilot logic test (Linux, platform-independent parts)\n");

    CHECK("FormatSize B", FormatSize(512) == L"512 B");
    CHECK("FormatSize KB", FormatSize(1536) == L"1.50 KB");
    CHECK("FormatSize MB", FormatSize(2 * 1024 * 1024) == L"2.00 MB");
    CHECK("FormatDate epoch", FormatDate(0) == L"\u2014");
    CHECK("FormatDate 2026-01-01",
          FormatDate(1767225600).substr(0, 10) == L"2026-01-01");

    CHECK("fuzzy substring", FuzzyScore(L"rep", L"report.txt") > 0);
    CHECK("fuzzy subsequence", FuzzyScore(L"rpt", L"report.txt") > 0);
    CHECK("fuzzy no match", FuzzyScore(L"zzz", L"report.txt") == -1);
    CHECK("fuzzy empty query", FuzzyScore(L"", L"anything") == 0);

    std::vector<std::pair<std::wstring, bool>> files = {
        {L"report.txt", false}, {L"photo.jpg", false}, {L"docs", true}};
    auto p1 = RenamePreview(files, RenameRule{L"prefix", L"", L"", L"NEW_", 1, 3, L""});
    CHECK("rename prefix", p1[0].second == L"NEW_report.txt");
    CHECK("rename prefix keeps dir w/o ext", p1[2].second == L"NEW_docs");
    auto p2 = RenamePreview(files, RenameRule{L"numbering", L"", L"", L"", 5, 2, L""});
    CHECK("rename numbering", p2[0].second == L"report_05.txt");
    auto p3 = RenamePreview(files, RenameRule{L"find_replace", L"report", L"final", L"", 1, 3, L""});
    CHECK("rename find/replace", p3[0].second == L"final.txt");
    auto p4 = RenamePreview(files, RenameRule{L"date", L"", L"", L"", 1, 3, L"2026-11-08"});
    CHECK("rename date", p4[1].second == L"photo_2026-11-08.jpg");

    // Config round-trip in a temp dir.
    fs::path tmp = fs::temp_directory_path() / "tnxpilot_test_cfg";
    fs::create_directories(tmp);
    std::string dir = tmp.string();
    std::wstring wdir(dir.begin(), dir.end());
    {
        Config cfg(wdir);
        cfg.settings.theme = L"light";
        cfg.settings.font_size = 12;
        cfg.AddBookmark(L"C:\\Users\\Test\\Docs");
        cfg.PushRecent(L"C:\\Users\\Test\\Pics");
        CHECK("config save", cfg.Save());
        CHECK("session save", cfg.SaveSession({L"C:\\", L"D:\\"}, 1));
    }
    {
        Config cfg(wdir);
        CHECK("config load", cfg.Load());
        CHECK("theme round-trip", cfg.settings.theme == L"light");
        CHECK("font round-trip", cfg.settings.font_size == 12);
        CHECK("bookmark round-trip",
              !cfg.bookmarks.empty() && cfg.bookmarks[0] == L"C:\\Users\\Test\\Docs");
        CHECK("recent round-trip",
              !cfg.recents.empty() && cfg.recents[0] == L"C:\\Users\\Test\\Pics");
        std::vector<std::wstring> tabs;
        int active = 0;
        CHECK("session load", cfg.LoadSession(tabs, active));
        CHECK("session tabs", tabs.size() == 2 && tabs[1] == L"D:\\");
        CHECK("session active tab", active == 1);
    }
    fs::remove_all(tmp);

    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
