#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace subs {

struct Rgb {
    int r = 240, g = 242, b = 245;
};

enum class RoleCat { NPC, VOX, HEV };

struct RoleRule {
    std::string prefix;
    Rgb color;
    RoleCat cat = RoleCat::NPC;
};

struct Config {
    // [Filtering]
    float max_distance = 650.0f;
    float glass_max_distance = 320.0f;
    float min_volume = 0.05f;
    bool check_occlusion = true;
    bool enable_vox = true;
    bool enable_hev = true;
    bool restore_unused_hev = true;
    bool enable_npc = true;
    bool debug_log = false;

    // [Visual]
    std::string preset = "default";
    std::string font_name = "Tahoma";
    int font_size = 0;
    int bottom_margin_pct = 13;
    int max_width_pct = 62;
    Rgb bg_color{0, 0, 0};
    int bg_alpha = 128;
    std::string bg_style = "rounded";
    int bg_radius = 8;
    Rgb bg_border_color{255, 165, 40};
    int bg_border_alpha = 0;
    bool text_glow = false;
    int text_glow_alpha = 26;
    bool text_cursor = false;
    int max_lines = 4;
    int max_lines_per_caption = 3;

    void ApplyPreset(const std::string& name) {
        std::string p = Lower(name);
        if (p == "hev") {
            preset = "hev";
            font_name = "Classic Console Neue";
            bg_style = "hev";
            bg_color = {12, 10, 6};
            bg_alpha = 170;
            bg_radius = 7;
            bg_border_color = {255, 165, 40};
            bg_border_alpha = 190;
            text_glow = true;
            text_glow_alpha = 26;
            text_cursor = true;
        } else if (p == "modern") {
            preset = "modern";
            font_name = "Segoe UI";
            bg_style = "cards";
            bg_color = {16, 18, 22};
            bg_alpha = 175;
            bg_radius = 8;
            bg_border_color = {255, 165, 40};
            bg_border_alpha = 0;
            text_glow = false;
            text_cursor = false;
        } else {
            preset = "default";
            font_name = "Tahoma";
            bg_style = "rounded";
            bg_color = {0, 0, 0};
            bg_alpha = 128;
            bg_radius = 8;
            bg_border_color = {255, 165, 40};
            bg_border_alpha = 0;
            text_glow = false;
            text_cursor = false;
        }
    }

    // [Files]
    std::string language = "auto";
    std::string fallback_dir = "valve";
    std::string subtitles_pattern = "subtitles*.tsv";
    std::string titles_pattern = "titles*.tsv";

    // [Colors]
    std::unordered_map<std::string, Rgb> named_colors;
    Rgb default_color{240, 242, 245};

    // [Roles]
    std::vector<RoleRule> roles;

    static std::string DetectAutoLanguage() {
        if (const char* cmd = GetCommandLineA()) {
            std::string lc = Lower(cmd);
            if (lc.find("-language russian") != std::string::npos) return "ru";
            if (lc.find("-language english") != std::string::npos) return "en";
        }
        HKEY hKey = nullptr;
        if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            char buf[64]{};
            DWORD sz = sizeof(buf);
            DWORD type = 0;
            LSTATUS st = RegQueryValueExA(hKey, "Language", nullptr, &type, reinterpret_cast<LPBYTE>(buf), &sz);
            RegCloseKey(hKey);
            if (st == ERROR_SUCCESS && type == REG_SZ && buf[0]) {
                return (Lower(buf) == "russian") ? "ru" : "en";
            }
        }
        return ((GetUserDefaultUILanguage() & 0xFF) == LANG_RUSSIAN) ? "ru" : "en";
    }

    void ApplyLanguage(const std::string& lang) {
        std::string l = Lower(lang);
        if (l == "auto") l = DetectAutoLanguage();
        if (l == "ru" || l == "russian") {
            language = "ru";
            subtitles_pattern = "subtitles_ru.tsv";
            titles_pattern = "titles_ru.tsv";
        } else if (l == "en" || l == "english") {
            language = "en";
            subtitles_pattern = "subtitles_en.tsv";
            titles_pattern = "titles_en.tsv";
        }
    }

    static std::string Trim(const std::string& s) {
        size_t a = 0, b = s.size();
        while (a < b && (unsigned char)s[a] <= ' ') a++;
        while (b > a && (unsigned char)s[b - 1] <= ' ') b--;
        return s.substr(a, b - a);
    }

    static std::string Lower(std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

    static int ParseInt(const std::string& val, int def) {
        try {
            return val.empty() ? def : std::stoi(val);
        } catch (...) {
            return def;
        }
    }

    static float ParseFloat(const std::string& val, float def) {
        try {
            return val.empty() ? def : std::stof(val);
        } catch (...) {
            return def;
        }
    }

    static Rgb ParseRgb(const std::string& val, Rgb def) {
        int r = def.r, g = def.g, b = def.b;
        char c1 = 0, c2 = 0;
        std::istringstream iss(val);
        if ((iss >> r >> c1 >> g >> c2 >> b) && c1 == ',' && c2 == ',') {
            return {std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b, 0, 255)};
        }
        return def;
    }

    void InitDefaultRoles() {
        auto col = [&](const char* k, Rgb def) {
            auto it = named_colors.find(k);
            return it != named_colors.end() ? it->second : def;
        };
        roles.clear();
        roles.push_back({"scientist/", col("color_scientist", {255,165,55}), RoleCat::NPC});
        roles.push_back({"sc_",        col("color_scientist", {255,165,55}), RoleCat::NPC});
        roles.push_back({"sci_",       col("color_scientist", {255,165,55}), RoleCat::NPC});
        roles.push_back({"barney/",    col("color_guard",     {100,175,255}), RoleCat::NPC});
        roles.push_back({"ba_",        col("color_guard",     {100,175,255}), RoleCat::NPC});
        roles.push_back({"hgrunt/",    col("color_enemy",     {120,235,120}), RoleCat::NPC});
        roles.push_back({"hg_",        col("color_enemy",     {120,235,120}), RoleCat::NPC});
        roles.push_back({"fvox/",      col("color_hev",       {255,195,65}), RoleCat::HEV});
        roles.push_back({"hev_",       col("color_hev",       {255,195,65}), RoleCat::HEV});
        roles.push_back({"vox/",       col("color_vox",       {155,215,255}), RoleCat::VOX});
        roles.push_back({"vox_",       col("color_vox",       {155,215,255}), RoleCat::VOX});
        roles.push_back({"tride/",     col("color_vox",       {155,215,255}), RoleCat::VOX});
        roles.push_back({"gman/",      col("color_gman",      {205,185,255}), RoleCat::NPC});
        roles.push_back({"gm_",        col("color_gman",      {205,185,255}), RoleCat::NPC});
        roles.push_back({"nihilanth/", col("color_gman",      {205,185,255}), RoleCat::NPC});
        roles.push_back({"holo/",      col("color_holo",      {140,245,220}), RoleCat::NPC});
        roles.push_back({"holo_",      col("color_holo",      {140,245,220}), RoleCat::NPC});
    }

    bool LoadIni(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            InitDefaultRoles();
            return false;
        }
        ApplyPreset("default");
        named_colors.clear();
        roles.clear();
        bool has_roles_section = false;
        std::string section, line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::string t = Trim(line);
            if (t.empty() || t[0] == ';' || t[0] == '#') continue;
            if (t.front() == '[' && t.back() == ']') {
                section = Lower(t.substr(1, t.size() - 2));
                if (section == "roles") has_roles_section = true;
                continue;
            }
            size_t eq = t.find('=');
            if (eq == std::string::npos) continue;
            std::string k = Lower(Trim(t.substr(0, eq)));
            std::string v = Trim(t.substr(eq + 1));

            if (section == "filtering") {
                if (k == "max_distance") max_distance = ParseFloat(v, max_distance);
                else if (k == "glass_max_distance") glass_max_distance = ParseFloat(v, glass_max_distance);
                else if (k == "min_volume") min_volume = ParseFloat(v, min_volume);
                else if (k == "check_occlusion") check_occlusion = (ParseInt(v, check_occlusion ? 1 : 0) != 0);
                else if (k == "enable_vox") enable_vox = (ParseInt(v, enable_vox ? 1 : 0) != 0);
                else if (k == "enable_hev") enable_hev = (ParseInt(v, enable_hev ? 1 : 0) != 0);
                else if (k == "restore_unused_hev") restore_unused_hev = (ParseInt(v, restore_unused_hev ? 1 : 0) != 0);
                else if (k == "enable_npc") enable_npc = (ParseInt(v, enable_npc ? 1 : 0) != 0);
                else if (k == "debug_log") debug_log = (ParseInt(v, debug_log ? 1 : 0) != 0);
            } else if (section == "visual") {
                if (k == "preset") ApplyPreset(v);
                else if (k == "font_name") font_name = v;
                else if (k == "font_size") font_size = ParseInt(v, font_size);
                else if (k == "bottom_margin_pct") bottom_margin_pct = std::clamp(ParseInt(v, bottom_margin_pct), 2, 45);
                else if (k == "max_width_pct") max_width_pct = std::clamp(ParseInt(v, max_width_pct), 20, 95);
                else if (k == "bg_color") bg_color = ParseRgb(v, bg_color);
                else if (k == "bg_alpha") bg_alpha = std::clamp(ParseInt(v, bg_alpha), 0, 255);
                else if (k == "bg_style") bg_style = Lower(v);
                else if (k == "bg_radius") bg_radius = std::clamp(ParseInt(v, bg_radius), 0, 24);
                else if (k == "bg_border_color") bg_border_color = ParseRgb(v, bg_border_color);
                else if (k == "bg_border_alpha") bg_border_alpha = std::clamp(ParseInt(v, bg_border_alpha), 0, 255);
                else if (k == "text_glow") text_glow = (ParseInt(v, text_glow ? 1 : 0) != 0);
                else if (k == "text_glow_alpha") text_glow_alpha = std::clamp(ParseInt(v, text_glow_alpha), 0, 255);
                else if (k == "text_cursor") text_cursor = (ParseInt(v, text_cursor ? 1 : 0) != 0);
                else if (k == "max_lines") max_lines = std::clamp(ParseInt(v, max_lines), 1, 10);
                else if (k == "max_lines_per_caption") max_lines_per_caption = std::clamp(ParseInt(v, max_lines_per_caption), 0, 10);
            } else if (section == "files") {
                if (k == "language" && !v.empty()) ApplyLanguage(v);
                else if (k == "fallback_dir" && !v.empty()) fallback_dir = v;
                else if (k == "subtitles_pattern" && !v.empty()) subtitles_pattern = v;
                else if (k == "titles_pattern" && !v.empty()) titles_pattern = v;
            } else if (section == "colors") {
                Rgb c = ParseRgb(v, default_color);
                named_colors[k] = c;
                if (k == "color_default") default_color = c;
            } else if (section == "roles") {
                // Format: prefix = r,g,b[,cat] OR prefix = color_name[,cat]
                RoleRule rule;
                rule.prefix = k;
                std::string colPart = v, catPart = "npc";
                // Count commas: if 3 commas -> r,g,b,cat; if 1 comma -> color_name,cat
                int commas = (int)std::count(v.begin(), v.end(), ',');
                if (commas == 3 || commas == 1) {
                    size_t last = v.rfind(',');
                    colPart = Trim(v.substr(0, last));
                    catPart = Lower(Trim(v.substr(last + 1)));
                }
                auto it = named_colors.find(Lower(colPart));
                rule.color = (it != named_colors.end()) ? it->second : ParseRgb(colPart, default_color);
                if (catPart == "vox") rule.cat = RoleCat::VOX;
                else if (catPart == "hev") rule.cat = RoleCat::HEV;
                else rule.cat = RoleCat::NPC;
                roles.push_back(rule);
            }
        }
        if (!has_roles_section || roles.empty()) {
            InitDefaultRoles();
        }
        return true;
    }

    std::pair<Rgb, RoleCat> Classify(const std::string& normKey, const std::string& text = {}) const {
        for (const auto& r : roles) {
            if (normKey.rfind(r.prefix, 0) == 0) {
                return {r.color, r.cat};
            }
        }
        auto getCol = [&](const char* k, Rgb def) {
            auto it = named_colors.find(k);
            return it != named_colors.end() ? it->second : def;
        };
        if (text.rfind("[VOX]", 0) == 0 || text.rfind("[\xD0\xA1\xD0\xB8\xD1\x81\xD1\x82\xD0\xB5\xD0\xBC\xD0\xB0 \xD0\xBE\xD0\xBF\xD0\xBE\xD0\xB2\xD0\xB5\xD1\x89\xD0\xB5\xD0\xBD\xD0\xB8\xD1\x8F]", 0) == 0) {
            return {getCol("color_vox", {155,215,255}), RoleCat::VOX};
        }
        if (text.rfind("[HEV]", 0) == 0) {
            return {getCol("color_hev", {255,195,65}), RoleCat::HEV};
        }
        return {default_color, RoleCat::NPC};
    }

    bool IsCategoryEnabled(RoleCat cat) const {
        if (cat == RoleCat::VOX) return enable_vox;
        if (cat == RoleCat::HEV) return enable_hev;
        return enable_npc;
    }
};

} // namespace subs
