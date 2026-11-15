#pragma once

// Typed access to game natives. Every native must be called from the script
// fiber (Mod::ScriptTick and what it calls), never from a network or worker thread.

#ifdef CVS_BACKEND_RPH
#include "../rph/RphApi.h"
#include <Windows.h>
#else
#include "ScriptHookV.h"
#endif

#include <cstdint>
#include <cstring>
#include <type_traits>

namespace cvs
{
    using Entity = int;
    using Ped = int;
    using Vehicle = int;
    using Player = int;
    using FireId = int;
    using Blip = int;
    using Hash = uint32_t;

    // The game lays vectors out as three floats on 8-byte boundaries. That padding
    // is the point, so MSVC's "structure was padded" warning doesn't apply here.
#pragma warning(push)
#pragma warning(disable : 4324)
    struct Vector3
    {
        alignas(8) float x = 0;
        alignas(8) float y = 0;
        alignas(8) float z = 0;
    };
#pragma warning(pop)
    static_assert(sizeof(Vector3) == 24);
}

namespace cvs::nat
{
    template <class T>
    inline uint64_t RawArg(T value)
    {
        static_assert(sizeof(T) <= sizeof(uint64_t));
        uint64_t raw = 0; // floats travel in the low 32 bits
        std::memcpy(&raw, &value, sizeof(T));
        return raw;
    }

#ifdef CVS_BACKEND_RPH
    // Set by cvs_rph_start, and only used on the game fiber.
    inline CvsRphHost g_host{};

    // RPH has no init/push/call sequence, so the whole call goes to the C# plugin at once.
    // RPH has to be told the return type, so each kind gets its own callback.
    template <class R, class... A>
    inline R Invoke(uint64_t hash, A... args)
    {
        // The extra slot keeps the array valid for a native with no arguments.
        const uint64_t raw[sizeof...(A) + 1] = { RawArg(args)..., 0 };
        const int32_t count = (int32_t)sizeof...(A);

        if constexpr (std::is_void_v<R>) { g_host.callInt(hash, raw, count); }
        else if constexpr (std::is_same_v<R, float>) { return g_host.callFloat(hash, raw, count); }
        else if constexpr (std::is_same_v<R, Vector3>)
        {
            float xyz[3]{};
            g_host.callVector(hash, raw, count, xyz);
            Vector3 v;
            v.x = xyz[0];
            v.y = xyz[1];
            v.z = xyz[2];
            return v;
        }
        else
        {
            static_assert(sizeof(R) == sizeof(int32_t), "This return type needs its own callback in CvsRphHost.");
            const int32_t value = g_host.callInt(hash, raw, count);
            R out;
            std::memcpy(&out, &value, sizeof(R));
            return out;
        }
    }
#else
    template <class R, class... A>
    inline R Invoke(uint64_t hash, A... args)
    {
        nativeInit(hash);
        (nativePush64(RawArg(args)), ...);
        PUINT64 result = nativeCall();
        if constexpr (std::is_void_v<R>) { (void)result; }
        else { return *reinterpret_cast<R*>(result); }
    }
#endif
}

#ifdef CVS_BACKEND_RPH
inline int worldGetAllVehicles(int* arr, int arrSize) { return cvs::nat::g_host.getAllVehicles(arr, arrSize); }
inline int worldGetAllPeds(int* arr, int arrSize) { return cvs::nat::g_host.getAllPeds(arr, arrSize); }
#endif

#include "../generated/Natives.g.h"
