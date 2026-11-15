// Chat vs Streamer for GTA V. Script Hook V's ASI loader loads this plugin at game
// startup. It never hooks or patches game memory: everything goes through Script
// Hook V's native-call interface, on the script fiber Script Hook V gives us.

#include "Log.h"
#include "Mod.h"

#include <Windows.h>

namespace
{
    cvs::Mod* g_mod = nullptr;

    DWORD WINAPI Bootstrap(LPVOID)
    {
        // Start early, on a plain thread: networking is off-limits under the loader
        // lock, and this way the overlay and chat points work during the long load.
        cvs::log::Guard("Starting up", [] { g_mod->Start(); });
        return 0;
    }

    void ScriptMain()
    {
        // Script Hook V runs this again from the top each time the game reloads scripts.
        cvs::log::Guard("Script start", [] { g_mod->OnScriptStart(); });
        while (true)
        {
            cvs::log::Guard("A script tick", [] { g_mod->ScriptTick(); });
            scriptWait(0);
        }
    }

    void OnKeyboard(DWORD key, WORD, BYTE, BOOL, BOOL, BOOL wasDownBefore, BOOL isUpNow)
    {
        g_mod->OnKey(key, wasDownBefore != FALSE, isUpNow != FALSE);
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);

        // Background threads outlive any FreeLibrary, so the code has to stay mapped.
        HMODULE pinned = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(&Bootstrap), &pinned);

        g_mod = new cvs::Mod();
        scriptRegister(module, ScriptMain);
        keyboardHandlerRegister(OnKeyboard);
        if (HANDLE t = CreateThread(nullptr, 0, Bootstrap, nullptr, 0, nullptr)) CloseHandle(t);
    }
    // Pinned, so detach only happens at process exit, when the worker threads are
    // already gone. The ledger was saved within the last 30 seconds.
    return TRUE;
}
