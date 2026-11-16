#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <deque>
#include <string>
#include <vector>

#include "goldsrc.hpp"
#include "hook_x86.hpp"
#include "config.hpp"
#include "database.hpp"
#include "gl_font.hpp"

namespace {

struct CaptionPage {
    std::wstring text;
    ULONGLONG start_ms = 0;
    ULONGLONG end_ms = 0;
};

struct ActiveCaption {
    std::wstring text;
    subs::Rgb color;
    subs::RoleCat cat = subs::RoleCat::NPC;
    ULONGLONG start_ms = 0;    // in g_gameMs units
    ULONGLONG expire_ms = 0;   // in g_gameMs units
    std::string key;           // normalized sound key
    int entnum = -1;           // emitting entity (-1 = unknown)
    int channel = -1;          // engine sound channel
    std::vector<CaptionPage> pages;
    int cachedWrapW = 0;
    int cachedMaxLines = -1;
};

// GoldSrc sound channels / flags
constexpr int kChanAuto   = 0;
constexpr int kChanStatic = 6;
constexpr int kSndChangeVol   = 0x40;
constexpr int kSndStop        = 0x20;
constexpr int kSndChangePitch = 0x80;

struct ActiveTitle {
    std::string key;
    std::wstring text;
    float x = -1.0f;
    float y = -1.0f;
    uint8_t r = 220, g = 220, b = 220;
    ULONGLONG start_ms = 0;
    ULONGLONG fadein_ms = 200;
    ULONGLONG hold_ms = 3000;
    ULONGLONG fadeout_ms = 1000;
};

SRWLOCK g_lock = SRWLOCK_INIT;
subs::Config g_cfg;
subs::Database g_db;

std::string g_exeDir;
std::string g_activeGameDir = "valve";
std::string g_iniPath;
FILETIME g_iniMtime{};
DWORD g_lastIniCheckTick = 0;
bool g_logOpened = false;

std::deque<ActiveCaption> g_captions;
std::vector<ActiveTitle> g_activeTitles;
std::string g_lastCapKey;
ULONGLONG g_lastCapMs = 0;
float g_lastClientTime = 0.0f;
bool g_resetRequested = false;
// Game-time clock (ms): advances only while client time advances,
// so captions freeze while the game is paused / in the menu.
ULONGLONG g_gameMs = 1;
float g_clockPrevTime = -1.0f;
bool g_isMModGame = false;

void ClearCaptionBuffersLocked() {
    g_captions.clear();
    g_activeTitles.clear();
    g_lastCapKey.clear();
    g_lastCapMs = 0;
}

// Removes captions emitted by the same entity on the same channel
// (the engine replaces/stops that sound, so its caption must go too).
// Never drop HEV suit captions on channel replacement unless keyFilter matches explicitly.
size_t DropCaptionsForChannelLocked(int entnum, int channel, const std::string* keyFilter) {
    size_t before = g_captions.size();
    g_captions.erase(
        std::remove_if(g_captions.begin(), g_captions.end(),
                       [&](const ActiveCaption& c) {
                           if (c.entnum != entnum || c.channel != channel) return false;
                           if (!keyFilter && c.cat == subs::RoleCat::HEV) return false;
                           return !keyFilter || c.key == *keyFilter;
                       }),
        g_captions.end());
    if (keyFilter ? (g_lastCapKey == *keyFilter) : true) {
        g_lastCapKey.clear();
    }
    return before - g_captions.size();
}

cl_enginefunc_t* g_engfuncs = nullptr;
glfont::GlApi g_gl;
glfont::FontAtlas g_atlas;
glfont::FontAtlas g_titleAtlas;
int g_screenW = 0;
int g_screenH = 0;
float g_viewOrg[3] = {0, 0, 0};
bool g_hasViewOrg = false;
ULONGLONG g_lastHudRedrawRealMs = 0;
ULONGLONG g_lastTimeAdvanceRealMs = 0;

bool g_hwHooked = false;
bool g_clientHooked = false;
bool g_glHooked = false;

using FnLoadLibraryA   = HMODULE(WINAPI*)(LPCSTR);
using FnStartSound     = void(__cdecl*)(int entnum, int entchannel, sfx_t* sfx, float* origin, float fvol, float attenuation, int flags, int pitch);
using FnInitialize     = int(__cdecl*)(cl_enginefunc_t* pEnginefuncs, int iVersion);
using FnHudRedraw      = int(__cdecl*)(float time, int intermission);
using FnCalcRefdef     = void(__cdecl*)(ref_params_t* pparams);
using FnHudReset       = void(__cdecl*)(void);
using FnTextMsgGet     = client_textmessage_t*(__cdecl*)(const char* pName);
using FnWglSwapBuffers = BOOL(WINAPI*)(HDC);
using FnLoadgame       = void(__cdecl*)(void);
using FnHookUserMsg    = int(__cdecl*)(const char* szMsgName, void* pfn);
using FnUserMsg        = int(__cdecl*)(const char* pszName, int iSize, void* pbuf);

FnLoadLibraryA   g_origLoadLibraryA = nullptr;
FnStartSound     g_origDynSound = nullptr;
FnStartSound     g_origStatSound = nullptr;
FnInitialize     g_origInitialize = nullptr;
FnHudRedraw      g_origHudRedraw = nullptr;
FnCalcRefdef     g_origCalcRefdef = nullptr;
FnHudReset       g_origHudReset = nullptr;
FnTextMsgGet     g_origTextMsgGet = nullptr;
FnWglSwapBuffers g_origWglSwapBuffers = nullptr;
FnLoadgame       g_origLoadgame = nullptr;
FnHookUserMsg    g_origHookUserMsg = nullptr;
FnUserMsg        g_origMsgBattery = nullptr;
FnUserMsg        g_origMsgHealth = nullptr;
FnUserMsg        g_origMsgDamage = nullptr;
FnUserMsg        g_origMsgWeapPickup = nullptr;

enum HevCue {
    kHevArmorGone = 0,
    kHevPowerRestored,
    kHevSuitDmg,
    kHevSuitCrit,
    kHevFire,
    kHevShock,
    kHevFracture,
    kHevLaceration,
    kHevBloodLoss,
    kHevSeekMedic,
    kHevCueCount
};

bool g_hevVoiceEnabled = true;
int g_prevBattery = -1;
int g_prevHealth = -1;
ULONGLONG g_lastNativeHevMs = 0;
ULONGLONG g_hevCueCooldown[kHevCueCount] = {};
uint32_t g_announcedWeaponsMask = 0;

void ResetHevStateLocked() {
    g_prevBattery = -1;
    g_prevHealth = -1;
    g_lastNativeHevMs = 0;
    g_announcedWeaponsMask = 0;
    for (auto& t : g_hevCueCooldown) t = 0;
}

void LogMsg(const char* fmt, ...) {
    if (!g_cfg.debug_log || g_exeDir.empty()) return;
    std::string logPath = g_exeDir + "\\subtitles_debug.log";
    FILE* f = std::fopen(logPath.c_str(), g_logOpened ? "a" : "w");
    if (!f) return;
    g_logOpened = true;
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(f, fmt, ap);
    va_end(ap);
    std::fputc('\n', f);
    std::fclose(f);
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string ParseCmdlineGameDir() {
    const char* cmd = GetCommandLineA();
    if (!cmd) return "valve";
    const char* p = cmd;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (_strnicmp(p, "-game", 5) == 0 && (p[5] == ' ' || p[5] == '\t')) {
            p += 5;
            while (*p == ' ' || *p == '\t') p++;
            bool q = (*p == '"');
            if (q) p++;
            std::string val;
            while (*p && (q ? (*p != '"') : (*p != ' ' && *p != '\t'))) {
                val.push_back(*p++);
            }
            if (!val.empty()) return val;
        }
        if (*p == '"') {
            p++;
            while (*p && *p != '"') p++;
            if (*p == '"') p++;
        } else {
            while (*p && *p != ' ' && *p != '\t') p++;
        }
    }
    return "valve";
}

void ReloadAllFromDisk(bool reloadDb) {
    AcquireSRWLockExclusive(&g_lock);
    g_cfg.LoadIni(g_iniPath);
    if (reloadDb) {
        g_db.ReloadAll(g_exeDir, g_activeGameDir, g_cfg);
        std::string gdLower = subs::Config::Lower(g_activeGameDir);
        std::string fbLower = subs::Config::Lower(subs::Database::ReadLiblistFallbackDir(g_exeDir + "\\" + g_activeGameDir));
        g_isMModGame = (gdLower == "hl1mmod" || fbLower == "hl1mmod");
        LogMsg("[reload] gameDir=%s mmod=%d subs=%zu titles=%zu roles=%zu",
               g_activeGameDir.c_str(), (int)g_isMModGame, g_db.subtitles.size(), g_db.titles.size(), g_cfg.roles.size());
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void __cdecl Cmd_SubtitlesReload() {
    if (g_engfuncs && g_engfuncs->pfnGetGameDirectory) {
        const char* gd = g_engfuncs->pfnGetGameDirectory();
        if (gd && *gd) g_activeGameDir = gd;
    }
    ReloadAllFromDisk(true);
    if (g_engfuncs && g_engfuncs->Con_Printf) {
        g_engfuncs->Con_Printf("[Subtitles] Reloaded (%s): %u captions, %u titles.\n",
                               g_activeGameDir.c_str(),
                               (unsigned)g_db.subtitles.size(),
                               (unsigned)g_db.titles.size());
    }
}

void CheckIniHotReload() {
    DWORD now = GetTickCount();
    if (now - g_lastIniCheckTick < 1000) return;
    g_lastIniCheckTick = now;

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExA(g_iniPath.c_str(), GetFileExInfoStandard, &fad)) {
        if (CompareFileTime(&fad.ftLastWriteTime, &g_iniMtime) != 0) {
            g_iniMtime = fad.ftLastWriteTime;
            ReloadAllFromDisk(false);
            LogMsg("[cfg] Hot-reloaded subtitles.ini");
        }
    }
}

uint32_t ComputeCaptionDurationMs(uint32_t rawMs, const std::string& utf8Text) {
    if (rawMs > 0) {
        return std::clamp<uint32_t>(rawMs, 1400, 90000);
    }
    uint32_t chars = 0;
    for (size_t i = 0; i < utf8Text.size(); ) {
        unsigned char c = (unsigned char)utf8Text[i];
        if ((c & 0x80) == 0) i += 1;
        else if ((c & 0xE0) == 0xC0) i += 2;
        else if ((c & 0xF0) == 0xE0) i += 3;
        else i += 4;
        chars++;
    }
    return std::clamp<uint32_t>(chars * 50, 300, 4800) + 1000;
}

bool TraceHeightClear(event_api_t* ev, const float* origin, float zOff, int flags, float& outBestFrac) {
    float start[3] = {g_viewOrg[0], g_viewOrg[1], g_viewOrg[2]};
    float end[3]   = {origin[0], origin[1], origin[2] + zOff};
    float dx = end[0] - start[0];
    float dy = end[1] - start[1];
    float dz = end[2] - start[2];
    float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len <= 24.0f) {
        outBestFrac = 1.0f;
        return true;
    }

    pmtrace_t tr{};
    ev->EV_PlayerTrace(start, end, flags, -1, &tr);
    if (tr.startsolid && !tr.allsolid) {
        float scale = 22.0f / len;
        float nudged[3] = {start[0] + dx * scale, start[1] + dy * scale, start[2] + dz * scale};
        tr = pmtrace_t{};
        ev->EV_PlayerTrace(nudged, end, flags, -1, &tr);
    }
    if (tr.fraction > outBestFrac) outBestFrac = tr.fraction;
    return tr.fraction >= 0.92f;
}

bool IsSoundOccluded(const float* origin, float dist) {
    if (!g_engfuncs || !g_engfuncs->pEventAPI || !g_engfuncs->pEventAPI->EV_PlayerTrace || !g_hasViewOrg) {
        return false;
    }
    event_api_t* ev = g_engfuncs->pEventAPI;
    bool hasPM = (ev->EV_PushPMStates && ev->EV_PopPMStates && ev->EV_SetTraceHull);
    if (hasPM) {
        ev->EV_PushPMStates();
        ev->EV_SetTraceHull(2);
    }

    auto probePass = [&](int flags, float& bestFrac) -> bool {
        if (TraceHeightClear(ev, origin, 48.0f, flags, bestFrac)) return true;
        if (TraceHeightClear(ev, origin, 16.0f, flags, bestFrac)) return true;
        return false;
    };

    float bestFrac = 0.0f;
    bool clear = probePass(0x00000001 /* PM_STUDIO_IGNORE */, bestFrac);
    if (!clear && dist <= g_cfg.glass_max_distance) {
        clear = probePass(0x00000005 /* PM_STUDIO_IGNORE | PM_GLASS_IGNORE */, bestFrac);
    }

    if (hasPM) ev->EV_PopPMStates();
    return !clear;
}

void TryHookClient(HMODULE cl);

void HandleSound(int entnum, int entchannel, bool isStatic, sfx_t* sfx, float* origin, float fvol, float attenuation, int flags) {
    if (!g_clientHooked) {
        if (HMODULE cl = GetModuleHandleA("client.dll")) TryHookClient(cl);
    }
    if (!sfx || reinterpret_cast<uintptr_t>(sfx) < 0x10000) return;

    // A dynamic sound on a real entity + exclusive channel replaces whatever
    // that entity was playing on that channel (engine behaviour).
    const bool exclusiveChan = !isStatic && entnum > 0 &&
                               entchannel != kChanAuto && entchannel != kChanStatic;

    if (flags & kSndStop) {
        std::string stopNorm = sfx->name[0] ? subs::Database::NormalizeKey(sfx->name) : std::string();
        size_t dropped = 0;
        AcquireSRWLockExclusive(&g_lock);
        if (exclusiveChan) {
            dropped = DropCaptionsForChannelLocked(entnum, entchannel, nullptr);
        } else if (!stopNorm.empty()) {
            dropped = DropCaptionsForChannelLocked(entnum, entchannel, &stopNorm);
        }
        ReleaseSRWLockExclusive(&g_lock);
        if (dropped > 0) {
            LogMsg("[sfx-stop] ent=%d ch=%d raw='%s' dropped=%zu", entnum, entchannel, sfx->name, dropped);
        }
        return;
    }
    if (flags & (kSndChangeVol | kSndChangePitch)) return;
    if (!sfx->name[0]) return;

    AcquireSRWLockShared(&g_lock);
    std::string norm;
    const subs::SubtitleEntry* entry = g_db.FindSubtitle(sfx->name, norm);
    if (!entry) {
        ReleaseSRWLockShared(&g_lock);
        return;
    }
    auto [color, cat] = g_cfg.Classify(norm, entry->text);
    bool catEnabled = g_cfg.IsCategoryEnabled(cat);
    float minVol = g_cfg.min_volume;
    float maxDist = g_cfg.max_distance;
    bool checkOcc = g_cfg.check_occlusion;
    uint32_t durMs = ComputeCaptionDurationMs(entry->duration_ms, entry->text);
    std::wstring wtext = Utf8ToWide(entry->text);
    ReleaseSRWLockShared(&g_lock);

    if (!catEnabled || fvol < minVol || wtext.empty()) {
        LogMsg("[sfx-skip] filtered raw='%s' norm='%s' cat=%d vol=%.2f", sfx->name, norm.c_str(), (int)cat, fvol);
        return;
    }

    bool originZero = (!origin || (origin[0] == 0.0f && origin[1] == 0.0f && origin[2] == 0.0f));
    bool ambientSpecial = (norm.rfind("tride/", 0) == 0 ||
                           norm.rfind("nihilanth/", 0) == 0 ||
                           norm.rfind("sc_dis", 0) == 0 ||
                           norm.rfind("scientist/c1a0_sci_dis", 0) == 0);
    bool isGlobal = (cat == subs::RoleCat::HEV) ||
                    (cat == subs::RoleCat::VOX && (fvol >= 0.8f || attenuation <= 0.8f)) ||
                    (attenuation <= 0.01f) ||
                    originZero || !g_hasViewOrg || ambientSpecial;

    bool isHgrunt = (norm.rfind("hg_", 0) == 0 || norm.rfind("hgrunt/", 0) == 0);
    bool isSiloSci = (norm.rfind("sc_silo", 0) == 0 || norm.rfind("scientist/c1a3_sci_silo", 0) == 0);
    float effMaxDist = isHgrunt ? std::max(maxDist, 1100.0f) : (isSiloSci ? std::max(maxDist, 1500.0f) : maxDist);

    float dist = 0.0f;
    if (!isGlobal && origin && g_hasViewOrg) {
        float dx = origin[0] - g_viewOrg[0];
        float dy = origin[1] - g_viewOrg[1];
        float dz = origin[2] - g_viewOrg[2];
        dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist > effMaxDist) {
            LogMsg("[sfx-skip] too_far raw='%s' norm='%s' dist=%.0f max=%.0f atten=%.2f", sfx->name, norm.c_str(), dist, effMaxDist, attenuation);
            return;
        }
        bool shouldCheckOcc = checkOcc && !isSiloSci && !(isHgrunt && dist <= 750.0f);
        if (shouldCheckOcc && IsSoundOccluded(origin, dist)) {
            LogMsg("[sfx-skip] occluded raw='%s' norm='%s' dist=%.0f atten=%.2f", sfx->name, norm.c_str(), dist, attenuation);
            return;
        }
    }

    AcquireSRWLockExclusive(&g_lock);
    const ULONGLONG nowMs = g_gameMs;
    if (g_lastCapKey == norm && nowMs >= g_lastCapMs && (nowMs - g_lastCapMs) < 1500) {
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    if (exclusiveChan && cat != subs::RoleCat::HEV) {
        DropCaptionsForChannelLocked(entnum, entchannel, nullptr);
    }
    // The same line re-triggered elsewhere: refresh instead of stacking a duplicate.
    g_captions.erase(
        std::remove_if(g_captions.begin(), g_captions.end(),
                       [&](const ActiveCaption& c) { return c.key == norm; }),
        g_captions.end());
    g_lastCapKey = norm;
    g_lastCapMs = nowMs;
    if (cat == subs::RoleCat::HEV) {
        g_lastNativeHevMs = nowMs;
    }
    ActiveCaption cap;
    cap.text = std::move(wtext);
    cap.color = color;
    cap.cat = cat;
    cap.start_ms = nowMs;
    cap.expire_ms = nowMs + durMs;
    cap.key = norm;
    cap.entnum = entnum;
    cap.channel = entchannel;
    g_captions.push_back(std::move(cap));
    while (g_captions.size() > 16) g_captions.pop_front();
    ReleaseSRWLockExclusive(&g_lock);
    LogMsg("[sfx-ok] raw='%s' norm='%s' ent=%d ch=%d global=%d dist=%.0f vol=%.2f atten=%.2f dur=%ums",
           sfx->name, norm.c_str(), entnum, entchannel, (int)isGlobal, dist, fvol, attenuation, durMs);
}

void __cdecl Hook_S_StartDynamicSound(int entnum, int entchannel, sfx_t* sfx, float* origin, float fvol, float attenuation, int flags, int pitch) {
    HandleSound(entnum, entchannel, false, sfx, origin, fvol, attenuation, flags);
    if (g_origDynSound) g_origDynSound(entnum, entchannel, sfx, origin, fvol, attenuation, flags, pitch);
}

void __cdecl Hook_S_StartStaticSound(int entnum, int entchannel, sfx_t* sfx, float* origin, float fvol, float attenuation, int flags, int pitch) {
    HandleSound(entnum, entchannel, true, sfx, origin, fvol, attenuation, flags);
    if (g_origStatSound) g_origStatSound(entnum, entchannel, sfx, origin, fvol, attenuation, flags, pitch);
}

client_textmessage_t* __cdecl Hook_TextMessageGet(const char* pName) {
    client_textmessage_t* orig = g_origTextMsgGet ? g_origTextMsgGet(pName) : nullptr;
    if (!orig || !pName || !pName[0]) return orig;

    std::string key = subs::Database::UpperTrimTitleKey(pName);

    AcquireSRWLockExclusive(&g_lock);
    auto it = g_db.titles.find(key);
    if (it == g_db.titles.end()) {
        ReleaseSRWLockExclusive(&g_lock);
        return orig;
    }

    std::wstring wtxt = Utf8ToWide(it->second);
    if (wtxt.empty()) {
        ReleaseSRWLockExclusive(&g_lock);
        return orig;
    }

    ActiveTitle at;
    at.key = key;
    at.text = std::move(wtxt);
    at.x = orig->x;
    at.y = orig->y;
    at.r = orig->r1;
    at.g = orig->g1;
    at.b = orig->b1;
    at.start_ms = GetTickCount64();

    double fin = std::max(0.2, (double)orig->fadein) * 1000.0;
    double fout = std::max(0.2, (double)orig->fadeout) * 1000.0;
    double hold = std::max(1.5, (double)orig->holdtime) * 1000.0;
    if (orig->effect == 2) {
        double typeMs = (double)at.text.size() * std::max(0.005, (double)orig->fadein) * 1000.0;
        fin = std::clamp(typeMs, 250.0, 2500.0);
    }
    at.fadein_ms = (ULONGLONG)fin;
    at.hold_ms = (ULONGLONG)hold;
    at.fadeout_ms = (ULONGLONG)fout;

    // Remove any existing title with the same key OR at the same (x, y) screen slot
    // (matches CHudMessage::MessageAdd and prevents overlapping titles in Uplink / c0a0).
    g_activeTitles.erase(
        std::remove_if(g_activeTitles.begin(), g_activeTitles.end(),
                       [&](const ActiveTitle& existing) {
                           if (existing.key == key) return true;
                           return std::fabs(existing.x - at.x) < 0.01f &&
                                  std::fabs(existing.y - at.y) < 0.01f;
                       }),
        g_activeTitles.end());
    g_activeTitles.push_back(std::move(at));
    if (g_activeTitles.size() > 8) g_activeTitles.erase(g_activeTitles.begin());

    // Return a silent copy so the vanilla engine doesn't draw garbled ASCII over our VGUI title
    static client_textmessage_t s_suppressed[16];
    static unsigned s_suppressedIdx = 0;
    client_textmessage_t& sup = s_suppressed[s_suppressedIdx++ & 15];
    sup = *orig;
    sup.effect = 0;
    sup.r1 = sup.g1 = sup.b1 = sup.a1 = 0;
    sup.r2 = sup.g2 = sup.b2 = sup.a2 = 0;
    sup.fadein = 0.0f;
    sup.fadeout = 0.0f;
    sup.holdtime = 0.0f;
    sup.fxtime = 0.0f;
    sup.pName = "";
    sup.pMessage = "";

    ReleaseSRWLockExclusive(&g_lock);
    return &sup;
}

template <typename MeasureFn>
std::vector<std::wstring> WrapTextWith(const std::wstring& text, int maxW, const MeasureFn& measureWidth) {
    std::vector<std::wstring> lines;
    std::wstring cur;
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == L'\n') {
            lines.push_back(cur);
            cur.clear();
            i++;
            continue;
        }
        size_t next = text.find_first_of(L" \n", i);
        if (next == std::wstring::npos) next = text.size();
        std::wstring word = text.substr(i, next - i);
        std::wstring cand = cur.empty() ? word : (cur + L" " + word);
        int w = measureWidth(cand);
        if (!cur.empty() && w > maxW) {
            lines.push_back(cur);
            cur = word;
        } else {
            cur = cand;
        }
        i = (next < text.size() && text[next] == L' ') ? (next + 1) : next;
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

std::vector<std::wstring> WrapText(const std::wstring& text, int maxW) {
    return WrapTextWith(text, maxW, [](const std::wstring& s) { return g_atlas.MeasureWidth(g_gl, s); });
}

std::wstring TrimWs(const std::wstring& s) {
    size_t a = 0;
    while (a < s.size() && (s[a] == L' ' || s[a] == L'\t' || s[a] == L'\r' || s[a] == L'\n')) ++a;
    size_t b = s.size();
    while (b > a && (s[b - 1] == L' ' || s[b - 1] == L'\t' || s[b - 1] == L'\r' || s[b - 1] == L'\n')) --b;
    return s.substr(a, b - a);
}

// Split a body segment on sentence boundaries (. ! ? …) into trimmed clauses.
std::vector<std::wstring> SplitSentenceClauses(const std::wstring& s) {
    std::vector<std::wstring> out;
    size_t start = 0;
    for (size_t i = 0; i + 1 < s.size(); ++i) {
        wchar_t c = s[i];
        bool endMark = (c == L'!' || c == L'?' || c == L'\u2026');
        bool dotBreak = (c == L'.' && s[i + 1] == L' ' && i >= 2 && s[i - 1] != L'.');
        if ((endMark && s[i + 1] == L' ') || dotBreak) {
            // Avoid splitting abbreviations like "д.ф.н." where next char is lowercase.
            if (dotBreak && i + 2 < s.size() && iswlower(s[i + 2])) continue;
            std::wstring part = TrimWs(s.substr(start, i - start + 1));
            if (!part.empty()) out.push_back(std::move(part));
            start = i + 2;
        }
    }
    std::wstring tail = TrimWs(s.substr(start));
    if (!tail.empty()) out.push_back(std::move(tail));
    return out;
}

template <typename MeasureFn>
void AppendSubClausesFitting(const std::wstring& prefix, const std::wstring& clause,
                             int maxLines, int maxW, const MeasureFn& measureWidth,
                             std::vector<std::wstring>& out) {
    if (WrapTextWith(prefix + clause, maxW, measureWidth).size() <= (size_t)maxLines) {
        out.push_back(clause);
        return;
    }
    // Try splitting on comma / semicolon / em-dash first.
    std::vector<std::wstring> pieces;
    size_t start = 0;
    for (size_t i = 0; i + 1 < clause.size(); ++i) {
        if ((clause[i] == L',' || clause[i] == L';') && clause[i + 1] == L' ') {
            std::wstring p = TrimWs(clause.substr(start, i - start + 1));
            if (!p.empty()) pieces.push_back(std::move(p));
            start = i + 2;
        } else if (clause[i] == L'\u2014' && i > 0 && clause[i - 1] == L' ' && clause[i + 1] == L' ') {
            std::wstring p = TrimWs(clause.substr(start, i - start + 1));
            if (!p.empty()) pieces.push_back(std::move(p));
            start = i + 2;
        }
    }
    std::wstring tail = TrimWs(clause.substr(start));
    if (!tail.empty()) pieces.push_back(std::move(tail));

    if (pieces.size() > 1) {
        for (const auto& p : pieces) {
            if (WrapTextWith(prefix + p, maxW, measureWidth).size() <= (size_t)maxLines) {
                out.push_back(p);
            } else {
                // Fallback: split by words into maxLines chunks.
                std::vector<std::wstring> lines = WrapTextWith(prefix + p, maxW, measureWidth);
                if (!prefix.empty() && !lines.empty() && lines[0].rfind(prefix, 0) == 0) {
                    lines[0] = lines[0].substr(prefix.size());
                }
                for (size_t i = 0; i < lines.size(); i += (size_t)maxLines) {
                    std::wstring chunk;
                    for (size_t j = i; j < std::min(lines.size(), i + (size_t)maxLines); ++j) {
                        if (!chunk.empty()) chunk += L' ';
                        chunk += TrimWs(lines[j]);
                    }
                    if (!chunk.empty()) out.push_back(std::move(chunk));
                }
            }
        }
        return;
    }

    // Fallback: word-level chunking.
    std::vector<std::wstring> lines = WrapTextWith(prefix + clause, maxW, measureWidth);
    if (!prefix.empty() && !lines.empty() && lines[0].rfind(prefix, 0) == 0) {
        lines[0] = lines[0].substr(prefix.size());
    }
    for (size_t i = 0; i < lines.size(); i += (size_t)maxLines) {
        std::wstring chunk;
        for (size_t j = i; j < std::min(lines.size(), i + (size_t)maxLines); ++j) {
            if (!chunk.empty()) chunk += L' ';
            chunk += TrimWs(lines[j]);
        }
        if (!chunk.empty()) out.push_back(std::move(chunk));
    }
}

template <typename MeasureFn>
std::vector<CaptionPage> BuildCaptionPages(const std::wstring& fullText, ULONGLONG startMs, ULONGLONG expireMs,
                                           int maxLinesPerCap, int maxW, const MeasureFn& measureWidth) {
    bool hasManualSplit = (fullText.find(L"||") != std::wstring::npos);
    if (!hasManualSplit && (maxLinesPerCap <= 0 ||
        WrapTextWith(fullText, maxW, measureWidth).size() <= (size_t)maxLinesPerCap)) {
        return {{fullText, startMs, expireMs}};
    }

    std::wstring prefix;
    std::wstring body = fullText;
    if (!fullText.empty() && fullText[0] == L'[') {
        size_t close = fullText.find(L']');
        if (close != std::wstring::npos && close + 1 < fullText.size() && fullText[close + 1] == L' ') {
            prefix = fullText.substr(0, close + 2);
            body = fullText.substr(close + 2);
        }
    }

    // Split on manual "||" first, then split oversized groups into sentence/clause pages.
    std::vector<std::wstring> manualGroups;
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t sep = body.find(L"||", pos);
        if (sep == std::wstring::npos) {
            std::wstring g = TrimWs(body.substr(pos));
            if (!g.empty()) manualGroups.push_back(std::move(g));
            break;
        }
        std::wstring g = TrimWs(body.substr(pos, sep - pos));
        if (!g.empty()) manualGroups.push_back(std::move(g));
        pos = sep + 2;
    }

    std::vector<std::wstring> pageBodies;
    int limit = std::max(1, maxLinesPerCap);
    for (const auto& grp : manualGroups) {
        if (maxLinesPerCap <= 0 || WrapTextWith(prefix + grp, maxW, measureWidth).size() <= (size_t)limit) {
            pageBodies.push_back(grp);
            continue;
        }
        std::vector<std::wstring> clauses;
        for (const auto& sc : SplitSentenceClauses(grp)) {
            AppendSubClausesFitting(prefix, sc, limit, maxW, measureWidth, clauses);
        }
        std::wstring cur;
        for (const auto& cl : clauses) {
            std::wstring cand = cur.empty() ? cl : (cur + L" " + cl);
            if (!cur.empty() && WrapTextWith(prefix + cand, maxW, measureWidth).size() > (size_t)limit) {
                pageBodies.push_back(cur);
                cur = cl;
            } else {
                cur = std::move(cand);
            }
        }
        if (!cur.empty()) pageBodies.push_back(std::move(cur));
    }

    if (pageBodies.empty()) return {{fullText, startMs, expireMs}};

    ULONGLONG totalDur = (expireMs > startMs) ? (expireMs - startMs) : 1200ULL;
    size_t totalWeight = 0;
    for (const auto& pb : pageBodies) totalWeight += std::max<size_t>(1, pb.size());

    std::vector<CaptionPage> pages;
    pages.reserve(pageBodies.size());
    ULONGLONG cursor = startMs;
    for (size_t i = 0; i < pageBodies.size(); ++i) {
        ULONGLONG endMs = expireMs;
        if (i + 1 < pageBodies.size()) {
            size_t w = std::max<size_t>(1, pageBodies[i].size());
            ULONGLONG dur = (totalDur * w) / totalWeight;
            if (totalDur >= (ULONGLONG)pageBodies.size() * 1500ULL) {
                dur = std::max<ULONGLONG>(dur, 1500ULL);
            }
            ULONGLONG remainingMin = (totalDur >= (ULONGLONG)pageBodies.size() * 1200ULL)
                                     ? (ULONGLONG)(pageBodies.size() - 1 - i) * 1200ULL : 0ULL;
            if (cursor + dur + remainingMin > expireMs && expireMs > cursor + remainingMin) {
                dur = expireMs - cursor - remainingMin;
            }
            endMs = std::min(expireMs, cursor + std::max<ULONGLONG>(400ULL, dur));
        }
        pages.push_back({prefix + pageBodies[i], cursor, endMs});
        cursor = endMs;
    }
    return pages;
}

const std::wstring& GetActiveCaptionText(ActiveCaption& cap, ULONGLONG nowMs, int wrapW, int maxLinesPerCap) {
    if (cap.pages.empty() || cap.cachedWrapW != wrapW || cap.cachedMaxLines != maxLinesPerCap) {
        cap.pages = BuildCaptionPages(cap.text, cap.start_ms, cap.expire_ms, maxLinesPerCap, wrapW,
                                      [](const std::wstring& s) { return g_atlas.MeasureWidth(g_gl, s); });
        cap.cachedWrapW = wrapW;
        cap.cachedMaxLines = maxLinesPerCap;
    }
    for (const auto& p : cap.pages) {
        if (nowMs < p.end_ms) return p.text;
    }
    return cap.pages.back().text;
}

void DrawActiveTitles(int screenW, int screenH, int fontH) {
    ULONGLONG nowMs = GetTickCount64();
    int lineStep = fontH + 4;
    for (size_t idx = 0; idx < g_activeTitles.size(); ) {
        const auto& at = g_activeTitles[idx];
        ULONGLONG elapsed = (nowMs >= at.start_ms) ? (nowMs - at.start_ms) : 0;
        ULONGLONG total = at.fadein_ms + at.hold_ms + at.fadeout_ms;
        if (elapsed >= total) {
            g_activeTitles.erase(g_activeTitles.begin() + idx);
            continue;
        }
        idx++;

        float alphaF = 1.0f;
        if (at.fadein_ms > 0 && elapsed < at.fadein_ms) {
            alphaF = (float)elapsed / (float)at.fadein_ms;
        } else if (at.fadeout_ms > 0 && elapsed > at.fadein_ms + at.hold_ms) {
            ULONGLONG outEl = elapsed - (at.fadein_ms + at.hold_ms);
            alphaF = 1.0f - (float)outEl / (float)at.fadeout_ms;
        }
        int alpha = std::clamp((int)(alphaF * 255.0f), 0, 255);
        if (alpha <= 0) continue;

        std::vector<std::wstring> lines;
        size_t pos = 0;
        while (pos <= at.text.size()) {
            size_t nl = at.text.find(L'\n', pos);
            if (nl == std::wstring::npos) {
                lines.push_back(at.text.substr(pos));
                break;
            }
            lines.push_back(at.text.substr(pos, nl - pos));
            pos = nl + 1;
        }

        int totalH = (int)lines.size() * lineStep;
        int baseY = (at.y < 0.0f) ? (screenH - totalH) / 2 : (int)(at.y * (float)screenH);

        for (size_t li = 0; li < lines.size(); ++li) {
            if (lines[li].empty()) continue;
            int w = g_titleAtlas.MeasureWidth(g_gl, lines[li]);
            int x = (at.x < 0.0f) ? (screenW - w) / 2 : (int)(at.x * (float)screenW);
            int y = baseY + (int)li * lineStep;
            g_titleAtlas.DrawText(g_gl, x, y, lines[li], at.r, at.g, at.b, (uint8_t)alpha, false);
        }
    }
}

void DrawRoundedRect(int x0, int y0, int x1, int y1, int radius, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    int rad = std::max(0, std::min({radius, (x1 - x0) / 2, (y1 - y0) / 2}));
    g_gl.glDisable(glfont::GlApi::kTexture2D);
    g_gl.glColor4ub(r, g, b, a);
    if (rad <= 0) {
        g_gl.glBegin(glfont::GlApi::kQuads);
        g_gl.glVertex2f((float)x0, (float)y0);
        g_gl.glVertex2f((float)x1, (float)y0);
        g_gl.glVertex2f((float)x1, (float)y1);
        g_gl.glVertex2f((float)x0, (float)y1);
        g_gl.glEnd();
        g_gl.glEnable(glfont::GlApi::kTexture2D);
        return;
    }

    constexpr int kSegs = 8;
    constexpr float kHalfPi = 1.57079632679f;
    const float fr = (float)rad;
    const float cx[4] = {(float)(x1 - rad), (float)(x1 - rad), (float)(x0 + rad), (float)(x0 + rad)};
    const float cy[4] = {(float)(y0 + rad), (float)(y1 - rad), (float)(y1 - rad), (float)(y0 + rad)};
    const float startAng[4] = {-kHalfPi, 0.0f, kHalfPi, 2.0f * kHalfPi};

    g_gl.glBegin(glfont::GlApi::kTriangleFan);
    g_gl.glVertex2f((float)(x0 + x1) * 0.5f, (float)(y0 + y1) * 0.5f);
    for (int corner = 0; corner < 4; ++corner) {
        for (int s = 0; s <= kSegs; ++s) {
            float ang = startAng[corner] + kHalfPi * ((float)s / (float)kSegs);
            g_gl.glVertex2f(cx[corner] + std::cos(ang) * fr, cy[corner] + std::sin(ang) * fr);
        }
    }
    g_gl.glVertex2f(cx[0] + std::cos(startAng[0]) * fr, cy[0] + std::sin(startAng[0]) * fr);
    g_gl.glEnd();
    g_gl.glEnable(glfont::GlApi::kTexture2D);
}

void DrawChamferedRect(int x0, int y0, int x1, int y1, int chamf,
                       uint8_t r, uint8_t g, uint8_t b, uint8_t a,
                       uint8_t br, uint8_t bg, uint8_t bb, uint8_t ba) {
    int c = std::max(0, std::min({chamf, (x1 - x0) / 3, (y1 - y0) / 3}));
    const float vx[8] = {
        (float)(x0 + c), (float)(x1 - c), (float)x1, (float)x1,
        (float)(x1 - c), (float)(x0 + c), (float)x0, (float)x0
    };
    const float vy[8] = {
        (float)y0, (float)y0, (float)(y0 + c), (float)(y1 - c),
        (float)y1, (float)y1, (float)(y1 - c), (float)(y0 + c)
    };
    g_gl.glDisable(glfont::GlApi::kTexture2D);
    if (a > 0) {
        g_gl.glColor4ub(r, g, b, a);
        g_gl.glBegin(glfont::GlApi::kTriangleFan);
        g_gl.glVertex2f((float)(x0 + x1) * 0.5f, (float)(y0 + y1) * 0.5f);
        for (int i = 0; i < 8; ++i) g_gl.glVertex2f(vx[i], vy[i]);
        g_gl.glVertex2f(vx[0], vy[0]);
        g_gl.glEnd();
    }
    if (ba > 0) {
        g_gl.glColor4ub(br, bg, bb, ba);
        g_gl.glBegin(glfont::GlApi::kLineLoop);
        for (int i = 0; i < 8; ++i) g_gl.glVertex2f(vx[i], vy[i]);
        g_gl.glEnd();
    }
    g_gl.glEnable(glfont::GlApi::kTexture2D);
}

void DrawTerminalCursor(int x, int y, int fontH, uint8_t r, uint8_t g, uint8_t b, uint8_t alpha, bool glow, uint8_t glowA) {
    if (alpha == 0 || (GetTickCount64() / 480) % 2 != 0) return;
    int cw = std::max(5, (fontH * 42) / 100);
    int cy0 = y + std::max(2, fontH / 7);
    int cy1 = y + fontH - std::max(2, fontH / 8);
    g_gl.glDisable(glfont::GlApi::kTexture2D);
    if (glow && glowA > 0) {
        uint8_t ga = (uint8_t)(((int)std::min<int>(180, (int)glowA * 2) * (int)alpha) / 255);
        g_gl.glColor4ub(r, g, b, ga);
        g_gl.glBegin(glfont::GlApi::kQuads);
        g_gl.glVertex2f((float)(x - 1), (float)(cy0 - 1));
        g_gl.glVertex2f((float)(x + cw + 1), (float)(cy0 - 1));
        g_gl.glVertex2f((float)(x + cw + 1), (float)(cy1 + 1));
        g_gl.glVertex2f((float)(x - 1), (float)(cy1 + 1));
        g_gl.glEnd();
    }
    g_gl.glColor4ub(r, g, b, (uint8_t)(((int)240 * (int)alpha) / 255));
    g_gl.glBegin(glfont::GlApi::kQuads);
    g_gl.glVertex2f((float)x, (float)cy0);
    g_gl.glVertex2f((float)(x + cw), (float)cy0);
    g_gl.glVertex2f((float)(x + cw), (float)cy1);
    g_gl.glVertex2f((float)x, (float)cy1);
    g_gl.glEnd();
    g_gl.glEnable(glfont::GlApi::kTexture2D);
}

bool IsGamePausedLocked(ULONGLONG nowRealMs) {
    if (g_lastHudRedrawRealMs == 0 || (nowRealMs - g_lastHudRedrawRealMs) > 180) return true;
    if (g_lastTimeAdvanceRealMs == 0 || (nowRealMs - g_lastTimeAdvanceRealMs) > 180) return true;
    return false;
}

void RenderSubtitlesOverlay() {
    if (g_screenW <= 0 || g_screenH <= 0) return;
    if (!g_gl.Load()) return;
    HGLRC ctx = g_gl.wglGetCurrentContext();
    if (!ctx) return;

    AcquireSRWLockExclusive(&g_lock);
    if (IsGamePausedLocked(GetTickCount64()) || (g_captions.empty() && g_activeTitles.empty())) {
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }

    int tall = (g_cfg.font_size > 0) ? g_cfg.font_size : std::max(16, (g_screenH * 28) / 1080);
    std::wstring face = Utf8ToWide(g_cfg.font_name);
    if (face.empty()) face = L"Trebuchet MS";
    if (!g_atlas.EnsureReady(g_gl, ctx, face, tall)) {
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }

    g_gl.glPushAttrib(glfont::GlApi::kAllAttribBits);
    g_gl.glPushClientAttrib(glfont::GlApi::kClientAllAttribBits);

    if (g_gl.glActiveTextureARB) {
        for (int u = 1; u <= 3; ++u) {
            g_gl.glActiveTextureARB(glfont::GlApi::kTexture0 + u);
            g_gl.glDisable(glfont::GlApi::kTexture2D);
        }
        g_gl.glActiveTextureARB(glfont::GlApi::kTexture0);
    }

    g_gl.glDisable(glfont::GlApi::kDepthTest);
    g_gl.glDisable(glfont::GlApi::kCullFace);
    g_gl.glDisable(glfont::GlApi::kScissorTest);
    g_gl.glDisable(glfont::GlApi::kAlphaTest);
    g_gl.glDisable(glfont::GlApi::kLighting);
    g_gl.glDisable(glfont::GlApi::kFog);
    g_gl.glDepthMask(0);
    g_gl.glEnable(glfont::GlApi::kBlend);
    g_gl.glBlendFunc(glfont::GlApi::kSrcAlpha, glfont::GlApi::kOneMinusSrcAlpha);
    g_gl.glEnable(glfont::GlApi::kTexture2D);
    g_gl.glTexEnvi(glfont::GlApi::kTextureEnv, glfont::GlApi::kTextureEnvMode, glfont::GlApi::kModulate);

    g_gl.glMatrixMode(glfont::GlApi::kProjection);
    g_gl.glPushMatrix();
    g_gl.glLoadIdentity();
    g_gl.glOrtho(0.0, (double)g_screenW, (double)g_screenH, 0.0, -1.0, 1.0);
    g_gl.glMatrixMode(glfont::GlApi::kModelview);
    g_gl.glPushMatrix();
    g_gl.glLoadIdentity();
    g_gl.glMatrixMode(glfont::GlApi::kTexture);
    g_gl.glPushMatrix();
    g_gl.glLoadIdentity();

    int fontH = g_atlas.LineHeight();

    if (!g_activeTitles.empty()) {
        int titleTall = std::max(16, (g_screenH * 28) / 1080);
        if (g_titleAtlas.EnsureReady(g_gl, ctx, L"Trebuchet MS", titleTall)) {
            DrawActiveTitles(g_screenW, g_screenH, g_titleAtlas.LineHeight());
        }
    }

    if (!g_captions.empty()) {
        int maxBoxW = (g_screenW * g_cfg.max_width_pct) / 100;
        int lineStep = fontH + 3;

        struct RenderLine { std::wstring txt; subs::Rgb col; int w; uint8_t alpha; };
        std::vector<std::vector<RenderLine>> perCap;
        perCap.reserve(g_captions.size());
        int totalLines = 0;
        for (auto& cap : g_captions) {
            float fIn = (g_gameMs >= cap.start_ms)
                ? std::clamp((float)(g_gameMs - cap.start_ms) / 150.0f, 0.0f, 1.0f)
                : 0.0f;
            float fOut = (cap.expire_ms > g_gameMs)
                ? std::clamp((float)(cap.expire_ms - g_gameMs) / 220.0f, 0.0f, 1.0f)
                : 0.0f;
            uint8_t capAlpha = (uint8_t)std::lround(255.0f * std::min(fIn, fOut));

            const std::wstring& pageTxt = GetActiveCaptionText(
                cap, g_gameMs, maxBoxW - 20, g_cfg.max_lines_per_caption);
            std::vector<RenderLine> lines;
            for (auto& wl : WrapText(pageTxt, maxBoxW - 20)) {
                int w = g_atlas.MeasureWidth(g_gl, wl);
                lines.push_back({std::move(wl), cap.color, w, capAlpha});
            }
            totalLines += (int)lines.size();
            perCap.push_back(std::move(lines));
        }
        size_t firstCap = 0;
        while (perCap.size() - firstCap > 1 && totalLines > g_cfg.max_lines) {
            totalLines -= (int)perCap[firstCap].size();
            ++firstCap;
        }
        if (firstCap > 0) {
            g_captions.erase(g_captions.begin(), g_captions.begin() + firstCap);
        }
        const int padX = 12, padY = 6;
        const int baseBottomY = g_screenH - (g_screenH * g_cfg.bottom_margin_pct) / 100;
        const uint8_t bgAlpha = (uint8_t)std::clamp(g_cfg.bg_alpha, 0, 255);

        if (g_cfg.bg_style == "cards") {
            const int gap = 5;
            int curBottomY = baseBottomY;
            for (size_t ci = perCap.size(); ci-- > firstCap; ) {
                const auto& lines = perCap[ci];
                if (lines.empty()) continue;
                const uint8_t ca = lines[0].alpha;
                if (ca == 0) continue;
                int widest = 0;
                for (const auto& rl : lines) widest = std::max(widest, rl.w);
                int cardW = widest + padX * 2 + 8;
                int cardH = (int)lines.size() * lineStep + padY * 2 - 3;
                int cardX = (g_screenW - cardW) / 2;
                int cardY = curBottomY - cardH;

                uint8_t effBgA = (uint8_t)(((int)bgAlpha * (int)ca) / 255);
                if (effBgA > 0) {
                    DrawRoundedRect(cardX, cardY, cardX + cardW, cardY + cardH, g_cfg.bg_radius,
                                    g_cfg.bg_color.r, g_cfg.bg_color.g, g_cfg.bg_color.b, effBgA);
                }
                const subs::Rgb accent = lines[0].col;
                DrawRoundedRect(cardX + 4, cardY + 5, cardX + 7, cardY + cardH - 5, 1,
                                accent.r, accent.g, accent.b, (uint8_t)(((int)230 * (int)ca) / 255));

                int y = cardY + padY;
                const uint8_t glowA = (uint8_t)std::clamp(g_cfg.text_glow_alpha, 0, 255);
                const bool drawCursorOnCard = g_cfg.text_cursor && (ci + 1 == perCap.size());
                for (size_t li = 0; li < lines.size(); ++li) {
                    const auto& rl = lines[li];
                    int x = cardX + padX + 8 + (widest - rl.w) / 2;
                    g_atlas.DrawText(g_gl, x, y, rl.txt, rl.col.r, rl.col.g, rl.col.b, rl.alpha,
                                     true, g_cfg.text_glow,
                                     rl.col.r, rl.col.g, rl.col.b, glowA);
                    if (drawCursorOnCard && li + 1 == lines.size()) {
                        DrawTerminalCursor(x + rl.w + 4, y, fontH, rl.col.r, rl.col.g, rl.col.b, rl.alpha, g_cfg.text_glow, glowA);
                    }
                    y += lineStep;
                }
                curBottomY = cardY - gap;
            }
        } else {
            std::vector<RenderLine> allLines;
            uint8_t maxLineAlpha = 0;
            for (size_t ci = firstCap; ci < perCap.size(); ++ci) {
                for (auto& rl : perCap[ci]) {
                    maxLineAlpha = std::max(maxLineAlpha, rl.alpha);
                    allLines.push_back(std::move(rl));
                }
            }

            if (!allLines.empty() && maxLineAlpha > 0) {
                int widest = 0;
                for (const auto& rl : allLines) widest = std::max(widest, rl.w);
                int cursorExtra = g_cfg.text_cursor ? std::max(10, fontH / 2) : 0;
                int boxW = widest + padX * 2 + cursorExtra;
                int boxH = (int)allLines.size() * lineStep + padY * 2 - 3;
                int boxX = (g_screenW - boxW) / 2;
                int boxY = baseBottomY - boxH;

                uint8_t effBgA = (uint8_t)(((int)bgAlpha * (int)maxLineAlpha) / 255);
                uint8_t effBorderA = (uint8_t)(((int)std::clamp(g_cfg.bg_border_alpha, 0, 255) * (int)maxLineAlpha) / 255);
                if (g_cfg.bg_style == "hev") {
                    DrawChamferedRect(boxX, boxY, boxX + boxW, boxY + boxH, g_cfg.bg_radius,
                                      g_cfg.bg_color.r, g_cfg.bg_color.g, g_cfg.bg_color.b, effBgA,
                                      g_cfg.bg_border_color.r, g_cfg.bg_border_color.g, g_cfg.bg_border_color.b,
                                      effBorderA);
                } else if (effBgA > 0) {
                    DrawRoundedRect(boxX, boxY, boxX + boxW, boxY + boxH, g_cfg.bg_radius,
                                    g_cfg.bg_color.r, g_cfg.bg_color.g, g_cfg.bg_color.b, effBgA);
                }

                int y = boxY + padY;
                const uint8_t glowA = (uint8_t)std::clamp(g_cfg.text_glow_alpha, 0, 255);
                for (size_t li = 0; li < allLines.size(); ++li) {
                    const auto& rl = allLines[li];
                    if (rl.alpha > 0) {
                        int x = (g_screenW - rl.w - cursorExtra / 2) / 2;
                        g_atlas.DrawText(g_gl, x, y, rl.txt, rl.col.r, rl.col.g, rl.col.b, rl.alpha,
                                         true, g_cfg.text_glow,
                                         rl.col.r, rl.col.g, rl.col.b, glowA);
                        if (g_cfg.text_cursor && li + 1 == allLines.size()) {
                            DrawTerminalCursor(x + rl.w + 4, y, fontH, rl.col.r, rl.col.g, rl.col.b, rl.alpha, g_cfg.text_glow, glowA);
                        }
                    }
                    y += lineStep;
                }
            }
        }
    }

    g_gl.glMatrixMode(glfont::GlApi::kTexture);
    g_gl.glPopMatrix();
    g_gl.glMatrixMode(glfont::GlApi::kModelview);
    g_gl.glPopMatrix();
    g_gl.glMatrixMode(glfont::GlApi::kProjection);
    g_gl.glPopMatrix();
    g_gl.glPopClientAttrib();
    g_gl.glPopAttrib();

    ReleaseSRWLockExclusive(&g_lock);
}

BOOL WINAPI Hook_WglSwapBuffers(HDC hdc) {
    RenderSubtitlesOverlay();
    return g_origWglSwapBuffers ? g_origWglSwapBuffers(hdc) : TRUE;
}

int __cdecl Hook_HUD_Redraw(float time, int intermission) {
    int ret = g_origHudRedraw ? g_origHudRedraw(time, intermission) : 0;
    CheckIniHotReload();
    if (!g_engfuncs || !g_engfuncs->pfnGetScreenInfo) return ret;

    SCREENINFO si{};
    si.iSize = sizeof(si);
    g_engfuncs->pfnGetScreenInfo(&si);
    if (si.iWidth <= 0 || si.iHeight <= 0) return ret;

    float curClientTime = (g_engfuncs->GetClientTime) ? g_engfuncs->GetClientTime() : time;
    ULONGLONG realNow = GetTickCount64();

    AcquireSRWLockExclusive(&g_lock);
    g_screenW = si.iWidth;
    g_screenH = si.iHeight;
    g_lastHudRedrawRealMs = realNow;
    bool rewound = (curClientTime > 0.0f && curClientTime + 0.01f < g_lastClientTime);
    if (g_resetRequested || rewound) {
        ClearCaptionBuffersLocked();
        g_resetRequested = false;
        g_clockPrevTime = curClientTime;   // do not advance on discontinuity
        g_lastTimeAdvanceRealMs = realNow;
    }
    g_lastClientTime = curClientTime;

    // Advance the game clock by the client-time delta (frozen while paused,
    // capped per frame so a level load / hitch doesn't expire everything).
    if (g_clockPrevTime < 0.0f) {
        g_lastTimeAdvanceRealMs = realNow;
    } else {
        float dt = curClientTime - g_clockPrevTime;
        if (dt > 0.0f) {
            g_gameMs += (ULONGLONG)(std::min(dt, 0.25f) * 1000.0f);
            g_lastTimeAdvanceRealMs = realNow;
        }
    }
    g_clockPrevTime = curClientTime;
    const ULONGLONG nowMs = g_gameMs;

    g_captions.erase(
        std::remove_if(g_captions.begin(), g_captions.end(),
                       [nowMs](const ActiveCaption& c) { return c.expire_ms <= nowMs; }),
        g_captions.end());
    ReleaseSRWLockExclusive(&g_lock);
    return ret;
}

void __cdecl Hook_V_CalcRefdef(ref_params_t* pparams) {
    if (g_origCalcRefdef) g_origCalcRefdef(pparams);
    if (pparams) {
        g_viewOrg[0] = pparams->vieworg[0];
        g_viewOrg[1] = pparams->vieworg[1];
        g_viewOrg[2] = pparams->vieworg[2];
        g_hasViewOrg = true;
    }
}

void __cdecl Hook_HUD_Reset() {
    AcquireSRWLockExclusive(&g_lock);
    ClearCaptionBuffersLocked();
    ResetHevStateLocked();
    g_clockPrevTime = -1.0f;
    g_lastClientTime = 0.0f;
    g_hasViewOrg = false;
    ReleaseSRWLockExclusive(&g_lock);
    if (g_origHudReset) g_origHudReset();
}

void __cdecl Cmd_SubtitlesClear() {
    AcquireSRWLockExclusive(&g_lock);
    ClearCaptionBuffersLocked();
    ReleaseSRWLockExclusive(&g_lock);
}

void PlayHevSentence(const char* sentence) {
    if (!g_engfuncs || !sentence || !*sentence) return;
    if (g_engfuncs->pfnClientCmd) {
        char cmd[128];
        std::snprintf(cmd, sizeof(cmd), "speak \"%s\"\n", sentence);
        g_engfuncs->pfnClientCmd(cmd);
    } else if (g_engfuncs->pfnPlaySoundByName) {
        g_engfuncs->pfnPlaySoundByName(sentence, 0.85f);
    }
}

bool TryTriggerRestoredHev(HevCue cue, const char* sentenceName) {
    AcquireSRWLockExclusive(&g_lock);
    const ULONGLONG nowMs = g_gameMs;
    bool ok = g_cfg.restore_unused_hev && !g_isMModGame && g_cfg.enable_hev && g_hevVoiceEnabled &&
              nowMs >= 2000 &&
              (g_lastNativeHevMs == 0 || nowMs - g_lastNativeHevMs >= 1800) &&
              (g_hevCueCooldown[cue] == 0 || nowMs - g_hevCueCooldown[cue] >= 15000);
    if (ok) {
        g_hevCueCooldown[cue] = nowMs;
        g_lastNativeHevMs = nowMs;
    }
    ReleaseSRWLockExclusive(&g_lock);

    if (ok) {
        PlayHevSentence(sentenceName);
        LogMsg("[hev-restore] played %s", sentenceName);
    }
    return ok;
}

void __cdecl Cmd_HevVoice() {
    g_hevVoiceEnabled = !g_hevVoiceEnabled;
    PlayHevSentence(g_hevVoiceEnabled ? "!HEV_V0" : "!HEV_V1");
}

void OnBatteryValue(int battery) {
    int prev = g_prevBattery;
    g_prevBattery = battery;
    if (prev < 0) return;
    if (prev > 0 && battery == 0) {
        TryTriggerRestoredHev(kHevArmorGone, (prev % 2 == 0) ? "!HEV_E0" : "!HEV_E1");
    } else if (prev == 0 && battery > 0) {
        TryTriggerRestoredHev(kHevPowerRestored, "!HEV_D00");
    }
}

void OnHealthValue(int health) {
    int prev = g_prevHealth;
    g_prevHealth = health;
    if (prev < 0) return;
    if (prev > 15 && health > 0 && health <= 15) {
        TryTriggerRestoredHev(kHevSeekMedic, "!HEV_DMG7");
    }
}

void OnDamageEvent(int dmgSave, int dmgTake, uint32_t dmgBits) {
    if (dmgSave + dmgTake <= 0) return;
    constexpr uint32_t kDmgBurn     = (1u << 3) | (1u << 21);
    constexpr uint32_t kDmgFall     = (1u << 5);
    constexpr uint32_t kDmgBlast    = (1u << 6);
    constexpr uint32_t kDmgShock    = (1u << 8);
    constexpr uint32_t kDmgPhysical = (1u << 1) | (1u << 2) | (1u << 7);

    if (dmgBits & kDmgShock) {
        TryTriggerRestoredHev(kHevShock, "!HEV_SHOCK");
    } else if (dmgBits & kDmgBurn) {
        TryTriggerRestoredHev(kHevFire, "!HEV_FIRE");
    } else if ((dmgBits & kDmgFall) && dmgTake >= 10) {
        TryTriggerRestoredHev(kHevFracture, (dmgTake >= 25) ? "!HEV_DMG5" : "!HEV_DMG4");
    } else if ((dmgBits & kDmgBlast) && dmgTake >= 25) {
        TryTriggerRestoredHev(kHevBloodLoss, "!HEV_DMG6");
    } else if ((dmgBits & kDmgPhysical) && dmgTake >= 20) {
        TryTriggerRestoredHev(kHevLaceration, "!HEV_DMG1");
    } else if (dmgSave >= 20 && g_prevBattery > 0) {
        TryTriggerRestoredHev((g_prevBattery <= 10) ? kHevSuitCrit : kHevSuitDmg,
                              (g_prevBattery <= 10) ? "!HEV_E2" : "!HEV_E4");
    }
}

int __cdecl Hook_MsgFunc_Battery(const char* pszName, int iSize, void* pbuf) {
    if (pbuf && iSize >= 1) {
        const uint8_t* b = static_cast<const uint8_t*>(pbuf);
        int val = (iSize >= 2) ? (int)(int16_t)(b[0] | (b[1] << 8)) : (int)b[0];
        OnBatteryValue(val);
    }
    return g_origMsgBattery ? g_origMsgBattery(pszName, iSize, pbuf) : 1;
}

int __cdecl Hook_MsgFunc_Health(const char* pszName, int iSize, void* pbuf) {
    if (pbuf && iSize >= 1) {
        OnHealthValue((int)static_cast<const uint8_t*>(pbuf)[0]);
    }
    return g_origMsgHealth ? g_origMsgHealth(pszName, iSize, pbuf) : 1;
}

int __cdecl Hook_MsgFunc_Damage(const char* pszName, int iSize, void* pbuf) {
    if (pbuf && iSize >= 6) {
        const uint8_t* b = static_cast<const uint8_t*>(pbuf);
        int dmgSave = b[0];
        int dmgTake = b[1];
        uint32_t bits = (uint32_t)b[2] | ((uint32_t)b[3] << 8) | ((uint32_t)b[4] << 16) | ((uint32_t)b[5] << 24);
        OnDamageEvent(dmgSave, dmgTake, bits);
    }
    return g_origMsgDamage ? g_origMsgDamage(pszName, iSize, pbuf) : 1;
}

const char* WeaponIdToHevSentence(int id) {
    switch (id) {
        case 2:  return "!HEV_PISTOL";
        case 3:  return "!HEV_44PISTOL";
        case 4:  return "!HEV_ASSAULT";
        case 6:  return "!HEV_XBOW";
        case 7:  return "!HEV_SHOTGUN";
        case 8:  return "!HEV_RPG";
        case 9:  return "!HEV_GAUSS";
        case 10: return "!HEV_EGON";
        case 11: return "!HEV_HORNET";
        case 12: return "!HEV_GRENADE";
        case 13: return "!HEV_TRIPMINE";
        case 14: return "!HEV_SATCHEL";
        case 15: return "!HEV_SQUEEK";
        default: return nullptr;
    }
}

void OnWeapPickup(int weapId) {
    if (weapId < 0 || weapId >= 32) return;
    const char* sentence = WeaponIdToHevSentence(weapId);
    if (!sentence) return;

    AcquireSRWLockExclusive(&g_lock);
    const ULONGLONG nowMs = g_gameMs;
    uint32_t bit = (1u << weapId);
    bool ok = g_cfg.restore_unused_hev && !g_isMModGame && g_cfg.enable_hev && g_hevVoiceEnabled &&
              nowMs >= 1000 && (g_announcedWeaponsMask & bit) == 0;
    if (ok) {
        g_announcedWeaponsMask |= bit;
        g_lastNativeHevMs = nowMs;
    }
    ReleaseSRWLockExclusive(&g_lock);

    if (ok) {
        PlayHevSentence(sentence);
        LogMsg("[hev-restore] weapon=%d -> %s", weapId, sentence);
    }
}

int __cdecl Hook_MsgFunc_WeapPickup(const char* pszName, int iSize, void* pbuf) {
    if (pbuf && iSize >= 1) {
        OnWeapPickup((int)static_cast<const uint8_t*>(pbuf)[0]);
    }
    return g_origMsgWeapPickup ? g_origMsgWeapPickup(pszName, iSize, pbuf) : 1;
}

int __cdecl Hook_pfnHookUserMsg(const char* szMsgName, void* pfn) {
    if (szMsgName && pfn) {
        if (std::strcmp(szMsgName, "Battery") == 0 && pfn != (void*)Hook_MsgFunc_Battery) {
            g_origMsgBattery = reinterpret_cast<FnUserMsg>(pfn);
            pfn = (void*)Hook_MsgFunc_Battery;
        } else if (std::strcmp(szMsgName, "Health") == 0 && pfn != (void*)Hook_MsgFunc_Health) {
            g_origMsgHealth = reinterpret_cast<FnUserMsg>(pfn);
            pfn = (void*)Hook_MsgFunc_Health;
        } else if (std::strcmp(szMsgName, "Damage") == 0 && pfn != (void*)Hook_MsgFunc_Damage) {
            g_origMsgDamage = reinterpret_cast<FnUserMsg>(pfn);
            pfn = (void*)Hook_MsgFunc_Damage;
        } else if (std::strcmp(szMsgName, "WeapPickup") == 0 && pfn != (void*)Hook_MsgFunc_WeapPickup) {
            g_origMsgWeapPickup = reinterpret_cast<FnUserMsg>(pfn);
            pfn = (void*)Hook_MsgFunc_WeapPickup;
        }
    }
    return g_origHookUserMsg ? g_origHookUserMsg(szMsgName, pfn) : 0;
}

void HookClientEngfuncs(cl_enginefunc_t* eng) {
    if (!eng) return;
    if (!g_engfuncs) g_engfuncs = eng;
    if (eng->pfnTextMessageGet && eng->pfnTextMessageGet != Hook_TextMessageGet) {
        if (!g_origTextMsgGet) {
            g_origTextMsgGet = eng->pfnTextMessageGet;
        }
        DWORD oldProt = 0;
        if (VirtualProtect(&eng->pfnTextMessageGet, sizeof(void*), PAGE_READWRITE, &oldProt)) {
            eng->pfnTextMessageGet = Hook_TextMessageGet;
            VirtualProtect(&eng->pfnTextMessageGet, sizeof(void*), oldProt, &oldProt);
        }
    }
    if (eng->pfnHookUserMsg && eng->pfnHookUserMsg != Hook_pfnHookUserMsg) {
        if (!g_origHookUserMsg) {
            g_origHookUserMsg = eng->pfnHookUserMsg;
        }
        DWORD oldProt = 0;
        if (VirtualProtect(&eng->pfnHookUserMsg, sizeof(void*), PAGE_READWRITE, &oldProt)) {
            eng->pfnHookUserMsg = Hook_pfnHookUserMsg;
            VirtualProtect(&eng->pfnHookUserMsg, sizeof(void*), oldProt, &oldProt);
        }
    }
}

cl_enginefunc_t* FindClientEngfuncs(void* pInit) {
    if (!pInit) return nullptr;
    const uint8_t* code = reinterpret_cast<const uint8_t*>(pInit);
    for (int i = 0; i < 56; ++i) {
        if (code[i] == 0xBF && code[i + 5] == 0xF3 && code[i + 6] == 0xA5) {
            uintptr_t addr = *reinterpret_cast<const uintptr_t*>(code + i + 1);
            if (addr > 0x10000) return reinterpret_cast<cl_enginefunc_t*>(addr);
        }
    }
    return nullptr;
}

int __cdecl Hook_Initialize(cl_enginefunc_t* pEnginefuncs, int iVersion) {
    HookClientEngfuncs(pEnginefuncs);
    int r = g_origInitialize ? g_origInitialize(pEnginefuncs, iVersion) : 0;
    g_engfuncs = pEnginefuncs;
    if (pEnginefuncs) {
        if (pEnginefuncs->pfnTextMessageGet && !g_origTextMsgGet &&
            pEnginefuncs->pfnTextMessageGet != Hook_TextMessageGet) {
            g_origTextMsgGet = reinterpret_cast<FnTextMsgGet>(
                hook::InstallInlineHook((void*)pEnginefuncs->pfnTextMessageGet, (void*)Hook_TextMessageGet));
            LogMsg("[client] Hooked pfnTextMessageGet at %p -> %p",
                   (void*)pEnginefuncs->pfnTextMessageGet, (void*)g_origTextMsgGet);
        }
        if (HMODULE cl = GetModuleHandleA("client.dll")) {
            if (cl_enginefunc_t* clientEng = FindClientEngfuncs((void*)GetProcAddress(cl, "Initialize"))) {
                HookClientEngfuncs(clientEng);
            }
        }
        if (pEnginefuncs->pfnGetGameDirectory) {
            const char* gd = pEnginefuncs->pfnGetGameDirectory();
            if (gd && *gd && subs::Config::Lower(gd) != subs::Config::Lower(g_activeGameDir)) {
                g_activeGameDir = gd;
                ReloadAllFromDisk(true);
            }
        }
        if (pEnginefuncs->pfnAddCommand) {
            pEnginefuncs->pfnAddCommand("subtitles_reload", Cmd_SubtitlesReload);
            pEnginefuncs->pfnAddCommand("subtitles_clear", Cmd_SubtitlesClear);
            pEnginefuncs->pfnAddCommand("hev_voice", Cmd_HevVoice);
        }
    }
    return r;
}

void __cdecl Hook_Host_Loadgame_f() {
    AcquireSRWLockExclusive(&g_lock);
    ClearCaptionBuffersLocked();
    ResetHevStateLocked();
    g_clockPrevTime = -1.0f;
    g_lastClientTime = 0.0f;
    g_resetRequested = false;
    g_hasViewOrg = false;
    ReleaseSRWLockExclusive(&g_lock);
    LogMsg("[load] Host_Loadgame_f cleared caption buffers");
    if (g_origLoadgame) g_origLoadgame();
}

void TryHookHw(HMODULE hw) {
    if (g_hwHooked || !hw) return;
    void* dyn  = hook::FindFunctionByStringXref(hw, "S_StartDynamicSound: %s volume > 255");
    void* stat = hook::FindFunctionByStringXref(hw, "S_StartStaticSound: %s volume > 255");
    void* load = hook::FindFunctionByStringXref(hw, "Loading game from");
    if (dyn)  g_origDynSound  = reinterpret_cast<FnStartSound>(hook::InstallInlineHook(dyn, (void*)Hook_S_StartDynamicSound));
    if (stat) g_origStatSound = reinterpret_cast<FnStartSound>(hook::InstallInlineHook(stat, (void*)Hook_S_StartStaticSound));
    if (load) g_origLoadgame  = reinterpret_cast<FnLoadgame>(hook::InstallInlineHook(load, (void*)Hook_Host_Loadgame_f));
    g_hwHooked = (g_origDynSound != nullptr || g_origStatSound != nullptr);
    LogMsg("[hw] dyn=%p->%p stat=%p->%p load=%p->%p",
           dyn, (void*)g_origDynSound, stat, (void*)g_origStatSound, load, (void*)g_origLoadgame);
}

void TryHookClient(HMODULE cl) {
    if (g_clientHooked || !cl) return;
    void* pInit   = (void*)GetProcAddress(cl, "Initialize");
    void* pRedraw = (void*)GetProcAddress(cl, "HUD_Redraw");
    void* pCalc   = (void*)GetProcAddress(cl, "V_CalcRefdef");
    void* pReset  = (void*)GetProcAddress(cl, "HUD_Reset");
    cl_enginefunc_t* clientEng = FindClientEngfuncs(pInit);
    if (clientEng && clientEng->pfnGetScreenInfo) {
        HookClientEngfuncs(clientEng);
    }
    if (pInit)   g_origInitialize = reinterpret_cast<FnInitialize>(hook::InstallInlineHook(pInit, (void*)Hook_Initialize));
    if (pRedraw) g_origHudRedraw  = reinterpret_cast<FnHudRedraw>(hook::InstallInlineHook(pRedraw, (void*)Hook_HUD_Redraw));
    if (pCalc)   g_origCalcRefdef = reinterpret_cast<FnCalcRefdef>(hook::InstallInlineHook(pCalc, (void*)Hook_V_CalcRefdef));
    if (pReset)  g_origHudReset   = reinterpret_cast<FnHudReset>(hook::InstallInlineHook(pReset, (void*)Hook_HUD_Reset));
    g_clientHooked = (g_origInitialize != nullptr && g_origHudRedraw != nullptr);
    LogMsg("[client] init=%p redraw=%p calc=%p reset=%p clientEng=%p", pInit, pRedraw, pCalc, pReset, (void*)clientEng);
}

void TryHookOpenGL(HMODULE gl) {
    if (g_glHooked || !gl) return;
    void* pSwap = (void*)GetProcAddress(gl, "wglSwapBuffers");
    if (pSwap) {
        g_origWglSwapBuffers = reinterpret_cast<FnWglSwapBuffers>(
            hook::InstallInlineHook(pSwap, (void*)Hook_WglSwapBuffers));
        g_glHooked = (g_origWglSwapBuffers != nullptr);
    }
    LogMsg("[gl] wglSwapBuffers=%p->%p", pSwap, (void*)g_origWglSwapBuffers);
}

HMODULE WINAPI Hook_LoadLibraryA(LPCSTR lpLibFileName) {
    HMODULE m = g_origLoadLibraryA ? g_origLoadLibraryA(lpLibFileName) : nullptr;
    if (m && lpLibFileName) {
        std::string s = subs::Config::Lower(lpLibFileName);
        if (s.find("hw.dll") != std::string::npos) TryHookHw(m);
        else if (s.find("client.dll") != std::string::npos) TryHookClient(m);
        else if (s.find("opengl32.dll") != std::string::npos) TryHookOpenGL(m);
    }
    return m;
}

void InitMod() {
    char exePath[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string ep = exePath;
    size_t sl = ep.find_last_of("\\/");
    g_exeDir = (sl != std::string::npos) ? ep.substr(0, sl) : ".";
    g_iniPath = g_exeDir + "\\subtitles.ini";
    g_activeGameDir = ParseCmdlineGameDir();

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExA(g_iniPath.c_str(), GetFileExInfoStandard, &fad)) {
        g_iniMtime = fad.ftLastWriteTime;
    }
    ReloadAllFromDisk(true);

    if (HMODULE hw = GetModuleHandleA("hw.dll")) TryHookHw(hw);
    if (HMODULE cl = GetModuleHandleA("client.dll")) TryHookClient(cl);
    if (HMODULE gl = GetModuleHandleA("opengl32.dll")) TryHookOpenGL(gl);
    if (HMODULE k32 = GetModuleHandleA("kernel32.dll")) {
        void* pLL = (void*)GetProcAddress(k32, "LoadLibraryA");
        if (pLL) g_origLoadLibraryA = reinterpret_cast<FnLoadLibraryA>(hook::InstallInlineHook(pLL, (void*)Hook_LoadLibraryA));
    }
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        InitMod();
    }
    return TRUE;
}
