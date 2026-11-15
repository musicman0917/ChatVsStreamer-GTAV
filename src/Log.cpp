#include "Log.h"
#include "Util.h"

#include <fstream>
#include <mutex>

namespace cvs
{
    std::filesystem::path DataDir()
    {
        static std::filesystem::path dir = []
        {
            wchar_t buf[MAX_PATH]{};
            std::filesystem::path p;
            if (GetEnvironmentVariableW(L"CVS_DATA_DIR", buf, MAX_PATH)) p = buf; // tests use a scratch folder
            else if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH)) p = std::filesystem::path(buf) / L"ChatVsStreamerGTAV";
            else p = L"ChatVsStreamerGTAV";
            std::error_code ec;
            std::filesystem::create_directories(p, ec);
            return p;
        }();
        return dir;
    }
}

namespace cvs::log
{
    static std::mutex g_mutex;
    static std::ofstream g_file;

    void Init()
    {
        std::lock_guard lock(g_mutex);
        g_file.open(DataDir() / L"ChatVsStreamer.log", std::ios::out | std::ios::trunc);
    }

    void Write(const char* level, const std::string& msg)
    {
        std::lock_guard lock(g_mutex);
        if (!g_file.is_open()) return;
        SYSTEMTIME t;
        GetLocalTime(&t);
        g_file << std::format("[{:02}:{:02}:{:02}] [{}] {}\n", t.wHour, t.wMinute, t.wSecond, level, msg);
        g_file.flush();
    }
}
