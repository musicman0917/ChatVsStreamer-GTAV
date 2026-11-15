#pragma once

#include "shv/Natives.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cvs
{
    class EffectEngine;
    struct EffectDef;

    enum class Tier { Nuisance, Disruptive, Painful, Devastating, Blessing };

    enum EffectFlag : uint32_t
    {
        NoFlags = 0,
        NeedsVehicle = 1,    // waits until the player is in a vehicle
        NeedsOnFoot = 2,     // waits until the player is out of one
        RiskyInMission = 4,  // can break a mission script, so it waits for the mission to end
        Silent = 8,          // no feed post, no overlay toast (jump scares only work unannounced)
        DirectCommand = 16,  // chat can type !id as well as !buy id
        NeedsHuman = 32,     // waits while the streamer is an animal (a morph or peyote), which can't hold a weapon or parachute
    };

    // The settings effects need when they run. Settings are read on the script fiber's caller.
    struct EffectOptions
    {
        bool blockRisky = true;
        bool ejectParachute = true;
        bool abductParachute = true;
        int fedsClearSeconds = 0; // 0 leaves the wanted level alone
    };

    struct EffectResult
    {
        enum class Kind { Ok, Retry, Fail };
        Kind kind = Kind::Ok;
        std::string reason;      // shown to the viewer when they get refunded
        double retryAfter = 1.0; // seconds, for Retry
    };

    // What an effect sees when it runs. Effects only ever run on the script fiber.
    struct EffectContext
    {
        EffectEngine& engine;
        const EffectDef& def;
        Player player;
        Ped ped;
        Vehicle vehicle;   // 0 when on foot
        Vector3 pos;
        float heading;
        const std::string& viewer;
        int count;         // size override (a raid's mob); 0 means the effect's default
        const EffectOptions& options;
    };

    using EffectFn = EffectResult (*)(EffectContext&);

    struct EffectDef
    {
        const char* id;          // what chat types after !buy
        const char* name;        // display name for chat, overlay and feed
        int defaultPrice;
        Tier tier;
        const char* description; // !info text
        uint32_t flags;
        EffectFn run;
        const char* alias = nullptr; // a second direct command, such as !whoami for !morph
    };

    // The single source of truth for the shop. Chat, the overlay, the control panel
    // and settings.ini are all driven from this table.
    const std::vector<EffectDef>& AllEffects();
    const EffectDef* FindEffect(const std::string& id);
    // What a bare !word buys: an effect with a direct command, by its id or its alias.
    const EffectDef* FindDirectCommand(const std::string& word);
    const char* TierName(Tier t);
}
