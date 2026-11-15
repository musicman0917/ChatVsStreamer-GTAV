cmake_minimum_required(VERSION 3.25)
project(ChatVsStreamerGTAV VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT so players don't need a specific VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Import library for ScriptHookV.dll, generated from a .def file. This is what the
# Script Hook V SDK's .lib provides, so the SDK doesn't have to be downloaded or
# redistributed. The linker fails if a declaration in src/shv/ScriptHookV.h doesn't
# mangle to a name in the .def, so the two can't drift apart.
set(SHV_DEF "${CMAKE_CURRENT_SOURCE_DIR}/src/shv/ScriptHookV.def")
set(SHV_LIB "${CMAKE_CURRENT_BINARY_DIR}/ScriptHookV.lib")
add_custom_command(
    OUTPUT "${SHV_LIB}"
    COMMAND "${CMAKE_AR}" /nologo "/def:${SHV_DEF}" "/out:${SHV_LIB}" /machine:x64
    DEPENDS "${SHV_DEF}"
    COMMENT "Generating ScriptHookV import library")
add_custom_target(shv_implib DEPENDS "${SHV_LIB}")

# Everything except the way in to the game. Both builds below share it.
set(CVS_CORE_SOURCES
    src/Economy.cpp
    src/EffectEngine.cpp
    src/Effects.cpp
    src/Json.cpp
    src/Log.cpp
    src/Mod.cpp
    src/Settings.cpp
    src/TwitchChat.cpp
    src/TwitchEventSub.cpp
    src/WebServer.cpp
)

add_library(ChatVsStreamerGTAV SHARED src/dllmain.cpp ${CVS_CORE_SOURCES})
add_dependencies(ChatVsStreamerGTAV shv_implib)
target_compile_definitions(ChatVsStreamerGTAV PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(ChatVsStreamerGTAV PRIVATE /W4 /utf-8 /permissive- /EHsc)
target_link_libraries(ChatVsStreamerGTAV PRIVATE "${SHV_LIB}" ws2_32 winhttp)
# Script Hook V's ASI loader picks up *.asi files next to the game executable.
set_target_properties(ChatVsStreamerGTAV PROPERTIES SUFFIX ".asi")

# The same mod for RAGE Plugin Hook, for game versions Script Hook V doesn't support yet. It
# doesn't link against Script Hook V at all: the C# plugin in src/rph/plugin loads this DLL
# and forwards its native calls. build.ps1 compiles that plugin.
add_library(ChatVsStreamerCore SHARED src/rph/RphMain.cpp ${CVS_CORE_SOURCES})
target_compile_definitions(ChatVsStreamerCore PRIVATE CVS_BACKEND_RPH WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(ChatVsStreamerCore PRIVATE /W4 /utf-8 /permissive- /EHsc)
target_link_libraries(ChatVsStreamerCore PRIVATE ws2_32 winhttp user32)

# ── Tests: a simulated Script Hook V, so everything except the game itself can be
#    exercised on a machine without GTA V. See tests/TestHost.cpp.
add_library(MockScriptHookV SHARED tests/MockScriptHook.cpp)
target_compile_definitions(MockScriptHookV PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(MockScriptHookV PRIVATE /W4 /utf-8 /permissive- /EHsc)
set_target_properties(MockScriptHookV PROPERTIES OUTPUT_NAME "ScriptHookV"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/mock"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/mock")

add_executable(TestHost tests/TestHost.cpp src/Json.cpp)
target_compile_definitions(TestHost PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(TestHost PRIVATE /W4 /utf-8 /permissive- /EHsc)
target_link_libraries(TestHost PRIVATE ws2_32)
add_dependencies(TestHost ChatVsStreamerGTAV ChatVsStreamerCore MockScriptHookV)
