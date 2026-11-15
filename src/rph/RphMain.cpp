// Chat vs Streamer for GTA V, built for RAGE Plugin Hook. RPH only loads .NET plugins, so a
// thin C# plugin (src/rph/plugin) loads this DLL, calls cvs_rph_tick once per frame on its
// game fiber, and forwards native calls to the game. Everything else is the same code the
// Script Hook V build uses.

#include "../Log.h"
#include "../Mod.h"
#include "RphApi.h"

#include <Windows.h>

namespace
{
    cvs::Mod* g_mod = nullptr;

    // Game fiber only.
    bool g_attached = false;
    bool g_scriptStartPending = false;
    bool g_wasLoading = false;

    DWORD WINAPI Bootstrap(LPVOID)
    {
        // On a plain thread, because this reads files and opens sockets, and the game
        // fiber must never wait on either.
        cvs::log::Guard("Starting up", [] { g_mod->Start(); });
        return 0;
    }

    bool KeyIsDown(int virtualKey)
    {
        if (cvs::nat::g_host.isKeyDown) return cvs::nat::g_host.isKeyDown(virtualKey) != 0;

        // Script Hook V's keyboard callback only fires while the game has focus, so check
        // for that here. Otherwise F7 would fire effects while the streamer types elsewhere.
        DWORD owner = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &owner);
        return owner == GetCurrentProcessId() && (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
    }
}

extern "C"
{
    // Called each time the plugin loads. RPH can reload a plugin into a new app domain while
    // this DLL stays loaded, so only the first call starts anything. Returns 0 on success.
    __declspec(dllexport) int32_t cvs_rph_start(int32_t apiVersion, const CvsRphHost* host, const char* gameVersion)
    {
        if (apiVersion != kCvsRphApiVersion || !host) return 1;

        int32_t result = 2;
        cvs::log::Guard("Attaching to RAGE Plugin Hook", [&]
        {
            cvs::nat::g_host = *host;
            g_scriptStartPending = true;
            g_wasLoading = false;
            g_attached = true;

            if (!g_mod)
            {
                // Background threads outlive any FreeLibrary, so the code has to stay mapped.
                HMODULE pinned = nullptr;
                GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                   reinterpret_cast<LPCWSTR>(&Bootstrap), &pinned);

                g_mod = new cvs::Mod();
                g_mod->hostGameVersion = gameVersion ? gameVersion : "unknown";
                if (HANDLE t = CreateThread(nullptr, 0, Bootstrap, nullptr, 0, nullptr)) CloseHandle(t);
            }
            result = 0;
        });
        return result;
    }

    __declspec(dllexport) void cvs_rph_tick()
    {
        if (!g_attached) return;

        cvs::log::Guard("A script tick", []
        {
            // Script Hook V restarts the script when the game loads a save, which is what
            // tells the engine its handles are stale. RPH's fibers just keep running, so
            // the end of a loading screen has to stand in for that restart.
            const bool loading = cvs::nat::GET_IS_LOADING_SCREEN_ACTIVE() != 0;
            if (g_wasLoading && !loading) g_scriptStartPending = true;
            g_wasLoading = loading;

            if (g_scriptStartPending)
            {
                g_scriptStartPending = false;
                g_mod->OnScriptStart();
            }
            g_mod->PollHotkeys(KeyIsDown);
            g_mod->ScriptTick();
        });
    }

    // Called when the plugin unloads. The host's callbacks are about to stop being valid, so
    // nothing may call the game after this. Chat and the web server keep running, because they
    // never touch the game, and they carry on from here if the plugin loads again.
    __declspec(dllexport) void cvs_rph_stop()
    {
        g_attached = false;
    }
}
