#pragma once

#include <format>
#include <string>

namespace cvs::log
{
    void Init();
    void Write(const char* level, const std::string& msg);

    template <class... Args> void Info(std::format_string<Args...> f, Args&&... a) { Write("INFO", std::format(f, std::forward<Args>(a)...)); }
    template <class... Args> void Warn(std::format_string<Args...> f, Args&&... a) { Write("WARN", std::format(f, std::forward<Args>(a)...)); }
    template <class... Args> void Error(std::format_string<Args...> f, Args&&... a) { Write("ERROR", std::format(f, std::forward<Args>(a)...)); }

    // An exception that escapes a thread ends the whole process, and here that process
    // is the streamer's game. Every thread's work runs inside this.
    template <class F>
    void Guard(const char* where, F&& work)
    {
        try { work(); }
        catch (const std::exception& e) { Error("{} failed: {}", where, e.what()); }
        catch (...) { Error("{} failed with an unknown error", where); }
    }
}
