#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <Windows.h>

namespace cvs
{
    inline std::wstring Widen(const std::string& s)
    {
        if (s.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
        std::wstring out(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
        return out;
    }

    inline std::string Narrow(const std::wstring& s)
    {
        if (s.empty()) return {};
        int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
        std::string out(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
        return out;
    }

    inline std::string Lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    }

    inline std::string Trim(const std::string& s)
    {
        size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return {};
        size_t e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    }

    inline std::vector<std::string> Split(const std::string& s, char sep)
    {
        std::vector<std::string> out;
        size_t start = 0;
        while (true)
        {
            size_t pos = s.find(sep, start);
            out.push_back(s.substr(start, pos == std::string::npos ? std::string::npos : pos - start));
            if (pos == std::string::npos) break;
            start = pos + 1;
        }
        return out;
    }

    inline bool StartsWith(const std::string& s, const std::string& prefix)
    {
        return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
    }

    // Twitch logins are lowercase letters, digits and underscores, up to 25 characters.
    // Anything else (typed into !give, for example) never reaches the ledger.
    inline bool IsValidLogin(const std::string& s)
    {
        if (s.empty() || s.size() > 25) return false;
        return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; });
    }

    // Text for the in-game feed. `~` starts a formatting token in the game's text
    // renderer, and long strings aren't safe, so viewer names get cleaned up first.
    inline std::string FeedSafe(const std::string& s, size_t maxBytes = 40)
    {
        std::string out;
        for (unsigned char c : s)
            if (c >= 0x20 && c != '~' && c != 0x7f) out.push_back((char)c);
        if (out.size() > maxBytes)
        {
            size_t cut = maxBytes;
            while (cut > 0 && ((unsigned char)out[cut] & 0xC0) == 0x80) --cut; // don't split a UTF-8 character
            out.resize(cut);
        }
        return out;
    }

    // Number plates hold at most 8 characters.
    inline std::string PlateText(const std::string& s)
    {
        std::string out;
        for (unsigned char c : s)
        {
            if (out.size() == 8) break;
            if (std::isalnum(c)) out.push_back((char)std::toupper(c));
        }
        return out.empty() ? "CHAOS" : out;
    }

    inline std::string JsonEscape(const std::string& s)
    {
        std::string out;
        out.reserve(s.size() + 8);
        for (unsigned char c : s)
        {
            switch (c)
            {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) { char buf[8]; snprintf(buf, sizeof(buf), "\\u%04x", c); out += buf; }
                else out.push_back((char)c);
            }
        }
        return out;
    }

    inline int ToInt(const std::string& s, int fallback)
    {
        try
        {
            if (s.empty()) return fallback;
            double v = std::stod(s);
            if (!std::isfinite(v)) return fallback;
            return (int)std::clamp(v, -2000000000.0, 2000000000.0);
        }
        catch (...) { return fallback; }
    }

    // Seconds since boot. CVS_TEST_TIMESCALE speeds this up so tests can cover
    // two-minute timeouts in a few seconds. Nothing sets it outside tests/.
    inline double NowSeconds()
    {
        static const double scale = []
        {
            wchar_t buf[32]{};
            return GetEnvironmentVariableW(L"CVS_TEST_TIMESCALE", buf, 32) ? _wtof(buf) : 1.0;
        }();
        return (double)GetTickCount64() / 1000.0 * scale;
    }

    // The game's name hash (Jenkins one-at-a-time over the lowercased string).
    constexpr uint32_t Joaat(std::string_view s)
    {
        uint32_t h = 0;
        for (char ch : s)
        {
            unsigned char c = (unsigned char)ch;
            if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
            h += c;
            h += h << 10;
            h ^= h >> 6;
        }
        h += h << 3;
        h ^= h >> 11;
        h += h << 15;
        return h;
    }

    // %LOCALAPPDATA%\ChatVsStreamerGTAV. Rockstar's default install lives under
    // Program Files, which a non-elevated game can't write to, so nothing the mod
    // saves goes in the game folder. It also keeps the Twitch token away from it.
    std::filesystem::path DataDir();
}
