#include "EffectEngine.h"
#include "Log.h"
#include "Util.h"

#include <algorithm>
#include <array>

namespace cvs
{
    using namespace nat;

    namespace
    {
        constexpr int kMaxSpawned = 40;
        constexpr float kReleaseDistance = 400.0f;
        constexpr double kMaxFrameStep = 0.25;   // a long gap (alt-tab) must not eat a timed effect
        constexpr double kCleanupInterval = 1.0;
        constexpr double kMarkInterval = 0.25;   // how long a dead attacker can keep its blip
        constexpr double kPublishInterval = 0.25;
        constexpr Hash kPlayerGroup = Joaat("PLAYER");
        constexpr int kPedTypeAnimal = 28;

        constexpr int kRelationshipHate = 5;
        // eCombatAttribute
        constexpr int kCombatCanUseCover = 0;
        constexpr int kCombatAlwaysFight = 5;
        constexpr int kCombatAggressive = 13;
        constexpr int kCombatChaseOnFoot = 21;
        constexpr int kCombatFightArmedWhenUnarmed = 46;
        constexpr int kCombatCanCharge = 50;
        constexpr int kCombatNeverFlee = 58;
        constexpr int kCombatFleesFromInvincible = 63;
        constexpr int kCombatAbilityAverage = 1, kCombatAbilityProfessional = 2;
        constexpr int kCombatMovementWillAdvance = 2;
        constexpr int kCombatRangeNear = 0;

        constexpr int kBlipRed = 1;
        // Where blips overlap, the higher priority is drawn on top. The scale isn't documented,
        // so this is a guess at "high" that should still leave the player's own arrow on top.
        constexpr int kBlipPriorityHigh = 9;
        constexpr float kRetreatDistance = 500.0f;

        float DistanceSq(const Vector3& a, const Vector3& b)
        {
            float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
            return dx * dx + dy * dy + dz * dz;
        }
    }

    const char* BlockReason(Block b)
    {
        switch (b)
        {
        case Block::None: return "ready";
        case Block::Online: return "effects only run in story mode";
        case Block::Loading: return "the game is loading";
        case Block::Switching: return "the streamer is switching characters";
        case Block::Cutscene: return "a cutscene is playing";
        case Block::Dead: return "the streamer is wasted";
        case Block::Arrested: return "the streamer is busted";
        case Block::Paused: return "the game is paused";
        case Block::NoControl: return "the streamer doesn't have control right now";
        }
        return "the game is busy";
    }

    void EffectEngine::OnScriptStart()
    {
        // Put back anything a previous session changed (the game doesn't reset gravity
        // or time scale on its own), then forget handles that no longer mean anything.
        CancelAll();
        m_spawned.clear();
        // After a save loads, the old handles could belong to something else, so a blip only
        // comes off if it's still on the same attacker. That's the case when RPH reloads the plugin.
        for (Marked& m : m_marked)
            if (DOES_ENTITY_EXIST(m.ped) && DOES_BLIP_EXIST(m.blip) && GET_BLIP_FROM_ENTITY(m.ped) == m.blip) REMOVE_BLIP(&m.blip);
        m_marked.clear();
        SetMeleeBoost(0.0f);
        m_hostileGroup = 0;
        m_lastUpdate = 0;
        // m_whenSafe is kept: when RPH reloads the plugin, the streamer still needs their own body back.
    }

    EffectEngine::Status EffectEngine::Update(double now)
    {
        double dt = m_lastUpdate > 0 ? std::clamp(now - m_lastUpdate, 0.0, kMaxFrameStep) : 0.0;
        m_lastUpdate = now;

        Status status;
        const Player player = PLAYER_ID();
        const Ped ped = PLAYER_PED_ID();
        status.inVehicle = IS_PED_IN_ANY_VEHICLE(ped, false) != 0;
        status.onMission = GET_MISSION_FLAG() != 0;

        if (NETWORK_IS_SESSION_STARTED() || NETWORK_IS_GAME_IN_PROGRESS()) status.block = Block::Online;
        else if (GET_IS_LOADING_SCREEN_ACTIVE() || IS_SCREEN_FADED_OUT()) status.block = Block::Loading;
        else if (IS_PLAYER_SWITCH_IN_PROGRESS()) status.block = Block::Switching;
        else if (IS_CUTSCENE_PLAYING()) status.block = Block::Cutscene;
        else if (!IS_PLAYER_PLAYING(player) || IS_ENTITY_DEAD(ped, false)) status.block = Block::Dead;
        else if (IS_PLAYER_BEING_ARRESTED(player, true)) status.block = Block::Arrested;
        else if (IS_PAUSE_MENU_ACTIVE()) status.block = Block::Paused;
        else if (!IS_PLAYER_CONTROL_ON(player)) status.block = Block::NoControl;
        else status.block = Block::None;

        switch (status.block)
        {
        case Block::None:
        case Block::NoControl:
        {
            m_playClock += dt;
            if (!m_timed.empty())
            {
                const Vector3 pos = GET_ENTITY_COORDS(ped, true);
                bool anyFinished = false;
                for (size_t i = 0; i < m_timed.size();)
                {
                    TimedEffect& t = m_timed[i];
                    t.player = player;
                    t.ped = ped;
                    t.pos = pos;
                    t.remaining -= dt;
                    if (t.tick) t.tick(t, dt);
                    if (t.remaining <= 0)
                    {
                        Finish(t, false);
                        m_timed.erase(m_timed.begin() + (std::ptrdiff_t)i);
                        anyFinished = true;
                    }
                    else ++i;
                }
                if (anyFinished) PublishSnapshot();
            }
            break;
        }
        case Block::Paused:
            break; // timers freeze while the pause menu is open
        default:
            CancelAll(); // loading, dead, a cutscene: nothing timed should survive these
            break;
        }

        if (status.block == Block::None && !m_whenSafe.empty())
        {
            std::vector<SafeFn> pending;
            pending.swap(m_whenSafe);
            for (SafeFn fn : pending)
                if (!fn(*this)) WhenSafe(fn);
        }

        if (status.block != Block::Online && status.block != Block::Loading && now - m_lastCleanup >= kCleanupInterval)
        {
            m_lastCleanup = now;
            UpdateSpawned(now, GET_ENTITY_COORDS(ped, true));
        }
        if (status.block != Block::Online && status.block != Block::Loading && now - m_lastMarkCheck >= kMarkInterval)
        {
            m_lastMarkCheck = now;
            UpdateMarked(ped);
        }

        if (now - m_lastPublish >= kPublishInterval)
        {
            m_lastPublish = now;
            PublishSnapshot();
        }
        return status;
    }

    EffectResult EffectEngine::Run(const EffectDef& def, const std::string& viewer, int count, const EffectOptions& options)
    {
        const Player player = PLAYER_ID();
        const Ped ped = PLAYER_PED_ID();
        const Vehicle vehicle = IS_PED_IN_ANY_VEHICLE(ped, false) ? GET_VEHICLE_PED_IS_IN(ped, false) : 0;

        if ((def.flags & NeedsVehicle) && !vehicle)
            return { EffectResult::Kind::Retry, "the streamer stayed on foot" };
        if ((def.flags & NeedsOnFoot) && vehicle)
            return { EffectResult::Kind::Retry, "the streamer stayed in a vehicle" };
        if (options.blockRisky && (def.flags & RiskyInMission) && GET_MISSION_FLAG())
            return { EffectResult::Kind::Retry, "the streamer is on a mission" };
        if ((def.flags & NeedsHuman) && GET_PED_TYPE(ped) == kPedTypeAnimal)
            return { EffectResult::Kind::Retry, "the streamer is an animal right now" };
        if (IsTimedActive(def.id))
            return { EffectResult::Kind::Retry, "it's already running" };

        EffectContext ctx{ *this, def, player, ped, vehicle, GET_ENTITY_COORDS(ped, true), GET_ENTITY_HEADING(ped), viewer, count, options };
        return def.run(ctx);
    }

    std::vector<ActiveEffect> EffectEngine::Snapshot() const
    {
        std::lock_guard lock(m_snapshotMutex);
        return m_snapshot;
    }

    void EffectEngine::PublishSnapshot()
    {
        std::vector<ActiveEffect> active;
        for (auto& t : m_timed)
            if (!t.hidden) active.push_back({ t.id, t.name, std::max(t.remaining, 0.0), t.duration });
        std::lock_guard lock(m_snapshotMutex);
        m_snapshot = std::move(active);
    }

    void EffectEngine::StartTimed(TimedEffect effect)
    {
        effect.engine = this;
        effect.remaining = effect.duration;
        effect.player = PLAYER_ID();
        effect.ped = PLAYER_PED_ID();
        effect.startPed = effect.ped;
        effect.pos = GET_ENTITY_COORDS(effect.ped, true);
        if (IS_PED_IN_ANY_VEHICLE(effect.ped, false)) effect.vehicle = GET_VEHICLE_PED_IS_IN(effect.ped, false);
        m_timed.push_back(std::move(effect));
        PublishSnapshot();
    }

    bool EffectEngine::IsTimedActive(const std::string& id) const
    {
        for (auto& t : m_timed)
            if (t.id == id) return true;
        return false;
    }

    void EffectEngine::Finish(TimedEffect& effect, bool cancelled)
    {
        if (effect.finish) effect.finish(effect, cancelled);
    }

    void EffectEngine::CancelAll()
    {
        if (m_timed.empty()) return;
        // Finish() may start nothing new, so iterating directly is safe.
        for (auto& t : m_timed) Finish(t, true);
        m_timed.clear();
        PublishSnapshot();
    }

    ModelState EffectEngine::RequestModels(std::initializer_list<Hash> models)
    {
        bool allLoaded = true;
        for (Hash m : models)
        {
            if (!IS_MODEL_IN_CDIMAGE(m) || !IS_MODEL_VALID(m)) return ModelState::Missing;
            if (!HAS_MODEL_LOADED(m))
            {
                REQUEST_MODEL(m);
                allLoaded = false;
            }
        }
        return allLoaded ? ModelState::Ready : ModelState::Loading;
    }

    bool EffectEngine::HasSpawnRoom(int count) const
    {
        return (int)(m_spawned.size() + m_marked.size()) + count <= kMaxSpawned;
    }

    void EffectEngine::Track(Entity entity, double seconds)
    {
        if (entity) m_spawned.push_back({ entity, NowSeconds() + seconds });
    }

    void EffectEngine::MarkEnemy(Ped ped, float meleeBoost, double retreatAfter)
    {
        if (!ped) return;
        const Blip blip = ADD_BLIP_FOR_ENTITY(ped);
        if (blip)
        {
            SET_BLIP_AS_FRIENDLY(blip, false);
            SET_BLIP_COLOUR(blip, kBlipRed);
            SET_BLIP_PRIORITY(blip, kBlipPriorityHigh);
            SET_BLIP_AS_SHORT_RANGE(blip, false); // still on the minimap's edge from far away
        }
        m_marked.push_back({ ped, blip, meleeBoost, retreatAfter > 0 ? m_playClock + retreatAfter : 0.0 });
        if (meleeBoost > m_meleeBoost) SetMeleeBoost(meleeBoost); // before their first swing, not at the next check
    }

    void EffectEngine::UpdateMarked(Ped player)
    {
        float boost = 0.0f;
        for (size_t i = 0; i < m_marked.size();)
        {
            Marked& m = m_marked[i];
            const bool alive = DOES_ENTITY_EXIST(m.ped) && !IS_ENTITY_DEAD(m.ped, false);
            const bool retreating = alive && m.retreatAt > 0 && m_playClock >= m.retreatAt;
            if (alive && !retreating)
            {
                boost = std::max(boost, m.meleeBoost);
                ++i;
                continue;
            }
            // Also when the game has already deleted the attacker: a blip can outlive its entity.
            if (DOES_BLIP_EXIST(m.blip)) REMOVE_BLIP(&m.blip);
            if (retreating)
            {
                CLEAR_PED_TASKS(m.ped);
                TASK_SMART_FLEE_PED(m.ped, player, kRetreatDistance, -1, false, false);
            }
            m_marked.erase(m_marked.begin() + (std::ptrdiff_t)i);
        }
        SetMeleeBoost(boost);
    }

    void EffectEngine::WhenSafe(SafeFn fn)
    {
        if (std::find(m_whenSafe.begin(), m_whenSafe.end(), fn) == m_whenSafe.end()) m_whenSafe.push_back(fn);
    }

    // Attackers in vehicles keep their old target, because a new combat task would undo a
    // helicopter's chase. They're after the streamer's own ped, which comes back in the end.
    void EffectEngine::RetargetAttackers(Ped target)
    {
        if (!m_hostileGroup) return;
        std::array<int, 256> peds{};
        const int count = worldGetAllPeds(peds.data(), (int)peds.size());
        for (int i = 0; i < count; ++i)
        {
            const Ped ped = peds[(size_t)i];
            if (ped == target || IS_PED_A_PLAYER(ped) || IS_ENTITY_DEAD(ped, false) || IS_PED_IN_ANY_VEHICLE(ped, false)) continue;
            if (GET_PED_RELATIONSHIP_GROUP_HASH(ped) == m_hostileGroup) TASK_COMBAT_PED(ped, target, 0, 16);
        }
    }

    void EffectEngine::SetMeleeBoost(float boost)
    {
        if (boost == m_meleeBoost) return;
        if (boost > 0.0f) SET_AI_MELEE_WEAPON_DAMAGE_MODIFIER(boost);
        else RESET_AI_MELEE_WEAPON_DAMAGE_MODIFIER();
        m_meleeBoost = boost;
    }

    void EffectEngine::UpdateSpawned(double now, const Vector3& playerPos)
    {
        for (size_t i = 0; i < m_spawned.size();)
        {
            Spawned& s = m_spawned[i];
            if (!DOES_ENTITY_EXIST(s.entity))
            {
                m_spawned.erase(m_spawned.begin() + (std::ptrdiff_t)i);
                continue;
            }
            bool tooFar = DistanceSq(GET_ENTITY_COORDS(s.entity, false), playerPos) > kReleaseDistance * kReleaseDistance;
            if (now >= s.releaseAt || tooFar || IS_ENTITY_DEAD(s.entity, false))
            {
                Entity handle = s.entity; // the native zeroes what it's given
                SET_ENTITY_AS_NO_LONGER_NEEDED(&handle);
                m_spawned.erase(m_spawned.begin() + (std::ptrdiff_t)i);
                continue;
            }
            ++i;
        }
    }

    Hash EffectEngine::HostileGroup()
    {
        if (!m_hostileGroup)
        {
            Hash group = 0;
            ADD_RELATIONSHIP_GROUP("CVS_HOSTILE", &group);
            SET_RELATIONSHIP_BETWEEN_GROUPS(kRelationshipHate, group, kPlayerGroup);
            SET_RELATIONSHIP_BETWEEN_GROUPS(kRelationshipHate, kPlayerGroup, group);
            m_hostileGroup = group;
        }
        return m_hostileGroup;
    }

    void EffectEngine::ConfigureAttacker(Ped attacker, Ped target, Hash weapon, int accuracy, bool relentless)
    {
        SET_PED_RELATIONSHIP_GROUP_HASH(attacker, HostileGroup());
        SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(attacker, true);
        SET_PED_COMBAT_ABILITY(attacker, relentless ? kCombatAbilityProfessional : kCombatAbilityAverage);
        SET_PED_COMBAT_ATTRIBUTES(attacker, kCombatAlwaysFight, true);
        SET_PED_COMBAT_ATTRIBUTES(attacker, kCombatAggressive, true);
        SET_PED_COMBAT_ATTRIBUTES(attacker, kCombatFightArmedWhenUnarmed, true);
        SET_PED_COMBAT_ATTRIBUTES(attacker, kCombatNeverFlee, true);
        if (relentless)
        {
            SET_PED_COMBAT_MOVEMENT(attacker, kCombatMovementWillAdvance);
            SET_PED_COMBAT_RANGE(attacker, kCombatRangeNear);
            SET_PED_COMBAT_ATTRIBUTES(attacker, kCombatCanUseCover, false);
            SET_PED_COMBAT_ATTRIBUTES(attacker, kCombatCanCharge, true);
            SET_PED_COMBAT_ATTRIBUTES(attacker, kCombatChaseOnFoot, true);
            SET_PED_COMBAT_ATTRIBUTES(attacker, kCombatFleesFromInvincible, false); // Plot Armor doesn't scare them off
        }
        SET_PED_ACCURACY(attacker, accuracy);
        if (weapon) GIVE_WEAPON_TO_PED(attacker, weapon, 999, false, true);
        TASK_COMBAT_PED(attacker, target, 0, 16);
        SET_PED_KEEP_TASK(attacker, true);
    }

    double EffectEngine::Random(double lo, double hi)
    {
        return std::uniform_real_distribution<double>(lo, hi)(m_rng);
    }

    Vector3 EffectEngine::PedSpawnPoint(const Vector3& center, float minRadius, float maxRadius)
    {
        return PedSpawnPoint(center, Random(0.0, 6.283185307), minRadius, maxRadius);
    }

    // The game may move the point to the nearest safe spot for a ped, so the direction is only a target.
    Vector3 EffectEngine::PedSpawnPoint(const Vector3& center, double angle, float minRadius, float maxRadius)
    {
        const float radius = (float)Random(minRadius, maxRadius);
        Vector3 p = center;
        p.x += (float)std::cos(angle) * radius;
        p.y += (float)std::sin(angle) * radius;

        // Flag 16 (flat ground) is the DB's recommendation; 0 is reported to crash scripts.
        Vector3 safe;
        if (GET_SAFE_COORD_FOR_PED(p.x, p.y, p.z, false, &safe, 16)) return safe;

        float groundZ = 0;
        if (GET_GROUND_Z_FOR_3D_COORD(p.x, p.y, p.z + 40.0f, &groundZ, false, false)) p.z = groundZ;
        return p;
    }

    Vector3 EffectEngine::VehicleSpawnPoint(Ped ped, float distanceBehind, float& outHeading)
    {
        outHeading = GET_ENTITY_HEADING(ped);
        Vector3 behind = GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(ped, 0.0f, -distanceBehind, 0.0f);

        Vector3 node;
        float nodeHeading = 0;
        if (GET_CLOSEST_VEHICLE_NODE_WITH_HEADING(behind.x, behind.y, behind.z, &node, &nodeHeading, 1, 3.0f, 0.0f))
        {
            outHeading = nodeHeading;
            return node;
        }

        float groundZ = 0;
        if (GET_GROUND_Z_FOR_3D_COORD(behind.x, behind.y, behind.z + 40.0f, &groundZ, false, false)) behind.z = groundZ;
        return behind;
    }
}
