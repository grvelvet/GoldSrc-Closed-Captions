#pragma once
#include <windows.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <fstream>
#include <algorithm>
#include <cctype>
#include "config.hpp"

namespace subs {

struct SubtitleEntry {
    uint32_t duration_ms = 2000;
    std::string text;
};

class Database {
public:
    std::unordered_map<std::string, SubtitleEntry> subtitles;
    std::unordered_map<std::string, std::string> titles;

    static std::string NormalizeKey(std::string s) {
        for (char& c : s) {
            if (c == '\\') c = '/';
            else c = (char)std::tolower((unsigned char)c);
        }
        while (!s.empty() && (s[0] == '!' || s[0] == '*' || s[0] == '#' || s[0] == '@' || s[0] == '/')) {
            s.erase(0, 1);
        }
        if (s.rfind("sound/", 0) == 0) s.erase(0, 6);
        if (s.size() > 4) {
            std::string tail = s.substr(s.size() - 4);
            if (tail == ".wav" || tail == ".mp3") s.erase(s.size() - 4);
        }
        return s;
    }

    static std::string UnescapeNewlines(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n') {
                out.push_back('\n');
                ++i;
            } else {
                out.push_back(s[i]);
            }
        }
        return out;
    }

    size_t LoadSubtitlesTsv(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return 0;
        size_t count = 0;
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            // Strip UTF-8 BOM on first line
            if (line.size() >= 3 && (unsigned char)line[0] == 0xEF &&
                (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF) {
                line.erase(0, 3);
            }
            if (line.empty() || line[0] == '#') continue;
            size_t t1 = line.find('\t');
            if (t1 == std::string::npos) continue;
            size_t t2 = line.find('\t', t1 + 1);
            if (t2 == std::string::npos) continue;
            std::string key = NormalizeKey(line.substr(0, t1));
            uint32_t dur = (uint32_t)std::strtoul(line.substr(t1 + 1, t2 - t1 - 1).c_str(), nullptr, 10);
            if (dur == 0) dur = 2000;
            std::string txt = UnescapeNewlines(line.substr(t2 + 1));
            if (!key.empty() && !txt.empty()) {
                subtitles[key] = {dur, std::move(txt)};
                count++;
            }
        }
        return count;
    }

    static std::string UpperTrimTitleKey(const std::string& raw) {
        size_t a = 0, b = raw.size();
        while (a < b && (unsigned char)raw[a] <= ' ') a++;
        while (b > a && (unsigned char)raw[b - 1] <= ' ') b--;
        std::string out = raw.substr(a, b - a);
        for (char& c : out) c = (char)std::toupper((unsigned char)c);
        return out;
    }

    size_t LoadTitlesTsv(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return 0;
        size_t count = 0;
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.size() >= 3 && (unsigned char)line[0] == 0xEF &&
                (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF) {
                line.erase(0, 3);
            }
            if (line.empty() || line[0] == '#') continue;
            size_t t1 = line.find('\t');
            if (t1 == std::string::npos) continue;
            std::string key = UpperTrimTitleKey(line.substr(0, t1));
            std::string txt = UnescapeNewlines(line.substr(t1 + 1));
            if (!key.empty()) {
                titles[key] = std::move(txt);
                count++;
            }
        }
        return count;
    }

    static std::vector<std::string> GlobDir(const std::string& dir, const std::string& pattern) {
        std::vector<std::string> files;
        WIN32_FIND_DATAA fd{};
        std::string query = dir + "\\" + pattern;
        HANDLE h = FindFirstFileA(query.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return files;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                files.push_back(dir + "\\" + fd.cFileName);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        std::sort(files.begin(), files.end());
        return files;
    }

    static std::string ReadLiblistFallbackDir(const std::string& dir) {
        std::ifstream f(dir + "\\liblist.gam");
        if (!f.is_open()) return "";
        std::string line;
        while (std::getline(f, line)) {
            size_t i = 0;
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
            if (i + 12 >= line.size()) continue;
            if (Config::Lower(line.substr(i, 12)) != "fallback_dir") continue;
            size_t q1 = line.find('"', i + 12);
            if (q1 == std::string::npos) continue;
            size_t q2 = line.find('"', q1 + 1);
            if (q2 != std::string::npos && q2 > q1 + 1) {
                return line.substr(q1 + 1, q2 - q1 - 1);
            }
        }
        return "";
    }

    void ReloadAll(const std::string& exeDir, const std::string& gameDir, const Config& cfg) {
        subtitles.clear();
        titles.clear();

        auto loadDir = [&](const std::string& modName) {
            if (modName.empty()) return;
            std::string dir = exeDir + "\\" + modName;
            for (const auto& f : GlobDir(dir, cfg.subtitles_pattern)) LoadSubtitlesTsv(f);
            for (const auto& f : GlobDir(dir, cfg.titles_pattern)) LoadTitlesTsv(f);
        };

        std::string baseFallback = Config::Lower(cfg.fallback_dir);
        std::string gameLower = Config::Lower(gameDir);
        loadDir(cfg.fallback_dir);

        if (!gameDir.empty() && gameLower != baseFallback) {
            std::string libFallback = ReadLiblistFallbackDir(exeDir + "\\" + gameDir);
            std::string libLower = Config::Lower(libFallback);
            if (!libFallback.empty() && libLower != baseFallback && libLower != gameLower) {
                loadDir(libFallback);
            }
            loadDir(gameDir);
        }
    }

    const SubtitleEntry* FindSubtitle(const std::string& rawKey, std::string& outNorm) const {
        outNorm = NormalizeKey(rawKey);
        auto it = subtitles.find(outNorm);
        if (it != subtitles.end()) return &it->second;
        size_t sl = outNorm.rfind('/');
        if (sl != std::string::npos) {
            auto it2 = subtitles.find(outNorm.substr(sl + 1));
            if (it2 != subtitles.end()) return &it2->second;
        }
        return nullptr;
    }
};

} // namespace subs
