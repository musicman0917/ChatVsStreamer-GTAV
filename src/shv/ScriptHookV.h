#pragma once

// The part of Alexander Blade's Script Hook V API this mod uses. The signatures
// match the SDK's main.h, so they compile to the same decorated names the real
// ScriptHookV.dll exports. Script Hook V itself is a separate download for players
// (dev-c.com/gtav/scripthookv) and isn't bundled with this mod.

#include <Windows.h>

#ifndef CVS_SHV_API
#define CVS_SHV_API __declspec(dllimport)
#endif

enum eGameVersion : int { VER_UNK = -1 };

// key, repeats, scanCode, isExtended, isWithAlt, wasDownBefore, isUpNow
typedef void (*KeyboardHandler)(DWORD, WORD, BYTE, BOOL, BOOL, BOOL, BOOL);

CVS_SHV_API void scriptRegister(HMODULE module, void (*scriptMain)());
CVS_SHV_API void scriptUnregister(HMODULE module);
CVS_SHV_API void scriptWait(DWORD time);

CVS_SHV_API void keyboardHandlerRegister(KeyboardHandler handler);
CVS_SHV_API void keyboardHandlerUnregister(KeyboardHandler handler);

CVS_SHV_API void nativeInit(UINT64 hash);
CVS_SHV_API void nativePush64(UINT64 value);
CVS_SHV_API PUINT64 nativeCall();

CVS_SHV_API int worldGetAllVehicles(int* arr, int arrSize);
CVS_SHV_API int worldGetAllPeds(int* arr, int arrSize);

CVS_SHV_API eGameVersion getGameVersion();
