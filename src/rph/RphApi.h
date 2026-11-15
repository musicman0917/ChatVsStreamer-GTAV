#pragma once

// The boundary between the C++ core and the C# plugin that hosts it under RAGE Plugin Hook.
// src/rph/plugin/ChatVsStreamerPlugin.cs and tests/TestHost.cpp both have to match this.
//
// Every native argument travels as one raw 64-bit value, the same value Script Hook V's
// nativePush64 takes: integers and handles as they are, floats in the low 32 bits, and
// pointers as addresses. tools/rph_probe checks that this works on the real game.

#include <cstdint>

inline constexpr int32_t kCvsRphApiVersion = 2;

extern "C"
{
    struct CvsRphHost
    {
        // For natives that return nothing or a 32-bit value (BOOL, int, a handle, a hash).
        int32_t (*callInt)(uint64_t hash, const uint64_t* args, int32_t count);
        float (*callFloat)(uint64_t hash, const uint64_t* args, int32_t count);
        void (*callVector)(uint64_t hash, const uint64_t* args, int32_t count, float* xyz);
        int32_t (*getAllVehicles)(int32_t* handles, int32_t capacity);
        int32_t (*getAllPeds)(int32_t* handles, int32_t capacity);
        // Optional. Null means read the real keyboard. The tests set it.
        int32_t (*isKeyDown)(int32_t virtualKey);
    };
}
