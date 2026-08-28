/*
TGSpeechBox — SAPI settings dialog application.
Copyright 2025-2026 Tamas Geczy.
Licensed under the MIT License. See LICENSE for details.
*/

#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <string>
#include <unordered_set>
#include <vector>

#include "resource.h"

namespace {

// -----------------------------
// Small path helpers
// -----------------------------

bool file_exists(const std::wstring& path)
{
    DWORD attr = GetFileAttributesW(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES) && ((attr & FILE_ATTRIBUTE_DIRECTORY) == 0);
}

bool dir_exists(const std::wstring& path)
{
    DWORD attr = GetFileAttributesW(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES) && ((attr & FILE_ATTRIBUTE_DIRECTORY) != 0);
}

std::wstring join_path(const std::wstring& left, const std::wstring& right)
{
    if (left.empty()) return right;
    if (right.empty()) return left;
    std::wstring out = left;
    wchar_t last = out.back();
    if (last != L'\\' && last != L'/') out.push_back(L'\\');
    out += right;
    return out;
}

std::wstring strip_filename(const std::wstring& path)
{
    const size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return L".";
    return path.substr(0, pos);
}

std::wstring parent_dir(const std::wstring& path) { return strip_filename(path); }

std::wstring get_exe_dir()
{
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L".";
    return strip_filename(std::wstring(buf));
}

std::wstring detect_base_dir(const std::wstring& module_dir)
{
    const std::wstring lower = [&]() {
        std::wstring s = module_dir;
        std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)std::towlower(c); });
        return s;
    }();
    const std::wstring suffix_x86 = L"\\x86";
    const std::wstring suffix_x64 = L"\\x64";
    if (lower.size() >= suffix_x86.size() && lower.compare(lower.size() - suffix_x86.size(), suffix_x86.size(), suffix_x86) == 0)
        return parent_dir(module_dir);
    if (lower.size() >= suffix_x64.size() && lower.compare(lower.size() - suffix_x64.size(), suffix_x64.size(), suffix_x64) == 0)
        return parent_dir(module_dir);
    return module_dir;
}

// -----------------------------
// INI helpers
// -----------------------------

std::wstring trim_copy(const std::wstring& s)
{
    size_t start = 0;
    while (start < s.size() && std::iswspace(s[start])) ++start;
    size_t end = s.size();
    while (end > start && std::iswspace(s[end - 1])) --end;
    return s.substr(start, end - start);
}

std::wstring normalize_lang_tag(std::wstring tag)
{
    tag = trim_copy(tag);
    std::transform(tag.begin(), tag.end(), tag.begin(), [](wchar_t c) {
        if (c == L'_') return L'-';
        return (wchar_t)std::towlower(c);
    });
    return tag;
}

std::vector<std::wstring> split_list(const std::wstring& s)
{
    std::vector<std::wstring> out;
    std::wstring cur;
    cur.reserve(16);
    auto flush = [&]() {
        const std::wstring t = normalize_lang_tag(cur);
        if (!t.empty()) out.push_back(t);
        cur.clear();
    };
    for (wchar_t c : s) {
        if (c == L',' || c == L';' || c == L'\n' || c == L'\r' || c == L'\t')
            flush();
        else
            cur.push_back(c);
    }
    flush();
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::wstring join_list(const std::vector<std::wstring>& items)
{
    std::wstring out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) out += L",";
        out += items[i];
    }
    return out;
}

std::wstring get_user_settings_path()
{
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"settings.ini";
    std::wstring dir(buf);
    dir = join_path(dir, L"TGSpeechSapi");
    CreateDirectoryW(dir.c_str(), nullptr);
    return join_path(dir, L"settings.ini");
}

// -----------------------------
// Settings struct
// -----------------------------

struct Settings {
    bool logging_enabled = false;
    std::unordered_set<std::wstring> excluded;

    int sample_rate = 22050;
    int pauseMode = 1; // 0=off, 1=short, 2=long

    // Pitch mode: 0 = language default, 1-5 = specific mode.
    int pitchMode = 0;
    int pitchInflectionScale = 50; // 0-100, maps to 0.0-2.0

    // Voicing tone (0-100, -1 = default).
    int voiceTilt       = 50;
    int noiseGlottalMod = 0;
    int pitchSyncF1     = 50;
    int pitchSyncB1     = 50;
    int speedQuotient   = 50;
    int aspirationTilt  = 50;
    int cascadeBwScale  = 50;
    int voiceTremor     = 0;
    int headSize        = 50;
    int chorusDepth     = 0;
    int chorusDetune    = 33;

    // FrameEx (0-100).
    int frameExCreakiness  = 0;
    int frameExBreathiness = 0;
    int frameExJitter      = 0;
    int frameExShimmer     = 0;
    int frameExSharpness   = 50;

    // Rate controls.
    bool rateBoostEnabled = false;
    bool overrideSystemRate = false;
    int globalRate = 50; // 0-100 slider, maps to 0.3-4.0x
};

void write_ini_int(const std::wstring& path, const wchar_t* section, const wchar_t* key, int val)
{
    wchar_t buf[32];
    wsprintfW(buf, L"%d", val);
    WritePrivateProfileStringW(section, key, buf, path.c_str());
}

Settings load_settings(const std::wstring& ini_path)
{
    Settings s;

    s.logging_enabled = GetPrivateProfileIntW(L"General", L"logging", 0, ini_path.c_str()) != 0;

    wchar_t buf[8192] = {};
    GetPrivateProfileStringW(L"Languages", L"excluded", L"", buf, (DWORD)(sizeof(buf) / sizeof(buf[0])), ini_path.c_str());
    for (const auto& t : split_list(buf)) s.excluded.insert(t);
    s.excluded.erase(L"default");

    s.sample_rate = GetPrivateProfileIntW(L"Audio", L"sampleRate", 22050, ini_path.c_str());
    s.pauseMode = GetPrivateProfileIntW(L"Audio", L"pauseMode", 1, ini_path.c_str());

    // Pitch mode: read string, map to index.
    {
        wchar_t pmBuf[64] = {};
        GetPrivateProfileStringW(L"Audio", L"pitchMode", L"", pmBuf, 64, ini_path.c_str());
        std::wstring pm(pmBuf);
        if (pm == L"espeak_style")    s.pitchMode = 1;
        else if (pm == L"fujisaki_style") s.pitchMode = 2;
        else if (pm == L"impulse_style")  s.pitchMode = 3;
        else if (pm == L"klatt_style")    s.pitchMode = 4;
        else if (pm == L"legacy")         s.pitchMode = 5;
        else s.pitchMode = 0;
    }
    s.pitchInflectionScale = GetPrivateProfileIntW(L"Audio", L"pitchInflectionScale", 50, ini_path.c_str());

    auto rd = [&](const wchar_t* key, int def) {
        return GetPrivateProfileIntW(L"VoicingTone", key, def, ini_path.c_str());
    };
    s.voiceTilt       = rd(L"voiceTilt", 50);
    s.noiseGlottalMod = rd(L"noiseGlottalMod", 0);
    s.pitchSyncF1     = rd(L"pitchSyncF1", 50);
    s.pitchSyncB1     = rd(L"pitchSyncB1", 50);
    s.speedQuotient   = rd(L"speedQuotient", 50);
    s.aspirationTilt  = rd(L"aspirationTilt", 50);
    s.cascadeBwScale  = rd(L"cascadeBwScale", 50);
    s.voiceTremor     = rd(L"voiceTremor", 0);
    s.headSize        = rd(L"headSize", 50);
    s.chorusDepth     = rd(L"chorusDepth", 0);
    s.chorusDetune    = rd(L"chorusDetune", 33);

    s.frameExCreakiness  = rd(L"frameExCreakiness", 0);
    s.frameExBreathiness = rd(L"frameExBreathiness", 0);
    s.frameExJitter      = rd(L"frameExJitter", 0);
    s.frameExShimmer     = rd(L"frameExShimmer", 0);
    s.frameExSharpness   = rd(L"frameExSharpness", 50);

    s.rateBoostEnabled   = GetPrivateProfileIntW(L"Audio", L"rateBoost", 0, ini_path.c_str()) != 0;
    s.overrideSystemRate = GetPrivateProfileIntW(L"Audio", L"overrideRate", 0, ini_path.c_str()) != 0;
    s.globalRate         = GetPrivateProfileIntW(L"Audio", L"globalRate", 50, ini_path.c_str());

    return s;
}

bool save_settings(const std::wstring& ini_path, const Settings& s)
{
    const wchar_t* logging_val = s.logging_enabled ? L"1" : L"0";
    if (!WritePrivateProfileStringW(L"General", L"logging", logging_val, ini_path.c_str()))
        return false;

    std::vector<std::wstring> excluded_sorted;
    excluded_sorted.reserve(s.excluded.size());
    for (const auto& t : s.excluded) {
        if (t == L"default") continue;
        excluded_sorted.push_back(t);
    }
    std::sort(excluded_sorted.begin(), excluded_sorted.end());
    if (!WritePrivateProfileStringW(L"Languages", L"excluded", join_list(excluded_sorted).c_str(), ini_path.c_str()))
        return false;

    write_ini_int(ini_path, L"Audio", L"sampleRate", s.sample_rate);
    write_ini_int(ini_path, L"Audio", L"pauseMode", s.pauseMode);

    // Pitch mode: map index to string.
    {
        const wchar_t* pmStr = L"";
        switch (s.pitchMode) {
            case 1: pmStr = L"espeak_style"; break;
            case 2: pmStr = L"fujisaki_style"; break;
            case 3: pmStr = L"impulse_style"; break;
            case 4: pmStr = L"klatt_style"; break;
            case 5: pmStr = L"legacy"; break;
            default: pmStr = L""; break;
        }
        WritePrivateProfileStringW(L"Audio", L"pitchMode", pmStr, ini_path.c_str());
    }
    write_ini_int(ini_path, L"Audio", L"pitchInflectionScale", s.pitchInflectionScale);

    auto wr = [&](const wchar_t* key, int val) {
        write_ini_int(ini_path, L"VoicingTone", key, val);
    };
    wr(L"voiceTilt",       s.voiceTilt);
    wr(L"noiseGlottalMod", s.noiseGlottalMod);
    wr(L"pitchSyncF1",     s.pitchSyncF1);
    wr(L"pitchSyncB1",     s.pitchSyncB1);
    wr(L"speedQuotient",   s.speedQuotient);
    wr(L"aspirationTilt",  s.aspirationTilt);
    wr(L"cascadeBwScale",  s.cascadeBwScale);
    wr(L"voiceTremor",     s.voiceTremor);
    wr(L"headSize",        s.headSize);
    wr(L"chorusDepth",     s.chorusDepth);
    wr(L"chorusDetune",    s.chorusDetune);

    wr(L"frameExCreakiness",  s.frameExCreakiness);
    wr(L"frameExBreathiness", s.frameExBreathiness);
    wr(L"frameExJitter",      s.frameExJitter);
    wr(L"frameExShimmer",     s.frameExShimmer);
    wr(L"frameExSharpness",   s.frameExSharpness);

    WritePrivateProfileStringW(L"Audio", L"rateBoost",
        s.rateBoostEnabled ? L"1" : L"0", ini_path.c_str());
    WritePrivateProfileStringW(L"Audio", L"overrideRate",
        s.overrideSystemRate ? L"1" : L"0", ini_path.c_str());
    write_ini_int(ini_path, L"Audio", L"globalRate", s.globalRate);

    return true;
}

// -----------------------------
// Language list + display names
// -----------------------------

std::wstring to_windows_locale_name(const std::wstring& tag)
{
    std::wstring out;
    out.reserve(tag.size());
    bool after_dash = false;
    for (wchar_t c : tag) {
        if (c == L'_') c = L'-';
        if (c == L'-') { out.push_back(L'-'); after_dash = true; continue; }
        out.push_back(after_dash ? (wchar_t)std::towupper(c) : (wchar_t)std::towlower(c));
    }
    return out;
}

std::wstring get_language_display_name(const std::wstring& tag)
{
    const std::wstring locale = to_windows_locale_name(tag);
    wchar_t name[256] = {};
    int rc = GetLocaleInfoEx(locale.c_str(), LOCALE_SLOCALIZEDDISPLAYNAME, name, (int)(sizeof(name) / sizeof(name[0])));
    if (rc <= 0) return tag;
    std::wstring out(name);
    out += L" (";
    out += tag;
    out += L")";
    return out;
}

std::vector<std::wstring> list_installed_language_tags(const std::wstring& base_dir)
{
    const std::wstring lang_dir = join_path(join_path(base_dir, L"packs"), L"lang");
    std::vector<std::wstring> tags;
    if (!dir_exists(lang_dir)) return tags;
    const std::wstring pattern = join_path(lang_dir, L"*.yaml");
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return tags;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring fname(fd.cFileName);
        const size_t dot = fname.find_last_of(L'.');
        if (dot == std::wstring::npos) continue;
        fname = fname.substr(0, dot);
        fname = normalize_lang_tag(fname);
        if (fname != L"default") tags.push_back(fname);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(tags.begin(), tags.end());
    tags.erase(std::unique(tags.begin(), tags.end()), tags.end());
    if (tags.empty()) tags = {L"en-us", L"en", L"bg", L"de", L"fr-fr", L"es", L"it"};
    return tags;
}

// -----------------------------
// Dialog helpers
// -----------------------------

void init_slider(HWND hDlg, int id, int val, int minVal = 0, int maxVal = 100)
{
    HWND sl = GetDlgItem(hDlg, id);
    SendMessageW(sl, TBM_SETRANGE, TRUE, MAKELONG(minVal, maxVal));
    SendMessageW(sl, TBM_SETTICFREQ, 10, 0);
    SendMessageW(sl, TBM_SETPOS, TRUE, val);
}

int get_slider(HWND hDlg, int id)
{
    return (int)SendDlgItemMessageW(hDlg, id, TBM_GETPOS, 0, 0);
}

void set_slider(HWND hDlg, int id, int val)
{
    SendDlgItemMessageW(hDlg, id, TBM_SETPOS, TRUE, val);
}

// -----------------------------
// Dialog state
// -----------------------------

struct DialogState {
    std::wstring base_dir;
    std::wstring ini_path;
    std::vector<std::wstring> lang_tags;
    Settings settings;
};

void init_list_view(HWND hList)
{
    ListView_SetExtendedListViewStyleEx(
        hList,
        LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES,
        LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES);

    RECT rc = {};
    GetClientRect(hList, &rc);
    const int width = (rc.right > rc.left) ? (rc.right - rc.left - 4) : 200;

    LVCOLUMNW col = {};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = const_cast<wchar_t*>(L"Language");
    col.cx = width;
    ListView_InsertColumn(hList, 0, &col);
}

static const int kSampleRates[] = { 11025, 16000, 22050, 44100 };
static const wchar_t* kSampleRateLabels[] = { L"11025 Hz", L"16000 Hz", L"22050 Hz", L"44100 Hz" };

void populate_sliders_from_settings(HWND hDlg, const Settings& s)
{
    init_slider(hDlg, IDC_SL_VOICE_TILT,  s.voiceTilt);
    init_slider(hDlg, IDC_SL_NOISE_MOD,   s.noiseGlottalMod);
    init_slider(hDlg, IDC_SL_PITCH_F1,    s.pitchSyncF1);
    init_slider(hDlg, IDC_SL_PITCH_B1,    s.pitchSyncB1);
    init_slider(hDlg, IDC_SL_SPEED_QUOT,  s.speedQuotient);
    init_slider(hDlg, IDC_SL_ASP_TILT,    s.aspirationTilt);
    init_slider(hDlg, IDC_SL_CASCADE_BW,  s.cascadeBwScale);
    init_slider(hDlg, IDC_SL_TREMOR,      s.voiceTremor);
    init_slider(hDlg, IDC_SL_HEAD_SIZE,   s.headSize);
    init_slider(hDlg, IDC_SL_CHORUS_DEPTH,  s.chorusDepth);
    init_slider(hDlg, IDC_SL_CHORUS_DETUNE, s.chorusDetune);

    init_slider(hDlg, IDC_SL_CREAKINESS,  s.frameExCreakiness);
    init_slider(hDlg, IDC_SL_BREATHINESS, s.frameExBreathiness);
    init_slider(hDlg, IDC_SL_JITTER,      s.frameExJitter);
    init_slider(hDlg, IDC_SL_SHIMMER,     s.frameExShimmer);
    init_slider(hDlg, IDC_SL_SHARPNESS,   s.frameExSharpness);

    CheckDlgButton(hDlg, IDC_RATE_BOOST,    s.rateBoostEnabled ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hDlg, IDC_OVERRIDE_RATE,  s.overrideSystemRate ? BST_CHECKED : BST_UNCHECKED);
    init_slider(hDlg, IDC_SL_GLOBAL_RATE,   s.globalRate);
}

void read_sliders_to_settings(HWND hDlg, Settings& s)
{
    s.voiceTilt       = get_slider(hDlg, IDC_SL_VOICE_TILT);
    s.noiseGlottalMod = get_slider(hDlg, IDC_SL_NOISE_MOD);
    s.pitchSyncF1     = get_slider(hDlg, IDC_SL_PITCH_F1);
    s.pitchSyncB1     = get_slider(hDlg, IDC_SL_PITCH_B1);
    s.speedQuotient   = get_slider(hDlg, IDC_SL_SPEED_QUOT);
    s.aspirationTilt  = get_slider(hDlg, IDC_SL_ASP_TILT);
    s.cascadeBwScale  = get_slider(hDlg, IDC_SL_CASCADE_BW);
    s.voiceTremor     = get_slider(hDlg, IDC_SL_TREMOR);
    s.headSize        = get_slider(hDlg, IDC_SL_HEAD_SIZE);
    s.chorusDepth     = get_slider(hDlg, IDC_SL_CHORUS_DEPTH);
    s.chorusDetune    = get_slider(hDlg, IDC_SL_CHORUS_DETUNE);

    s.frameExCreakiness  = get_slider(hDlg, IDC_SL_CREAKINESS);
    s.frameExBreathiness = get_slider(hDlg, IDC_SL_BREATHINESS);
    s.frameExJitter      = get_slider(hDlg, IDC_SL_JITTER);
    s.frameExShimmer     = get_slider(hDlg, IDC_SL_SHIMMER);
    s.frameExSharpness   = get_slider(hDlg, IDC_SL_SHARPNESS);

    s.rateBoostEnabled   = IsDlgButtonChecked(hDlg, IDC_RATE_BOOST) == BST_CHECKED;
    s.overrideSystemRate = IsDlgButtonChecked(hDlg, IDC_OVERRIDE_RATE) == BST_CHECKED;
    s.globalRate         = get_slider(hDlg, IDC_SL_GLOBAL_RATE);
}

void reset_sliders_to_defaults(HWND hDlg)
{
    set_slider(hDlg, IDC_SL_VOICE_TILT,  50);
    set_slider(hDlg, IDC_SL_NOISE_MOD,   0);
    set_slider(hDlg, IDC_SL_PITCH_F1,    50);
    set_slider(hDlg, IDC_SL_PITCH_B1,    50);
    set_slider(hDlg, IDC_SL_SPEED_QUOT,  50);
    set_slider(hDlg, IDC_SL_ASP_TILT,    50);
    set_slider(hDlg, IDC_SL_CASCADE_BW,  50);
    set_slider(hDlg, IDC_SL_TREMOR,      0);
    set_slider(hDlg, IDC_SL_HEAD_SIZE,   50);
    set_slider(hDlg, IDC_SL_CHORUS_DEPTH,  0);
    set_slider(hDlg, IDC_SL_CHORUS_DETUNE, 33);

    set_slider(hDlg, IDC_SL_CREAKINESS,  0);
    set_slider(hDlg, IDC_SL_BREATHINESS, 0);
    set_slider(hDlg, IDC_SL_JITTER,      0);
    set_slider(hDlg, IDC_SL_SHIMMER,     0);
    set_slider(hDlg, IDC_SL_SHARPNESS,   50);

    // Reset sample rate to 22050 (#117: match every other platform's
    // default), pause mode to Short, pitch to language default.
    SendDlgItemMessageW(hDlg, IDC_SAMPLE_RATE, CB_SETCURSEL, 2, 0);
    SendDlgItemMessageW(hDlg, IDC_PAUSE_MODE, CB_SETCURSEL, 1, 0);
    SendDlgItemMessageW(hDlg, IDC_PITCH_MODE, CB_SETCURSEL, 0, 0);
    set_slider(hDlg, IDC_SL_INFLECTION, 50);

    CheckDlgButton(hDlg, IDC_RATE_BOOST,   BST_UNCHECKED);
    CheckDlgButton(hDlg, IDC_OVERRIDE_RATE, BST_UNCHECKED);
    set_slider(hDlg, IDC_SL_GLOBAL_RATE, 50);
}

// -----------------------------
// Dialog proc
// -----------------------------

INT_PTR CALLBACK MainDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_INITDIALOG: {
        auto* st = reinterpret_cast<DialogState*>(lParam);
        SetWindowLongPtrW(hDlg, DWLP_USER, reinterpret_cast<LONG_PTR>(st));

        // Logging checkbox.
        CheckDlgButton(hDlg, IDC_ENABLE_LOGGING, st->settings.logging_enabled ? BST_CHECKED : BST_UNCHECKED);

        // Language list.
        HWND hList = GetDlgItem(hDlg, IDC_LANG_LIST);
        init_list_view(hList);
        for (size_t i = 0; i < st->lang_tags.size(); ++i) {
            const std::wstring& tag = st->lang_tags[i];
            const std::wstring text = get_language_display_name(tag);
            LVITEMW item = {};
            item.mask = LVIF_TEXT | LVIF_PARAM;
            item.iItem = (int)i;
            item.pszText = const_cast<wchar_t*>(text.c_str());
            item.lParam = (LPARAM)i;
            ListView_InsertItem(hList, &item);
            const bool enabled = st->settings.excluded.find(normalize_lang_tag(tag)) == st->settings.excluded.end();
            ListView_SetCheckState(hList, (int)i, enabled ? TRUE : FALSE);
        }

        // Sample rate combo.
        HWND combo = GetDlgItem(hDlg, IDC_SAMPLE_RATE);
        int sel = 2; // default to 22050 (#117)
        for (int i = 0; i < 4; ++i) {
            SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)kSampleRateLabels[i]);
            if (kSampleRates[i] == st->settings.sample_rate) sel = i;
        }
        SendMessageW(combo, CB_SETCURSEL, sel, 0);

        // Pause mode combo.
        {
            HWND pm = GetDlgItem(hDlg, IDC_PAUSE_MODE);
            SendMessageW(pm, CB_ADDSTRING, 0, (LPARAM)L"Off");
            SendMessageW(pm, CB_ADDSTRING, 0, (LPARAM)L"Short");
            SendMessageW(pm, CB_ADDSTRING, 0, (LPARAM)L"Long");
            int pmSel = st->settings.pauseMode;
            if (pmSel < 0 || pmSel > 2) pmSel = 1;
            SendMessageW(pm, CB_SETCURSEL, pmSel, 0);
        }

        // Pitch mode combo.
        {
            HWND pm = GetDlgItem(hDlg, IDC_PITCH_MODE);
            SendMessageW(pm, CB_ADDSTRING, 0, (LPARAM)L"Use Language Default");
            SendMessageW(pm, CB_ADDSTRING, 0, (LPARAM)L"eSpeak Style");
            SendMessageW(pm, CB_ADDSTRING, 0, (LPARAM)L"Fujisaki");
            SendMessageW(pm, CB_ADDSTRING, 0, (LPARAM)L"Impulse");
            SendMessageW(pm, CB_ADDSTRING, 0, (LPARAM)L"Klatt");
            SendMessageW(pm, CB_ADDSTRING, 0, (LPARAM)L"Classic");
            int pmSel = st->settings.pitchMode;
            if (pmSel < 0 || pmSel > 5) pmSel = 0;
            SendMessageW(pm, CB_SETCURSEL, pmSel, 0);
        }

        // Inflection scale slider.
        init_slider(hDlg, IDC_SL_INFLECTION, st->settings.pitchInflectionScale);

        // Sliders.
        populate_sliders_from_settings(hDlg, st->settings);

        return TRUE;
    }

    case WM_COMMAND: {
        const WORD id = LOWORD(wParam);

        if (id == IDC_RESET_DEFAULTS) {
            reset_sliders_to_defaults(hDlg);
            return TRUE;
        }

        if (id == IDOK) {
            auto* st = reinterpret_cast<DialogState*>(GetWindowLongPtrW(hDlg, DWLP_USER));
            if (!st) { EndDialog(hDlg, IDCANCEL); return TRUE; }

            st->settings.logging_enabled = (IsDlgButtonChecked(hDlg, IDC_ENABLE_LOGGING) == BST_CHECKED);

            // Read language list.
            HWND hList = GetDlgItem(hDlg, IDC_LANG_LIST);
            const int count = ListView_GetItemCount(hList);
            std::unordered_set<std::wstring> excluded;
            for (int i = 0; i < count; ++i) {
                if (ListView_GetCheckState(hList, i)) continue;
                LVITEMW item = {};
                item.mask = LVIF_PARAM;
                item.iItem = i;
                if (ListView_GetItem(hList, &item)) {
                    const size_t idx = (size_t)item.lParam;
                    if (idx < st->lang_tags.size())
                        excluded.insert(normalize_lang_tag(st->lang_tags[idx]));
                }
            }
            st->settings.excluded = std::move(excluded);

            // Sample rate.
            int srSel = (int)SendDlgItemMessageW(hDlg, IDC_SAMPLE_RATE, CB_GETCURSEL, 0, 0);
            if (srSel >= 0 && srSel < 4)
                st->settings.sample_rate = kSampleRates[srSel];

            // Pause mode.
            int pmSel = (int)SendDlgItemMessageW(hDlg, IDC_PAUSE_MODE, CB_GETCURSEL, 0, 0);
            if (pmSel >= 0 && pmSel <= 2)
                st->settings.pauseMode = pmSel;

            // Pitch mode.
            int pitchSel = (int)SendDlgItemMessageW(hDlg, IDC_PITCH_MODE, CB_GETCURSEL, 0, 0);
            if (pitchSel >= 0 && pitchSel <= 5)
                st->settings.pitchMode = pitchSel;
            st->settings.pitchInflectionScale = get_slider(hDlg, IDC_SL_INFLECTION);

            // Sliders.
            read_sliders_to_settings(hDlg, st->settings);

            if (!save_settings(st->ini_path, st->settings)) {
                MessageBoxW(hDlg, L"Failed to write settings.ini. Try running this tool as administrator.",
                    L"TGSpeechBox SAPI Settings", MB_OK | MB_ICONERROR);
                return TRUE;
            }

            EndDialog(hDlg, IDOK);
            return TRUE;
        }

        if (id == IDCANCEL) {
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }

        break;
    }

    default:
        break;
    }

    return FALSE;
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    DialogState st;
    const std::wstring module_dir = get_exe_dir();
    st.base_dir = detect_base_dir(module_dir);
    st.ini_path = get_user_settings_path();

    st.lang_tags = list_installed_language_tags(st.base_dir);
    st.settings = load_settings(st.ini_path);

    (void)DialogBoxParamW(hInstance, MAKEINTRESOURCEW(IDD_MAIN_DIALOG), nullptr, MainDlgProc, reinterpret_cast<LPARAM>(&st));
    return 0;
}
