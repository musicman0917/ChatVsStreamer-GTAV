#pragma once

#include "Effects.h"

#include <initializer_list>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace cvs
{
    // Why new effects can't fire right now. The order in Update() matters: the most
    // important reason wins.
    enum class Block { None, Online, Loading, Switching, Cutscene, Dead, Arrested, Paused, NoControl };

    const char* BlockReason(Block b);

    // An effect that lasts. `finish` always runs exactly once, so anything a timed
    // effect changes (gravity, time scale, invincibility) is always put back.
    struct TimedEffect
    {
        using TickFn = void (*)(TimedEffect&, double dt);
        using FinishFn = void (*)(TimedEffect&, bool cancelled);

        std::string id, name;
        double duration = 0, remaining = 0;
        TickFn tick = nullptr;
        FinishFn finish = nullptr;
        bool hidden = false;   // left out of the overlay's active list

        EffectEngine* engine = nullptr;
        Player player = 0;     // refreshed every frame
        Ped ped = 0;           // refreshed every frame
        Ped startPed = 0;      // who it was applied to (a character switch changes `ped`)
        Vehicle vehicle = 0;   // the vehicle at the start, if any
        Vector3 pos;           // refreshed every frame

        // Scratch space for the effect.
        double a = 0, b = 0;
        int phase = 0;
    };

    struct ActiveEffect
    {
        std::string id, name;
        double remaining = 0, duration = 0;
    };

    enum class ModelState { Ready, Loading, Missing };

    class EffectEngine
    {
    public:
        struct Status
        {
            Block block = Block::Loading;
            // Where the player is, so a random pick can skip what couldn't run right now.
            bool inVehicle = false, onMission = false;
            bool CanAct() const { return block == Block::None; }
        };

        // Script Hook V restarts the script fiber whenever the game reloads, which
        // invalidates every handle this class holds.
        void OnScriptStart();

        // Call once per frame. Reads the game state and advances timed effects.
        Status Update(double now);

        // Runs an effect now, or says why it can't. The caller has already checked
        // that Update() returned CanAct() this frame.
        EffectResult Run(const EffectDef& def, const std::string& viewer, int count, const EffectOptions& options);

        // Thread-safe, for the overlay.
        std::vector<ActiveEffect> Snapshot() const;

        // ── Used by effect implementations ──────────────────────────────────────
        void StartTimed(TimedEffect effect);
        bool IsTimedActive(const std::string& id) const;

        ModelState RequestModels(std::initializer_list<Hash> models);
        // Counts what Track and MarkEnemy are both still watching.
        bool HasSpawnRoom(int count) const;
        // Marks an entity to be handed back to the game after `seconds`. Everything
        // is released eventually, so a long stream can't exhaust the entity pools.
        void Track(Entity entity, double seconds);
        // For an attacker already handed back to the game: a red blip that comes off when it dies
        // or the game clears it away. While one with a melee boost is alive, every AI punch and
        // swing does that many times the damage. One with a retreat time runs off after fighting
        // that long, counting only unpaused play.
        void MarkEnemy(Ped ped, float meleeBoost, double retreatAfter);

        // For what a finish can't do yet, such as giving the streamer their own body back while
        // they're dead or in the air. Runs on frames when nothing blocks effects, until it returns
        // true. A reload doesn't drop it, so it has to check its handles still mean what they did.
        using SafeFn = bool (*)(EffectEngine&);
        void WhenSafe(SafeFn fn);
        // After the streamer changes bodies, so attackers on foot go after the one they're in now.
        void RetargetAttackers(Ped target);

        // `relentless` is for swarms: professional, never taking cover, always closing in.
        void ConfigureAttacker(Ped attacker, Ped target, Hash weapon, int accuracy, bool relentless = false);
        Hash HostileGroup();
        Vector3 PedSpawnPoint(const Vector3& center, float minRadius, float maxRadius);
        // The same, in a given direction (radians, anticlockwise from east), so a group can surround someone.
        Vector3 PedSpawnPoint(const Vector3& center, double angle, float minRadius, float maxRadius);
        Vector3 VehicleSpawnPoint(Ped ped, float distanceBehind, float& outHeading);

        double Random(double lo, double hi);

    private:
        struct Spawned
        {
            Entity entity = 0;
            double releaseAt = 0;
        };

        struct Marked
        {
            Ped ped = 0;
            Blip blip = 0;
            float meleeBoost = 0;
            double retreatAt = 0; // on m_playClock; 0 means never
        };

        void CancelAll();
        void Finish(TimedEffect& effect, bool cancelled);
        void UpdateSpawned(double now, const Vector3& playerPos);
        void UpdateMarked(Ped player);
        void SetMeleeBoost(float boost);
        void PublishSnapshot();

        std::vector<TimedEffect> m_timed;
        std::vector<Spawned> m_spawned;
        std::vector<Marked> m_marked;
        std::vector<SafeFn> m_whenSafe;
        Hash m_hostileGroup = 0;
        float m_meleeBoost = 0;     // what the mod set AI melee damage to; 0 means the game's own
        double m_playClock = 0;     // seconds of unpaused play, which retreat times count
        double m_lastUpdate = 0;
        double m_lastCleanup = 0;
        double m_lastMarkCheck = 0;
        double m_lastPublish = 0;
        // Seeded from the clock, not std::random_device: this object is built inside
        // DllMain, and random_device can load a DLL, which isn't safe under the loader lock.
        std::mt19937 m_rng{ (uint32_t)GetTickCount64() };

        mutable std::mutex m_snapshotMutex;
        std::vector<ActiveEffect> m_snapshot;
    };
}
