#include "Effects.h"
#include "EffectEngine.h"
#include "Log.h"
#include "Util.h"

#include <array>
#include <optional>

namespace cvs
{
    using namespace nat;

    namespace
    {
        using Kind = EffectResult::Kind;

        // ePedType
        constexpr int kPedCivMale = 4, kPedMission = 26, kPedAnimal = 28, kPedArmy = 29;
        // eExplosionTag
        constexpr int kExpRocket = 4, kExpBlimp = 29;
        // Vehicle seats
        constexpr int kSeatDriver = -1;
        // Controls. Group 0 is the player's own controls.
        constexpr int kControls = 0;
        constexpr int kInputSprint = 21, kInputJump = 22, kInputAttack = 24, kInputMoveLR = 30, kInputMoveUD = 31,
                      kInputMoveUpOnly = 32, kInputMoveDownOnly = 33, kInputMoveLeftOnly = 34, kInputMoveRightOnly = 35,
                      kInputVehMoveLR = 59, kInputVehLeftOnly = 63, kInputVehRightOnly = 64,
                      kInputVehAttack = 69, kInputVehAttack2 = 70,
                      kInputVehAccelerate = 71, kInputVehBrake = 72, kInputVehHandbrake = 76, kInputVehHorn = 86,
                      kInputVehPassengerAttack = 92,
                      kInputMeleeLight = 140, kInputMeleeHeavy = 141, kInputMeleeAlt = 142, kInputAttack2 = 257,
                      kInputMelee1 = 263, kInputMelee2 = 264;
        // Every way of changing weapon: the wheel, next and previous, and the number keys.
        constexpr std::array<int, 15> kInputsChangeWeapon = { 14, 15, 16, 17, 37, 157, 158, 159, 160, 161, 162, 163, 164, 165, 99 };

        // TASK_LEAVE_VEHICLE: the ped throws themself out, even from a moving vehicle.
        constexpr int kLeaveBailOut = 4160;
        // TASK_VEHICLE_MISSION's VehMissionType, and the driving style the game calls Rushed.
        constexpr int kMissionRam = 2, kDriveRushed = 1074528293;
        // eCombatAttribute: whether a ped will get out of their vehicle to fight.
        constexpr int kCombatLeaveVehicles = 3;
        // eDispatchType: every police, SWAT and army response.
        constexpr std::array<int, 9> kPoliceDispatch = { 1, 2, 4, 6, 7, 8, 12, 13, 14 };

        constexpr Hash kWeaponUnarmed = Joaat("weapon_unarmed");
        constexpr Hash kWeaponPistol = Joaat("weapon_pistol");
        constexpr Hash kWeaponSmg = Joaat("weapon_smg");
        constexpr Hash kWeaponCarbine = Joaat("weapon_carbinerifle");
        constexpr Hash kParachute = Joaat("gadget_parachute");
        constexpr Hash kPlayerGroup = Joaat("PLAYER");

        constexpr double kAttackerLifetime = 180.0;
        constexpr double kGiftLifetime = 5.0; // handed straight back to the game; the car just persists normally

        EffectResult Ok() { return { Kind::Ok, "" }; }
        EffectResult Retry(const char* reason, double after = 1.0) { return { Kind::Retry, reason, after }; }
        EffectResult Fail(const char* reason) { return { Kind::Fail, reason }; }

        // Models stream in over several frames. Rather than block the script fiber
        // (which would also pause every running effect), a purchase just retries
        // shortly until everything is loaded.
        std::optional<EffectResult> NeedModels(EffectContext& c, std::initializer_list<Hash> models)
        {
            switch (c.engine.RequestModels(models))
            {
            case ModelState::Ready: return std::nullopt;
            case ModelState::Loading: return Retry("the game couldn't load it in time", 0.2);
            default: return Fail("this game version doesn't have that model");
            }
        }

        EffectResult StartTimed(EffectContext& c, double seconds, TimedEffect::TickFn tick, TimedEffect::FinishFn finish, double a = 0, double b = 0)
        {
            TimedEffect t;
            t.id = c.def.id;
            t.name = c.def.name;
            t.duration = seconds;
            t.tick = tick;
            t.finish = finish;
            t.hidden = (c.def.flags & Silent) != 0;
            t.a = a;
            t.b = b;
            c.engine.StartTimed(std::move(t));
            return Ok();
        }

        bool VehicleStillThere(const TimedEffect& t) { return t.vehicle && DOES_ENTITY_EXIST(t.vehicle); }

        // Whether the streamer is still in the vehicle the effect started on.
        bool StillInVehicle(const TimedEffect& t)
        {
            return VehicleStillThere(t) && IS_PED_IN_ANY_VEHICLE(t.ped, false) && GET_VEHICLE_PED_IS_IN(t.ped, false) == t.vehicle;
        }

        float LengthOf(const Vector3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

        bool Within(const Vector3& a, const Vector3& b, float radius)
        {
            const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
            return dx * dx + dy * dy + dz * dz <= radius * radius;
        }

        Vector3 RingPoint(const Vector3& center, double angle, float radius)
        {
            Vector3 p = center;
            p.x += (float)std::cos(angle) * radius;
            p.y += (float)std::sin(angle) * radius;
            return p;
        }

        // ── Spawning ────────────────────────────────────────────────────────────

        EffectResult SpawnGang(EffectContext& c, Hash model, Hash weapon, int count, int pedType, int accuracy,
                               float minRadius = 14.0f, float maxRadius = 24.0f)
        {
            if (auto wait = NeedModels(c, { model })) return *wait;
            if (!c.engine.HasSpawnRoom(count)) return Retry("too many attackers are already out");

            int spawned = 0;
            for (int i = 0; i < count; ++i)
            {
                Vector3 p = c.engine.PedSpawnPoint(c.pos, minRadius, maxRadius);
                Ped attacker = CREATE_PED(pedType, model, p.x, p.y, p.z, c.heading, false, false);
                if (!attacker) continue;
                c.engine.ConfigureAttacker(attacker, c.ped, weapon, accuracy);
                c.engine.Track(attacker, kAttackerLifetime);
                ++spawned;
            }
            SET_MODEL_AS_NO_LONGER_NEEDED(model);
            return spawned ? Ok() : Fail("the game wouldn't spawn them here");
        }

        struct Crew
        {
            Hash model;
            int seat;
            Hash weapon;
        };

        // A hostile vehicle with people in it. `chaseFromAir` puts it overhead and has
        // the pilot follow, because a combat task alone leaves helicopters hovering.
        EffectResult SpawnHostileVehicle(EffectContext& c, Hash vehicleModel, std::initializer_list<Crew> crew, int pedType, bool chaseFromAir)
        {
            if (auto wait = NeedModels(c, { vehicleModel })) return *wait;
            for (auto& member : crew)
                if (auto wait = NeedModels(c, { member.model })) return *wait;
            if (!c.engine.HasSpawnRoom((int)crew.size() + 1)) return Retry("too many attackers are already out");

            float heading = c.heading;
            Vector3 p;
            if (chaseFromAir) { p = c.engine.PedSpawnPoint(c.pos, 50.0f, 70.0f); p.z = c.pos.z + 50.0f; }
            else p = c.engine.VehicleSpawnPoint(c.ped, 70.0f, heading);

            Vehicle v = CREATE_VEHICLE(vehicleModel, p.x, p.y, p.z, heading, false, false, false);
            if (!v) return Fail("the game wouldn't spawn it here");
            SET_VEHICLE_ENGINE_ON(v, true, true, false);
            c.engine.Track(v, kAttackerLifetime);

            for (auto& member : crew)
            {
                Ped ped = CREATE_PED_INSIDE_VEHICLE(v, pedType, member.model, member.seat, false, false);
                if (!ped) continue;
                c.engine.ConfigureAttacker(ped, c.ped, member.weapon, 30);
                if (chaseFromAir && member.seat == kSeatDriver) TASK_HELI_CHASE(ped, c.ped, 0.0f, 0.0f, 30.0f);
                c.engine.Track(ped, kAttackerLifetime);
            }

            SET_MODEL_AS_NO_LONGER_NEEDED(vehicleModel);
            for (auto& member : crew) SET_MODEL_AS_NO_LONGER_NEEDED(member.model);
            return Ok();
        }

        EffectResult SetWanted(EffectContext& c, int stars)
        {
            if (GET_PLAYER_WANTED_LEVEL(c.player) >= stars) return Fail("the cops are already on it");
            SET_PLAYER_WANTED_LEVEL(c.player, stars, false);
            SET_PLAYER_WANTED_LEVEL_NOW(c.player, false);
            return Ok();
        }

        // Story-mode money lives in a per-character stat.
        Hash CashStat(Ped ped)
        {
            switch (GET_ENTITY_MODEL(ped))
            {
            case Joaat("player_zero"): return Joaat("SP0_TOTAL_CASH");
            case Joaat("player_one"): return Joaat("SP1_TOTAL_CASH");
            case Joaat("player_two"): return Joaat("SP2_TOTAL_CASH");
            default: return 0;
            }
        }

        // ── Nuisance ────────────────────────────────────────────────────────────

        // Works anywhere: in a vehicle, the streamer bails out first. `a` is the time left on the
        // ground. The minimum and maximum times match, and the tick knocks them back down if
        // anything stands them up, so movement input can't end it early.
        EffectResult Ragdoll(EffectContext& c)
        {
            static constexpr double kSecondsDown = 3.5, kBailOutAllowance = 3.0;
            if (c.vehicle) TASK_LEAVE_VEHICLE(c.ped, c.vehicle, kLeaveBailOut);
            return StartTimed(c, kSecondsDown + (c.vehicle ? kBailOutAllowance : 0.0), [](TimedEffect& t, double dt)
            {
                if (IS_PED_IN_ANY_VEHICLE(t.ped, true)) return;
                t.a -= dt;
                if (t.a <= 0.0) { t.remaining = 0.0; return; }
                if (IS_PED_RAGDOLL(t.ped)) return;
                const int ms = (int)(t.a * 1000.0);
                SET_PED_TO_RAGDOLL(t.ped, ms, ms, 0, false, false, false);
            }, nullptr, kSecondsDown);
        }

        // How far the player is holding one way rather than the other. A control being fed reads
        // back as what it was fed, so this reads the one-direction controls, which feeding leaves alone.
        float HeldAxis(int positive, int negative)
        {
            return std::abs(GET_DISABLED_CONTROL_NORMAL(kControls, positive)) - std::abs(GET_DISABLED_CONTROL_NORMAL(kControls, negative));
        }

        EffectResult Drunk(EffectContext& c)
        {
            static constexpr const char* kClip = "move_m@drunk@verydrunk";
            REQUEST_CLIP_SET(kClip);
            if (!HAS_CLIP_SET_LOADED(kClip)) return Retry("the game couldn't load it in time", 0.2);

            SET_PED_MOVEMENT_CLIPSET(c.ped, kClip, 1.0f);
            SET_PED_IS_DRUNK(c.ped, true);
            SET_PED_MOTION_BLUR(c.ped, true);
            SHAKE_GAMEPLAY_CAM("DRUNK_SHAKE", 1.5f);
            ANIMPOSTFX_PLAY("DrugsDrivingIn", 0, true);
            return StartTimed(c, 15.0, [](TimedEffect& t, double)
            {
                // A fed value beats the key being held, but the game ignores it on a blocked
                // control and the player freezes, so nothing is blocked here.
                if (IS_PED_IN_ANY_VEHICLE(t.ped, false))
                {
                    SET_CONTROL_VALUE_NEXT_FRAME(kControls, kInputVehMoveLR, -HeldAxis(kInputVehRightOnly, kInputVehLeftOnly));
                    return;
                }
                SET_CONTROL_VALUE_NEXT_FRAME(kControls, kInputMoveLR, -HeldAxis(kInputMoveRightOnly, kInputMoveLeftOnly));
                // Forward is negative on this control, so feeding it as positive walks backwards.
                SET_CONTROL_VALUE_NEXT_FRAME(kControls, kInputMoveUD, HeldAxis(kInputMoveUpOnly, kInputMoveDownOnly));
            }, [](TimedEffect& t, bool)
            {
                if (DOES_ENTITY_EXIST(t.startPed))
                {
                    RESET_PED_MOVEMENT_CLIPSET(t.startPed, 0.0f);
                    SET_PED_IS_DRUNK(t.startPed, false);
                    SET_PED_MOTION_BLUR(t.startPed, false);
                }
                STOP_GAMEPLAY_CAM_SHAKING(true);
                ANIMPOSTFX_STOP("DrugsDrivingIn");
            });
        }

        // The DB notes a player-occupied car only honks briefly, so this keeps re-pressing it.
        EffectResult StuckHorn(EffectContext& c)
        {
            SET_VEHICLE_ALARM(c.vehicle, true);
            START_VEHICLE_ALARM(c.vehicle);
            return StartTimed(c, 15.0, [](TimedEffect& t, double dt)
            {
                t.a += dt;
                if (t.a < 0.4 || !VehicleStillThere(t)) return;
                t.a = 0;
                START_VEHICLE_HORN(t.vehicle, 1000, Joaat("HELDDOWN"), false);
            }, [](TimedEffect& t, bool)
            {
                if (VehicleStillThere(t)) SET_VEHICLE_ALARM(t.vehicle, false);
            }, 1.0);
        }

        EffectResult Fog(EffectContext&) { SET_WEATHER_TYPE_NOW_PERSIST("FOGGY"); return Ok(); }
        EffectResult Storm(EffectContext&) { SET_WEATHER_TYPE_NOW_PERSIST("THUNDER"); return Ok(); }
        EffectResult Blizzard(EffectContext&) { SET_WEATHER_TYPE_NOW_PERSIST("BLIZZARD"); return Ok(); }
        EffectResult Midnight(EffectContext&) { SET_CLOCK_TIME(0, 0, 0); return Ok(); }

        EffectResult NoHud(EffectContext& c)
        {
            return StartTimed(c, 45.0, [](TimedEffect&, double) { HIDE_HUD_AND_RADAR_THIS_FRAME(); }, nullptr);
        }

        EffectResult LegDay(EffectContext& c)
        {
            return StartTimed(c, 30.0, [](TimedEffect&, double)
            {
                DISABLE_CONTROL_ACTION(kControls, kInputSprint, true);
                DISABLE_CONTROL_ACTION(kControls, kInputJump, true);
            }, nullptr);
        }

        // ── Disruptive ──────────────────────────────────────────────────────────

        EffectResult Yeet(EffectContext& c)
        {
            if (c.vehicle)
            {
                SET_ENTITY_VELOCITY(c.vehicle, 0.0f, 0.0f, 18.0f);
                return Ok();
            }
            SET_PED_TO_RAGDOLL(c.ped, 4000, 5000, 0, false, false, false);
            SET_ENTITY_VELOCITY(c.ped, (float)c.engine.Random(-8, 8), (float)c.engine.Random(-8, 8), 22.0f);
            return Ok();
        }

        // Straight up, carrying half the car's momentum. With a parachute the ragdoll is cut short,
        // because the jump peaks after about 2.5 seconds and they need time to pull the cord.
        EffectResult Eject(EffectContext& c)
        {
            const Vector3 momentum = GET_ENTITY_VELOCITY(c.vehicle);
            if (IS_VEHICLE_A_CONVERTIBLE(c.vehicle, false)) LOWER_CONVERTIBLE_ROOF(c.vehicle, true);
            else SET_VEHICLE_DOOR_OPEN(c.vehicle, 0, false, true);
            if (c.options.ejectParachute) GIVE_WEAPON_TO_PED(c.ped, kParachute, 1, false, false);

            CLEAR_PED_TASKS_IMMEDIATELY(c.ped);
            if (c.options.ejectParachute) SET_PED_TO_RAGDOLL(c.ped, 1500, 2000, 0, false, false, false);
            else SET_PED_TO_RAGDOLL(c.ped, 3000, 4000, 0, false, false, false);
            SET_ENTITY_VELOCITY(c.ped, momentum.x * 0.5f, momentum.y * 0.5f, 25.0f);
            return Ok();
        }

        EffectResult FlatTires(EffectContext& c)
        {
            SET_VEHICLE_TYRES_CAN_BURST(c.vehicle, true);
            for (int wheel : { 0, 1, 2, 3, 4, 5 }) SET_VEHICLE_TYRE_BURST(c.vehicle, wheel, true, 1000.0f);
            return Ok();
        }

        EffectResult Stall(EffectContext& c)
        {
            return StartTimed(c, 10.0, [](TimedEffect& t, double)
            {
                if (VehicleStillThere(t)) SET_VEHICLE_ENGINE_ON(t.vehicle, false, true, true);
            }, [](TimedEffect& t, bool)
            {
                if (VehicleStillThere(t)) SET_VEHICLE_ENGINE_ON(t.vehicle, true, true, false);
            });
        }

        EffectResult NoBrakes(EffectContext& c)
        {
            return StartTimed(c, 20.0, [](TimedEffect&, double)
            {
                DISABLE_CONTROL_ACTION(kControls, kInputVehBrake, true);
                DISABLE_CONTROL_ACTION(kControls, kInputVehHandbrake, true);
            }, nullptr);
        }

        EffectResult FloorIt(EffectContext& c)
        {
            return StartTimed(c, 12.0, [](TimedEffect&, double)
            {
                SET_CONTROL_VALUE_NEXT_FRAME(kControls, kInputVehAccelerate, 1.0f);
                DISABLE_CONTROL_ACTION(kControls, kInputVehBrake, true);
                DISABLE_CONTROL_ACTION(kControls, kInputVehHandbrake, true);
            }, nullptr);
        }

        EffectResult JammedGun(EffectContext& c)
        {
            return StartTimed(c, 20.0, [](TimedEffect&, double)
            {
                for (int control : { kInputAttack, kInputAttack2, kInputVehAttack, kInputVehAttack2, kInputVehPassengerAttack,
                                     kInputMeleeLight, kInputMeleeHeavy, kInputMeleeAlt, kInputMelee1, kInputMelee2 })
                    DISABLE_CONTROL_ACTION(kControls, control, true);
            }, nullptr);
        }

        EffectResult Butterfingers(EffectContext& c)
        {
            Hash weapon = 0;
            GET_CURRENT_PED_WEAPON(c.ped, &weapon, true);
            if (!weapon || weapon == kWeaponUnarmed) return Retry("the streamer wasn't holding a weapon");
            // SET_PED_DROPS_WEAPON does nothing visible to the player: it's reported to drop only
            // a magazine. This drops the gun itself, 2m away. An ammo count of -1 is what the one
            // working example found uses, and it isn't documented.
            SET_PED_DROPS_INVENTORY_WEAPON(c.ped, weapon, 0.0f, 2.0f, 0.0f, -1);
            SET_CURRENT_PED_WEAPON(c.ped, kWeaponUnarmed, true);
            return Ok();
        }

        EffectResult MoonGravity(EffectContext& c)
        {
            SET_GRAVITY_LEVEL(1);
            // On foot the gravity level was too subtle to notice in-game, so jumps get bigger too.
            return StartTimed(c, 30.0, [](TimedEffect& t, double) { SET_SUPER_JUMP_THIS_FRAME(t.player); },
                              [](TimedEffect&, bool) { SET_GRAVITY_LEVEL(0); });
        }

        EffectResult SlowMo(EffectContext& c)
        {
            SET_TIME_SCALE(0.4f);
            return StartTimed(c, 20.0, nullptr, [](TimedEffect&, bool) { SET_TIME_SCALE(1.0f); });
        }

        EffectResult Blackout(EffectContext& c)
        {
            SET_ARTIFICIAL_LIGHTS_STATE(true);
            return StartTimed(c, 60.0, nullptr, [](TimedEffect&, bool) { SET_ARTIFICIAL_LIGHTS_STATE(false); });
        }

        EffectResult Snitch(EffectContext& c) { return SetWanted(c, 2); }

        EffectResult BadDogs(EffectContext& c)
        {
            return SpawnGang(c, Joaat("a_c_rottweiler"), 0, 3, kPedAnimal, 50);
        }

        // ── Painful ─────────────────────────────────────────────────────────────

        EffectResult HotSeat(EffectContext& c)
        {
            if (c.vehicle) SET_VEHICLE_ENGINE_HEALTH(c.vehicle, -100.0f);
            else START_ENTITY_FIRE(c.ped);
            return Ok();
        }

        EffectResult MostWanted(EffectContext& c) { return SetWanted(c, 4); }

        EffectResult AngryMob(EffectContext& c)
        {
            int size = c.count > 0 ? std::clamp(c.count, 3, 12) : 6;
            return SpawnGang(c, Joaat("g_m_y_mexgoon_01"), kWeaponPistol, size, kPedMission, 20);
        }

        EffectResult MountainLions(EffectContext& c)
        {
            return SpawnGang(c, Joaat("a_c_mtlion"), 0, 2, kPedAnimal, 50, 18.0f, 26.0f);
        }

        EffectResult HitSquad(EffectContext& c)
        {
            const Hash soldier = Joaat("s_m_y_blackops_01");
            return SpawnHostileVehicle(c, Joaat("mesa3"), {
                { soldier, kSeatDriver, kWeaponCarbine }, { soldier, 0, kWeaponCarbine },
                { soldier, 1, kWeaponCarbine }, { soldier, 2, kWeaponCarbine } }, kPedArmy, false);
        }

        EffectResult TaxCollector(EffectContext& c)
        {
            Hash stat = CashStat(c.ped);
            if (!stat) return Fail("this character doesn't carry cash");
            int cash = 0;
            if (!STAT_GET_INT(stat, &cash, -1)) return Fail("couldn't read the streamer's wallet");
            if (cash <= 0) return Fail("the streamer is already broke");
            int take = std::min(cash, std::max(100, cash / 10));
            STAT_SET_INT(stat, cash - take, true);
            return Ok();
        }

        EffectResult Carjacked(EffectContext& c)
        {
            const Hash thiefModel = Joaat("a_m_y_business_01");
            if (auto wait = NeedModels(c, { thiefModel })) return *wait;

            CLEAR_PED_TASKS_IMMEDIATELY(c.ped);
            Ped thief = CREATE_PED_INSIDE_VEHICLE(c.vehicle, kPedCivMale, thiefModel, kSeatDriver, false, false);
            SET_MODEL_AS_NO_LONGER_NEEDED(thiefModel);
            if (!thief) return Fail("the thief couldn't get in");

            SET_VEHICLE_ENGINE_ON(c.vehicle, true, true, false);
            SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(thief, true);
            TASK_VEHICLE_DRIVE_WANDER(thief, c.vehicle, 40.0f, 786603);
            SET_PED_KEEP_TASK(thief, true);
            c.engine.Track(thief, kAttackerLifetime);
            return Ok();
        }

        EffectResult Downgrade(EffectContext& c)
        {
            const Hash scooter = Joaat("faggio2");
            if (auto wait = NeedModels(c, { scooter })) return *wait;

            const float speed = GET_ENTITY_SPEED(c.vehicle);
            const Vector3 here = GET_ENTITY_COORDS(c.vehicle, true);
            // The old vehicle isn't deleted (it might be a saved personal vehicle): it's
            // moved out of the way so the scooter has room to appear.
            const Vector3 behind = GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(c.vehicle, 0.0f, -12.0f, 0.0f);
            SET_ENTITY_COORDS(c.vehicle, behind.x, behind.y, behind.z, false, false, false, false);

            Vehicle v = CREATE_VEHICLE(scooter, here.x, here.y, here.z, c.heading, false, false, false);
            SET_MODEL_AS_NO_LONGER_NEEDED(scooter);
            if (!v) return Fail("the game wouldn't spawn it here");
            SET_PED_INTO_VEHICLE(c.ped, v, kSeatDriver);
            SET_VEHICLE_ENGINE_ON(v, true, true, false);
            SET_VEHICLE_FORWARD_SPEED(v, speed);
            c.engine.Track(v, kGiftLifetime);
            return Ok();
        }

        EffectResult WrongTurn(EffectContext& c)
        {
            static constexpr std::array<Vector3, 7> kSpots = { {
                { 501.8f, 5604.5f, 797.9f },    // Mount Chiliad summit
                { -75.0f, -818.0f, 326.2f },    // Maze Bank roof
                { 1747.0f, 3273.7f, 41.1f },    // Sandy Shores airfield
                { -2047.4f, 3132.1f, 32.8f },   // Fort Zancudo
                { -275.5f, 6635.8f, 7.5f },     // Paleto Bay pier
                { -1850.1f, -1231.7f, 13.0f },  // Del Perro pier
                { 1679.0f, 2513.7f, 45.6f },    // Bolingbroke prison yard
            } };
            const Vector3& spot = kSpots[(size_t)c.engine.Random(0.0, (double)kSpots.size() - 0.001)];
            SET_ENTITY_COORDS(c.vehicle ? c.vehicle : c.ped, spot.x, spot.y, spot.z, false, false, false, true);
            return Ok();
        }

        EffectResult FreeFall(EffectContext& c)
        {
            if (c.vehicle) CLEAR_PED_TASKS_IMMEDIATELY(c.ped);
            GIVE_WEAPON_TO_PED(c.ped, kParachute, 1, false, false);
            SET_ENTITY_COORDS(c.ped, c.pos.x, c.pos.y, c.pos.z + 600.0f, false, false, false, false);
            return Ok();
        }

        // ── Devastating ─────────────────────────────────────────────────────────

        EffectResult CarBomb(EffectContext& c)
        {
            return StartTimed(c, 6.0, [](TimedEffect& t, double dt)
            {
                t.a += dt;
                if (t.a < 1.0) return;
                t.a = 0;
                PLAY_SOUND_FRONTEND(-1, "Beep_Red", "DLC_HEIST_HACKING_SNAKE_SOUNDS", true);
            }, [](TimedEffect& t, bool cancelled)
            {
                if (!cancelled && VehicleStillThere(t)) EXPLODE_VEHICLE(t.vehicle, true, false);
            }, 1.0);
        }

        EffectResult Airstrike(EffectContext& c)
        {
            return StartTimed(c, 8.0, [](TimedEffect& t, double dt)
            {
                t.a += dt;
                if (t.a < 0.35) return;
                t.a = 0;
                // Each hit lands closer: 45m out at the start, 6m by the end.
                const double progress = 1.0 - t.remaining / t.duration;
                const float radius = (float)(45.0 - 39.0 * progress);
                const Vector3 p = RingPoint(t.pos, t.engine->Random(0.0, 6.283185307), radius);
                ADD_EXPLOSION(p.x, p.y, p.z, kExpRocket, 1.0f, true, false, 1.0f, false);
            }, nullptr);
        }

        EffectResult ArmySurplus(EffectContext& c)
        {
            return SpawnHostileVehicle(c, Joaat("rhino"), { { Joaat("s_m_y_marine_01"), kSeatDriver, 0 } }, kPedArmy, false);
        }

        EffectResult AirSupport(EffectContext& c)
        {
            const Hash soldier = Joaat("s_m_y_blackops_01");
            return SpawnHostileVehicle(c, Joaat("buzzard"), {
                { Joaat("s_m_y_pilot_01"), kSeatDriver, 0 },
                { soldier, 1, kWeaponCarbine }, { soldier, 2, kWeaponCarbine } }, kPedArmy, true);
        }

        void ExplodeVehiclesNear(const Vector3& center, float radius)
        {
            std::array<int, 256> vehicles{};
            int count = worldGetAllVehicles(vehicles.data(), (int)vehicles.size());
            for (int i = 0; i < count; ++i)
            {
                Vector3 p = GET_ENTITY_COORDS(vehicles[(size_t)i], false);
                float dx = p.x - center.x, dy = p.y - center.y, dz = p.z - center.z;
                if (dx * dx + dy * dy + dz * dz <= radius * radius) EXPLODE_VEHICLE(vehicles[(size_t)i], true, false);
            }
        }

        // A four-second countdown, then a blinding flash and a shockwave, then aftershocks.
        // Nothing here touches the save, so the damage ends when the streamer respawns.
        EffectResult NuclearChaos(EffectContext& c)
        {
            SET_WEATHER_TYPE_NOW_PERSIST("HALLOWEEN");
            return StartTimed(c, 16.0, [](TimedEffect& t, double dt)
            {
                static constexpr double kDetonateAt = 4.0, kFlashSeconds = 2.0;
                const double elapsed = t.duration - t.remaining;
                t.a += dt;

                if (elapsed < kDetonateAt)
                {
                    if (t.a < 1.0) return;
                    t.a = 0;
                    PLAY_SOUND_FRONTEND(-1, "Beep_Red", "DLC_HEIST_HACKING_SNAKE_SOUNDS", true);
                    return;
                }

                if (t.phase == 0)
                {
                    t.phase = 1;
                    t.a = 0;
                    SHAKE_GAMEPLAY_CAM("LARGE_EXPLOSION_SHAKE", 3.0f);
                    PLAY_SOUND_FRONTEND(-1, "ScreenFlash", "WastedSounds", true);
                    for (int i = 0; i < 8; ++i)
                    {
                        const Vector3 p = RingPoint(t.pos, i * 0.785398, 18.0f);
                        ADD_EXPLOSION(p.x, p.y, p.z, kExpBlimp, 5.0f, true, false, 3.0f, false);
                    }
                    ExplodeVehiclesNear(t.pos, 120.0f);
                }

                const double sinceFlash = elapsed - kDetonateAt;
                if (sinceFlash < kFlashSeconds)
                {
                    const int alpha = std::clamp((int)(255.0 * (1.0 - sinceFlash / kFlashSeconds)), 0, 255);
                    DRAW_RECT(0.5f, 0.5f, 1.0f, 1.0f, 255, 255, 255, alpha, false);
                }

                if (t.a >= 0.5)
                {
                    t.a = 0;
                    const Vector3 p = RingPoint(t.pos, t.engine->Random(0.0, 6.283185307), (float)t.engine->Random(20.0, 120.0));
                    ADD_EXPLOSION(p.x, p.y, p.z, kExpBlimp, 2.0f, true, false, 1.5f, false);
                }
            }, [](TimedEffect&, bool)
            {
                SET_WEATHER_TYPE_NOW_PERSIST("SMOG");
            });
        }

        // Bought in silence, then hits at a random moment in the next 40 seconds. `a` holds the
        // moment it lands. A loud, invisible, harmless explosion is the actual jolt.
        EffectResult JumpScare(EffectContext& c)
        {
            const double hitsAt = c.engine.Random(5.0, 40.0);
            return StartTimed(c, hitsAt + 0.7, [](TimedEffect& t, double)
            {
                const double elapsed = t.duration - t.remaining;
                if (elapsed < t.a) return;
                if (t.phase == 0)
                {
                    t.phase = 1;
                    ADD_EXPLOSION(t.pos.x, t.pos.y, t.pos.z, kExpBlimp, 0.0f, true, true, 2.0f, true);
                    PLAY_SOUND_FRONTEND(-1, "ScreenFlash", "WastedSounds", true);
                    ANIMPOSTFX_PLAY("ExplosionJosh3", 500, false);
                    SHAKE_GAMEPLAY_CAM("LARGE_EXPLOSION_SHAKE", 1.5f);
                }
                const int alpha = std::clamp((int)(255.0 * t.remaining / 0.7), 0, 255);
                DRAW_RECT(0.5f, 0.5f, 1.0f, 1.0f, 255, 255, 255, alpha, false);
            }, nullptr, hitsAt);
        }

        // ── Blessings ───────────────────────────────────────────────────────────

        EffectResult Heal(EffectContext& c)
        {
            SET_ENTITY_HEALTH(c.ped, GET_ENTITY_MAX_HEALTH(c.ped), 0, 0);
            return Ok();
        }

        EffectResult Armor(EffectContext& c)
        {
            SET_PED_ARMOUR(c.ped, GET_PLAYER_MAX_ARMOUR(c.player));
            return Ok();
        }

        EffectResult PitStop(EffectContext& c)
        {
            SET_VEHICLE_FIXED(c.vehicle);
            SET_VEHICLE_DEFORMATION_FIXED(c.vehicle);
            SET_VEHICLE_ENGINE_HEALTH(c.vehicle, 1000.0f);
            SET_VEHICLE_PETROL_TANK_HEALTH(c.vehicle, 1000.0f);
            SET_VEHICLE_DIRT_LEVEL(c.vehicle, 0.0f);
            SET_VEHICLE_ENGINE_ON(c.vehicle, true, true, false);
            return Ok();
        }

        EffectResult CleanRecord(EffectContext& c)
        {
            if (GET_PLAYER_WANTED_LEVEL(c.player) == 0) return Fail("the streamer isn't wanted right now");
            CLEAR_PLAYER_WANTED_LEVEL(c.player);
            return Ok();
        }

        EffectResult Stimulus(EffectContext& c)
        {
            Hash stat = CashStat(c.ped);
            if (!stat) return Fail("this character doesn't carry cash");
            int cash = 0;
            if (!STAT_GET_INT(stat, &cash, -1)) return Fail("couldn't read the streamer's wallet");
            STAT_SET_INT(stat, cash > 2000000000 ? cash : cash + 5000, true);
            return Ok();
        }

        EffectResult AmmoDrop(EffectContext& c)
        {
            Hash weapon = 0;
            GET_CURRENT_PED_WEAPON(c.ped, &weapon, true);
            if (!weapon || weapon == kWeaponUnarmed) return Retry("the streamer wasn't holding a gun");
            ADD_AMMO_TO_PED(c.ped, weapon, 250);
            return Ok();
        }

        EffectResult ArmsDealer(EffectContext& c)
        {
            GIVE_WEAPON_TO_PED(c.ped, kWeaponPistol, 150, false, false);
            GIVE_WEAPON_TO_PED(c.ped, kWeaponSmg, 300, false, false);
            GIVE_WEAPON_TO_PED(c.ped, kWeaponCarbine, 300, false, true);
            return Ok();
        }

        EffectResult GiftCar(EffectContext& c)
        {
            const Hash model = Joaat("zentorno");
            if (auto wait = NeedModels(c, { model })) return *wait;

            const Vector3 p = GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(c.ped, 0.0f, 7.0f, 0.5f);
            Vehicle v = CREATE_VEHICLE(model, p.x, p.y, p.z, c.heading, false, false, false);
            SET_MODEL_AS_NO_LONGER_NEEDED(model);
            if (!v) return Fail("the game wouldn't spawn it here");
            SET_VEHICLE_ON_GROUND_PROPERLY(v, 5.0f);
            const std::string plate = PlateText(c.viewer); // the buyer's name goes on the plate
            SET_VEHICLE_NUMBER_PLATE_TEXT(v, plate.c_str());
            c.engine.Track(v, kGiftLifetime);
            return Ok();
        }

        EffectResult ClearSkies(EffectContext&)
        {
            CLEAR_WEATHER_TYPE_PERSIST();
            SET_WEATHER_TYPE_NOW("EXTRASUNNY");
            return Ok();
        }

        EffectResult Daybreak(EffectContext&) { SET_CLOCK_TIME(12, 0, 0); return Ok(); }

        EffectResult PlotArmor(EffectContext& c)
        {
            SET_PLAYER_INVINCIBLE(c.player, true);
            return StartTimed(c, 20.0, nullptr, [](TimedEffect& t, bool) { SET_PLAYER_INVINCIBLE(t.player, false); });
        }

        EffectResult Caffeine(EffectContext& c)
        {
            SET_RUN_SPRINT_MULTIPLIER_FOR_PLAYER(c.player, 1.49f); // the native's maximum
            return StartTimed(c, 30.0, [](TimedEffect& t, double) { SET_SUPER_JUMP_THIS_FRAME(t.player); },
                              [](TimedEffect& t, bool) { SET_RUN_SPRINT_MULTIPLIER_FOR_PLAYER(t.player, 1.0f); });
        }

        // ── Direct-command chaos ────────────────────────────────────────────────

        // What Carmageddon drops. They stay loaded while it runs.
        const std::initializer_list<Hash> kFallingModels = {
            Joaat("asea"), Joaat("sandking"), Joaat("zentorno"), Joaat("police"), Joaat("ambulance"),
            Joaat("firetruk"), Joaat("dump"), Joaat("bus"), Joaat("mule") };

        // How many fall together. The min guards against the rare draw that lands exactly on 16.
        int BatchSize(EffectEngine& engine) { return std::min(15, (int)engine.Random(10.0, 16.0)); }

        // Drops vehicles in front of the camera and hands each straight back to the game, which
        // cleans it up like any other wreck, so a stream full of these can't exhaust the vehicle
        // pool. They fall in about a second, and spread all round, a tester saw only explosions.
        // Returns how many the game spawned.
        int DropVehicles(EffectEngine& engine, const Vector3& center, int count)
        {
            static constexpr float kHeight = 35.0f, kNearest = 15.0f, kFarthest = 60.0f;
            static constexpr double kDegrees = 3.14159265358979 / 180.0, kHalfArc = 35.0 * kDegrees;
            // The camera's heading counts from north, and RingPoint's angle from east.
            const double ahead = (GET_GAMEPLAY_CAM_ROT(2).z + 90.0) * kDegrees;
            int dropped = 0;
            for (int i = 0; i < count; ++i)
            {
                // The square root spreads them evenly over the area rather than bunching them close in.
                const float radius = (float)std::sqrt(engine.Random(kNearest * kNearest, kFarthest * kFarthest));
                const Vector3 p = RingPoint(center, ahead + engine.Random(-kHalfArc, kHalfArc), radius);
                float ground = 0;
                if (!GET_GROUND_Z_FOR_3D_COORD(p.x, p.y, center.z + 50.0f, &ground, false, false)) ground = center.z;

                const float yaw = (float)engine.Random(0.0, 360.0);
                const Hash model = kFallingModels.begin()[(size_t)engine.Random(0.0, (double)kFallingModels.size() - 0.001)];
                Vehicle v = CREATE_VEHICLE(model, p.x, p.y, ground + kHeight, yaw, false, false, false);
                if (!v) continue;
                SET_ENTITY_ROTATION(v, (float)engine.Random(-60.0, 60.0), (float)engine.Random(-60.0, 60.0), yaw, 2, true);
                SET_ENTITY_VELOCITY(v, 0.0f, 0.0f, (float)engine.Random(-40.0, -25.0));
                SET_ENTITY_ANGULAR_VELOCITY(v, (float)engine.Random(-3.0, 3.0), (float)engine.Random(-3.0, 3.0), (float)engine.Random(-1.0, 1.0));
                SET_ENTITY_AS_NO_LONGER_NEEDED(&v);
                ++dropped;
            }
            return dropped;
        }

        // The first batch of 10 to 15 falls straight away, which also shows whether the game will
        // spawn them here. Three more fall within 5 seconds either side of the 15, 30 and 45
        // second marks, so the gaps vary. `phase` counts those three, and `a` is when the next
        // one falls.
        EffectResult Carmageddon(EffectContext& c)
        {
            static constexpr int kMoreBatches = 3;
            static constexpr double kApart = 15.0, kJitter = 5.0;
            // A model that takes too long ends in the queue's usual two-minute refund.
            if (auto wait = NeedModels(c, kFallingModels)) return *wait;
            if (DropVehicles(c.engine, c.pos, BatchSize(c.engine)) == 0)
            {
                for (Hash model : kFallingModels) SET_MODEL_AS_NO_LONGER_NEEDED(model);
                return Fail("the game wouldn't spawn them here");
            }
            return StartTimed(c, 60.0, [](TimedEffect& t, double)
            {
                if (t.phase >= kMoreBatches || t.duration - t.remaining < t.a) return;
                DropVehicles(*t.engine, t.pos, BatchSize(*t.engine)); // ones the game won't spawn are skipped, not retried
                ++t.phase;
                t.a = kApart * (t.phase + 1) + t.engine->Random(-kJitter, kJitter);
            }, [](TimedEffect&, bool)
            {
                for (Hash model : kFallingModels) SET_MODEL_AS_NO_LONGER_NEEDED(model);
            }, kApart + c.engine.Random(-kJitter, kJitter));
        }

        // Natives only: this mod never patches handling data in memory.
        EffectResult IceTires(EffectContext& c)
        {
            return StartTimed(c, 15.0, [](TimedEffect& t, double)
            {
                if (StillInVehicle(t)) SET_VEHICLE_REDUCE_GRIP(t.vehicle, true);
                else t.remaining = 0.0;
            }, [](TimedEffect& t, bool)
            {
                if (VehicleStillThere(t)) SET_VEHICLE_REDUCE_GRIP(t.vehicle, false);
            });
        }

        // What Pinball saw on foot, logged when it ends, because only the real game can show
        // which of these checks stops a throw.
        struct PinballFootLog { int frames = 0, reported = 0, ragdolling = 0, discarded = 0, ahead = 0, thrown = 0; };
        PinballFootLog g_pinballFoot;

        // Looks for something solid just in front of someone walking or pushing into it. On foot
        // the mod has never once acted on the game's own report of a hit, so this doesn't rely on
        // it. Like the game's normal, the answer points into the surface.
        bool SomethingAhead(const TimedEffect& t, float impact, Vector3& into)
        {
            static constexpr float kWalking = 1.0f, kPushing = 0.3f, kReach = 0.45f, kRayHeight = 0.2f;
            static constexpr int kWorldVehiclesPedsObjects = 1 | 2 | 4 | 8 | 16;
            const float held = std::abs(HeldAxis(kInputMoveUpOnly, kInputMoveDownOnly)) + std::abs(HeldAxis(kInputMoveRightOnly, kInputMoveLeftOnly));
            if (impact < kWalking && held < kPushing) return false;

            const Vector3 forward = GET_ENTITY_FORWARD_VECTOR(t.ped);
            const float z = t.pos.z + kRayHeight;
            const int test = START_EXPENSIVE_SYNCHRONOUS_SHAPE_TEST_LOS_PROBE(t.pos.x, t.pos.y, z, t.pos.x + forward.x * kReach, t.pos.y + forward.y * kReach, z,
                                                                              kWorldVehiclesPedsObjects, t.ped, 7);
            BOOL hit = false;
            Vector3 end, normal;
            Entity struck = 0;
            if (GET_SHAPE_TEST_RESULT(test, &hit, &end, &normal, &struck) != 2 || !hit) return false;
            into.x = -normal.x;
            into.y = -normal.y;
            into.z = -normal.z;
            return true;
        }

        // `a` is the time until the next bounce is allowed, and `b` is the speed last frame.
        EffectResult Pinball(EffectContext& c)
        {
            g_pinballFoot = {};
            return StartTimed(c, 20.0, [](TimedEffect& t, double dt)
            {
                static constexpr float kMultiplier = 8.0f, kMinBounce = 12.0f, kMaxOnFoot = 20.0f, kMaxInVehicle = 40.0f, kLift = 3.0f;
                static constexpr double kGapOnFoot = 2.5, kGapInVehicle = 0.35;
                const bool inVehicle = IS_PED_IN_ANY_VEHICLE(t.ped, false);
                const Entity body = inVehicle ? GET_VEHICLE_PED_IS_IN(t.ped, false) : t.ped;

                // A hit has already slowed them by the time it's reported, so the speed going in
                // is last frame's.
                const float speed = LengthOf(GET_ENTITY_VELOCITY(body));
                const float impact = std::max(speed, (float)t.b);
                t.b = speed;

                t.a -= dt;
                if (!inVehicle)
                {
                    // Left on afterwards: the game can't be asked what this was before, and it
                    // seems to be on already, because a test plugin saw hits without it.
                    SET_ENTITY_RECORDS_COLLISIONS(t.ped, true);
                    ++g_pinballFoot.frames;
                }
                if (t.a > 0.0) return;

                // The game's normal points into whatever was hit, so the floor reads as straight
                // down and shouldn't bounce, and away from the hit is the opposite direction.
                const auto bounces = [](const Vector3& into) { return LengthOf(into) >= 0.5f && into.z >= -0.75f; };
                Vector3 hit;
                bool found = false;
                const char* how = "reported by the game";
                if (HAS_ENTITY_COLLIDED_WITH_ANYTHING(body))
                {
                    hit = GET_COLLISION_NORMAL_OF_LAST_HIT_FOR_ENTITY(body);
                    found = bounces(hit);
                    if (!inVehicle)
                    {
                        ++g_pinballFoot.reported;
                        if (IS_PED_RAGDOLL(t.ped)) ++g_pinballFoot.ragdolling;
                        if (!found) ++g_pinballFoot.discarded;
                    }
                }
                if (!found && !inVehicle && SomethingAhead(t, impact, hit) && bounces(hit))
                {
                    found = true;
                    how = "found just ahead";
                    ++g_pinballFoot.ahead;
                }
                if (!found) return;

                // Clamped so a fast crash can't throw them through the map, and never downward
                // for the same reason.
                const float push = std::clamp(impact * kMultiplier, kMinBounce, inVehicle ? kMaxInVehicle : kMaxOnFoot);
                if (!inVehicle)
                {
                    SET_PED_TO_RAGDOLL(t.ped, 1500, 2000, 0, false, false, false);
                    ++g_pinballFoot.thrown;
                }
                SET_ENTITY_VELOCITY(body, -hit.x * push, -hit.y * push, std::max(-hit.z * push, 0.0f) + kLift);
                log::Info("Pinball threw the streamer off a hit at {:.0f} m/s, {}", push, how);
                // Someone on foot gets time to get back up, and a lower cap above, so that a run
                // of hits can't kill them outright.
                t.a = inVehicle ? kGapInVehicle : kGapOnFoot;
            }, [](TimedEffect&, bool)
            {
                const PinballFootLog& p = g_pinballFoot;
                if (p.frames == 0) return;
                log::Info("Pinball on foot: {} frames, game reported {} hits ({} while ragdolling, {} discarded), {} more found ahead, {} throws",
                          p.frames, p.reported, p.ragdolling, p.discarded, p.ahead, p.thrown);
            });
        }

        // Unlike Downgrade, this deletes the old vehicle, so a saved personal vehicle could be lost.
        EffectResult VehicleRoulette(EffectContext& c)
        {
            static constexpr std::array<Hash, 7> kPool = { Joaat("faggio2"), Joaat("rhino"), Joaat("tractor"), Joaat("lazer"),
                                                           Joaat("submersible"), Joaat("bulldozer"), Joaat("blimp") };
            // Kept between tries, so a retry waits for the same model rather than asking for another.
            static Hash pick = 0;
            if (!pick) pick = kPool[(size_t)c.engine.Random(0.0, (double)kPool.size() - 0.001)];
            if (auto wait = NeedModels(c, { pick }))
            {
                if (wait->kind == Kind::Fail) pick = 0;
                return *wait;
            }
            const Hash model = pick;
            pick = 0;

            const Vector3 here = GET_ENTITY_COORDS(c.vehicle, true);
            const Vector3 rotation = GET_ENTITY_ROTATION(c.vehicle, 2);
            const Vector3 velocity = GET_ENTITY_VELOCITY(c.vehicle);

            CLEAR_PED_TASKS_IMMEDIATELY(c.ped);
            Vehicle old = c.vehicle;
            SET_ENTITY_AS_MISSION_ENTITY(old, true, true);
            DELETE_VEHICLE(&old);

            Vehicle v = CREATE_VEHICLE(model, here.x, here.y, here.z, c.heading, false, false, false);
            SET_MODEL_AS_NO_LONGER_NEEDED(model);
            if (!v) return Fail("the game wouldn't spawn it here");
            SET_ENTITY_ROTATION(v, rotation.x, rotation.y, rotation.z, 2, true);
            SET_PED_INTO_VEHICLE(c.ped, v, kSeatDriver);
            SET_VEHICLE_ENGINE_ON(v, true, true, false);
            SET_ENTITY_VELOCITY(v, velocity.x, velocity.y, velocity.z);
            c.engine.Track(v, kGiftLifetime);
            return Ok();
        }

        // `a` is the speed it's held at.
        EffectResult LockThrottle(EffectContext& c)
        {
            const float cruise = std::max(GET_VEHICLE_ESTIMATED_MAX_SPEED(c.vehicle), 30.0f);
            return StartTimed(c, 12.0, [](TimedEffect& t, double)
            {
                if (!StillInVehicle(t)) { t.remaining = 0.0; return; }
                SET_VEHICLE_FORWARD_SPEED(t.vehicle, (float)t.a);
                DISABLE_CONTROL_ACTION(kControls, kInputVehBrake, true);
                DISABLE_CONTROL_ACTION(kControls, kInputVehHandbrake, true);
            }, nullptr, cruise);
        }

        // The vehicle and driver are explosion-proof while it runs, or the first honk would be the last.
        // `a` is the time until the next explosion is allowed.
        EffectResult ExplodingHorn(EffectContext& c)
        {
            SET_ENTITY_PROOFS(c.vehicle, false, false, true, false, false, false, false, false);
            SET_ENTITY_PROOFS(c.ped, false, false, true, false, false, false, false, false);
            return StartTimed(c, 25.0, [](TimedEffect& t, double dt)
            {
                t.a -= dt;
                if (t.a > 0.0 || !StillInVehicle(t) || !IS_CONTROL_PRESSED(kControls, kInputVehHorn)) return;
                t.a = 0.5;
                const Vector3 p = GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(t.vehicle, 0.0f, (float)t.engine->Random(6.0, 10.0), 0.0f);
                ADD_EXPLOSION(p.x, p.y, p.z, kExpRocket, 1.0f, true, false, 1.0f, false);
            }, [](TimedEffect& t, bool)
            {
                if (VehicleStillThere(t)) SET_ENTITY_PROOFS(t.vehicle, false, false, false, false, false, false, false, false);
                if (DOES_ENTITY_EXIST(t.startPed)) SET_ENTITY_PROOFS(t.startPed, false, false, false, false, false, false, false, false);
            });
        }

        // Script fiber only, and only one of these effects runs at a time.
        struct Maniac
        {
            Ped driver;
            Vehicle vehicle;
            Hash originalGroup;
        };
        std::vector<Maniac> g_maniacs;

        EffectResult MadMaxTraffic(EffectContext& c)
        {
            static constexpr float kRadius = 100.0f, kExtraTopSpeed = 30.0f, kMaxSpeed = 70.0f;
            static constexpr size_t kMostCars = 16;

            std::array<int, 256> vehicles{};
            const int count = worldGetAllVehicles(vehicles.data(), (int)vehicles.size());
            const Hash hostile = c.engine.HostileGroup();
            g_maniacs.clear();
            for (int i = 0; i < count && g_maniacs.size() < kMostCars; ++i)
            {
                const Vehicle v = vehicles[(size_t)i];
                if (v == c.vehicle || !Within(GET_ENTITY_COORDS(v, false), c.pos, kRadius)) continue;
                // Checked first, because asking for the driver of a parked car can create one.
                if (IS_VEHICLE_SEAT_FREE(v, kSeatDriver, false)) continue;
                const Ped driver = GET_PED_IN_VEHICLE_SEAT(v, kSeatDriver, false);
                if (!driver || IS_PED_A_PLAYER(driver) || IS_ENTITY_DEAD(driver, false)) continue;

                g_maniacs.push_back({ driver, v, GET_PED_RELATIONSHIP_GROUP_HASH(driver) });
                SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(driver, true);
                SET_PED_RELATIONSHIP_GROUP_HASH(driver, hostile);
                SET_PED_COMBAT_ATTRIBUTES(driver, kCombatLeaveVehicles, false);
                SET_DRIVER_ABILITY(driver, 1.0f);
                SET_DRIVER_AGGRESSIVENESS(driver, 1.0f);
                MODIFY_VEHICLE_TOP_SPEED(v, kExtraTopSpeed);
                TASK_VEHICLE_MISSION_PED_TARGET(driver, v, c.ped, kMissionRam, kMaxSpeed, kDriveRushed, 0.0f, 0.0f, true);
                SET_PED_KEEP_TASK(driver, true);
            }
            if (g_maniacs.empty()) return Retry("there was no traffic nearby", 2.0);

            return StartTimed(c, 20.0, nullptr, [](TimedEffect&, bool)
            {
                for (const Maniac& m : g_maniacs)
                {
                    if (DOES_ENTITY_EXIST(m.vehicle)) MODIFY_VEHICLE_TOP_SPEED(m.vehicle, 0.0f);
                    if (!DOES_ENTITY_EXIST(m.driver)) continue;
                    SET_PED_RELATIONSHIP_GROUP_HASH(m.driver, m.originalGroup);
                    SET_PED_COMBAT_ATTRIBUTES(m.driver, kCombatLeaveVehicles, true);
                    SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(m.driver, false);
                    SET_PED_KEEP_TASK(m.driver, false);
                    CLEAR_PED_TASKS(m.driver);
                    if (DOES_ENTITY_EXIST(m.vehicle) && !IS_ENTITY_DEAD(m.driver, false)) TASK_VEHICLE_DRIVE_WANDER(m.driver, m.vehicle, 20.0f, 786603);
                }
                g_maniacs.clear();
            });
        }

        // Script fiber only, and only one of these effects runs at a time.
        struct FloodState
        {
            float x = 0, y = 0, normalLevel = 0, highLevel = 0;
        };
        FloodState g_flood;

        // Raises the water with MODIFY_WATER. How far that reaches in the real game isn't known yet,
        // so the storm around it keeps the effect visible regardless. `a` paces the height updates.
        EffectResult FlashFlood(EffectContext& c)
        {
            static constexpr float kOffset = 25.0f, kAboveGround = 3.0f;

            float water = 0, ground = c.pos.z;
            if (!GET_WATER_HEIGHT(c.pos.x, c.pos.y, c.pos.z, &water)) water = 0.0f; // sea level
            GET_GROUND_Z_FOR_3D_COORD(c.pos.x, c.pos.y, c.pos.z + 2.0f, &ground, false, false);
            g_flood = { c.pos.x, c.pos.y, water, std::max(water + kOffset, ground + kAboveGround) };

            SET_RAIN(1.0f);
            SET_DEEP_OCEAN_SCALER(3.0f);
            return StartTimed(c, 30.0, [](TimedEffect& t, double dt)
            {
                static constexpr double kRiseSeconds = 6.0, kUpdateEvery = 0.25;
                static constexpr float kRadius = 150.0f;

                t.a += dt;
                if (t.a >= kUpdateEvery)
                {
                    t.a = 0;
                    // Up over the first seconds, level, then back down over the last ones.
                    const double elapsed = t.duration - t.remaining;
                    const float amount = (float)std::clamp(std::min(elapsed, t.remaining) / kRiseSeconds, 0.0, 1.0);
                    MODIFY_WATER(g_flood.x, g_flood.y, kRadius, g_flood.normalLevel + (g_flood.highLevel - g_flood.normalLevel) * amount);
                }

                if (IS_PED_IN_ANY_VEHICLE(t.ped, false))
                {
                    const Vehicle v = GET_VEHICLE_PED_IS_IN(t.ped, false);
                    if (IS_ENTITY_IN_WATER(v)) SET_VEHICLE_ENGINE_ON(v, false, true, true);
                }
            }, [](TimedEffect& t, bool)
            {
                MODIFY_WATER(g_flood.x, g_flood.y, 150.0f, g_flood.normalLevel);
                RESET_DEEP_OCEAN_SCALER();
                SET_RAIN(-1.0f); // back to whatever the weather calls for
                if (IS_PED_IN_ANY_VEHICLE(t.ped, false)) SET_VEHICLE_ENGINE_ON(GET_VEHICLE_PED_IS_IN(t.ped, false), true, false, false);
            });
        }

        // Script fiber only, and only one of these effects runs at a time.
        struct Shooter
        {
            Ped ped;
            Vector3 lastImpact;
        };
        std::vector<Shooter> g_shooters;

        void RefreshShooters(const TimedEffect& t)
        {
            static constexpr float kRadius = 80.0f;
            static constexpr size_t kMostPeds = 40;

            std::array<int, 256> peds{};
            const int count = worldGetAllPeds(peds.data(), (int)peds.size());
            std::vector<Shooter> found;
            for (int i = 0; i < count && found.size() < kMostPeds; ++i)
            {
                const Ped ped = peds[(size_t)i];
                if (IS_PED_A_PLAYER(ped) || IS_ENTITY_DEAD(ped, false) || !Within(GET_ENTITY_COORDS(ped, false), t.pos, kRadius)) continue;
                Shooter shooter{ ped, {} };
                for (const Shooter& known : g_shooters)
                    if (known.ped == ped) shooter.lastImpact = known.lastImpact;
                found.push_back(shooter);
            }
            g_shooters = std::move(found);
        }

        // The game only has explosive ammo for players, and it's per frame, so it stops by itself.
        // For everyone else, an explosion goes off where each of their shots lands.
        // `a` is the time until the next list refresh, and `b` the time until the next explosion.
        EffectResult WarzoneRounds(EffectContext& c)
        {
            g_shooters.clear();
            return StartTimed(c, 20.0, [](TimedEffect& t, double dt)
            {
                SET_EXPLOSIVE_AMMO_THIS_FRAME(t.player);
                t.a -= dt;
                t.b -= dt;
                if (t.a <= 0.0) { t.a = 1.0; RefreshShooters(t); }

                for (Shooter& shooter : g_shooters)
                {
                    if (!DOES_ENTITY_EXIST(shooter.ped) || !IS_PED_SHOOTING(shooter.ped)) continue;
                    Vector3 impact;
                    if (!GET_PED_LAST_WEAPON_IMPACT_COORD(shooter.ped, &impact) || Within(impact, shooter.lastImpact, 0.1f)) continue;
                    shooter.lastImpact = impact;
                    if (t.b > 0.0) continue; // too many at once could hurt the frame rate
                    t.b = 0.15;
                    ADD_EXPLOSION(impact.x, impact.y, impact.z, kExpRocket, 1.0f, true, false, 0.5f, false);
                }
            }, [](TimedEffect&, bool) { g_shooters.clear(); });
        }

        // `a` is the weapon, and `b` is 1 if it was only lent and goes back at the end.
        EffectResult WeaponRoulette(EffectContext& c)
        {
            struct Prize
            {
                Hash weapon;
                int ammo;
            };
            static constexpr std::array<Prize, 6> kPrizes = { {
                { Joaat("weapon_bat"), 1 }, { Joaat("weapon_firework"), 2 }, { Joaat("weapon_flaregun"), 3 },
                { Joaat("weapon_heavysniper"), 1 }, { Joaat("weapon_petrolcan"), 100 }, { Joaat("weapon_snowball"), 5 } } };
            static constexpr double kLockedSeconds = 5.0, kLentSeconds = 30.0;

            const Prize& prize = kPrizes[(size_t)c.engine.Random(0.0, (double)kPrizes.size() - 0.001)];
            const bool lent = !HAS_PED_GOT_WEAPON(c.ped, prize.weapon, false);
            if (lent) GIVE_WEAPON_TO_PED(c.ped, prize.weapon, prize.ammo, false, true);
            SET_CURRENT_PED_WEAPON(c.ped, prize.weapon, true);
            SET_PED_CAN_SWITCH_WEAPON(c.ped, false);

            return StartTimed(c, kLentSeconds, [](TimedEffect& t, double)
            {
                if (t.duration - t.remaining < kLockedSeconds)
                {
                    // Only forced again if something took it away, because re-equipping it
                    // every frame could keep restarting the animation.
                    Hash held = 0;
                    GET_CURRENT_PED_WEAPON(t.ped, &held, true);
                    if (held != (Hash)t.a) SET_CURRENT_PED_WEAPON(t.ped, (Hash)t.a, true);
                    for (int input : kInputsChangeWeapon) DISABLE_CONTROL_ACTION(kControls, input, true);
                }
                else if (t.phase == 0)
                {
                    t.phase = 1;
                    SET_PED_CAN_SWITCH_WEAPON(t.ped, true);
                }
            }, [](TimedEffect& t, bool)
            {
                if (!DOES_ENTITY_EXIST(t.startPed)) return;
                SET_PED_CAN_SWITCH_WEAPON(t.startPed, true);
                if (t.b != 0.0) REMOVE_WEAPON_FROM_PED(t.startPed, (Hash)t.a);
            }, prize.weapon, lent ? 1.0 : 0.0);
        }

        // Handed straight back to the game, so they don't count against the spawn cap. They keep
        // attacking on SET_PED_KEEP_TASK, and the game clears them away when they're dead or far off.
        EffectResult CloneSwarm(EffectContext& c)
        {
            static constexpr int kClones = 10;
            static constexpr std::array<Hash, 3> kMelee = { Joaat("weapon_machete"), Joaat("weapon_golfclub"), Joaat("weapon_nightstick") };
            const Hash model = Joaat("s_m_y_clown_01");
            if (auto wait = NeedModels(c, { model })) return *wait;

            int spawned = 0;
            for (int i = 0; i < kClones; ++i)
            {
                const Vector3 p = c.engine.PedSpawnPoint(c.pos, 15.0f, 25.0f);
                Ped clone = CREATE_PED(kPedMission, model, p.x, p.y, p.z, c.heading, false, false);
                if (!clone) continue;
                c.engine.ConfigureAttacker(clone, c.ped, kMelee[(size_t)i % kMelee.size()], 30);
                SET_ENTITY_AS_NO_LONGER_NEEDED(&clone);
                ++spawned;
            }
            SET_MODEL_AS_NO_LONGER_NEEDED(model);
            return spawned ? Ok() : Fail("the game wouldn't spawn them here");
        }

        // ── Character swarms ────────────────────────────────────────────────────

        constexpr int kSwarmSize = 8;
        constexpr float kSwarmNearest = 12.0f, kSwarmFarthest = 25.0f;

        // What makes one swarm different from another. A new swarm is a row here, plus a row with
        // the same id in the effect table below.
        struct SwarmPreset
        {
            const char* type;                    // the effect's id, so also its chat command
            std::array<const char*, 2> models;   // the first one this game has is used
            std::array<const char*, 2> weapons;  // likewise; none at all means bare fists
            int accuracy;
            float meleeBoost;                    // AI melee damage while any of them is alive; 0 leaves it alone
            double retreatAfter;                 // seconds before any still standing run off; 0 means never
        };

        // The aliens retreat because a stun gun knocks the streamer down without killing them, so
        // eight could otherwise keep them on the ground for good.
        constexpr std::array<SwarmPreset, 7> kSwarms = { {
            { "aliens",     { "s_m_m_movalien_01" },                { "weapon_stungun" },                    25, 0.0f, 45.0 },
            { "clowns",     { "s_m_y_clown_01" },                   { "weapon_bat" },                        30, 0.0f, 0.0 },
            { "astronauts", { "s_m_m_movspace_01" },                { "weapon_raypistol", "weapon_pistol" }, 25, 0.0f, 0.0 },
            { "zombies",    { "u_m_y_zombie_01" },                  {},                                      20, 0.0f, 0.0 },
            { "sasquatch",  { "cs_orleans", "ig_orleans" },         {},                                      20, 3.0f, 0.0 },
            { "lesters",    { "ig_lestercrest", "cs_lestercrest" }, { "weapon_golfclub" },                   30, 0.0f, 0.0 },
            { "cultists",   { "a_m_y_acult_01" },                   { "weapon_machete" },                    30, 0.0f, 0.0 },
        } };

        const SwarmPreset* FindSwarm(std::string_view type)
        {
            for (const SwarmPreset& s : kSwarms)
                if (type == s.type) return &s;
            return nullptr;
        }

        Hash FirstModelInGame(const std::array<const char*, 2>& names)
        {
            for (const char* name : names)
                if (name && IS_MODEL_IN_CDIMAGE(Joaat(name)) && IS_MODEL_VALID(Joaat(name))) return Joaat(name);
            return 0;
        }

        Hash FirstWeaponInGame(const std::array<const char*, 2>& names)
        {
            for (const char* name : names)
                if (name && IS_WEAPON_VALID(Joaat(name))) return Joaat(name);
            return 0;
        }

        // Headings count anticlockwise from north.
        float HeadingToward(const Vector3& from, const Vector3& to)
        {
            const double degrees = std::atan2(-(to.x - from.x), to.y - from.y) * 180.0 / 3.14159265358979;
            return (float)(degrees < 0.0 ? degrees + 360.0 : degrees);
        }

        // Surrounds the streamer with attackers of one kind, 12 to 25m out, facing in. Each is
        // handed straight back to the game, so the entity pools free up as they die or wander off,
        // and keeps fighting on SET_PED_KEEP_TASK. The engine keeps a red blip on each until it dies.
        EffectResult TriggerCharacterSwarm(EffectContext& c, std::string_view swarmType, int count = kSwarmSize)
        {
            const SwarmPreset* swarm = FindSwarm(swarmType);
            if (!swarm) return Fail("there's no swarm by that name");
            const Hash model = FirstModelInGame(swarm->models);
            if (!model) return Fail("this game version doesn't have that model");
            if (auto wait = NeedModels(c, { model })) return *wait;
            if (!c.engine.HasSpawnRoom(count)) return Retry("too many attackers are already out");

            const Hash weapon = FirstWeaponInGame(swarm->weapons);
            if (swarm->weapons[0] && !weapon) log::Warn("This game version has none of the {} swarm's weapons, so they fight bare-handed", swarm->type);

            // Evenly round the circle, each nudged a little, so they come from every side.
            const double spacing = 6.283185307 / count;
            const double start = c.engine.Random(0.0, spacing);
            int spawned = 0;
            for (int i = 0; i < count; ++i)
            {
                const double angle = start + spacing * i + c.engine.Random(-spacing / 4.0, spacing / 4.0);
                const Vector3 p = c.engine.PedSpawnPoint(c.pos, angle, kSwarmNearest, kSwarmFarthest);
                Ped attacker = CREATE_PED(kPedMission, model, p.x, p.y, p.z, HeadingToward(p, c.pos), false, false);
                if (!attacker) continue;
                c.engine.ConfigureAttacker(attacker, c.ped, weapon, swarm->accuracy, true);
                c.engine.MarkEnemy(attacker, swarm->meleeBoost, swarm->retreatAfter);
                SET_ENTITY_AS_NO_LONGER_NEEDED(&attacker); // this zeroes the handle, so it comes last
                ++spawned;
            }
            SET_MODEL_AS_NO_LONGER_NEEDED(model);
            return spawned ? Ok() : Fail("the game wouldn't spawn them here");
        }

        // Every swarm in the effect table runs this, and its id picks the preset. It's the usual
        // eight unless the purchase carries its own size, as a raid's mob does.
        EffectResult CharacterSwarm(EffectContext& c)
        {
            return TriggerCharacterSwarm(c, c.def.id, c.count > 0 ? std::clamp(c.count, 1, 12) : kSwarmSize);
        }

        // ── Morph ───────────────────────────────────────────────────────────────

        // The streamer's own ped isn't changed at all. It waits, hidden, while they play as a new
        // one, then they're put back in it, so their weapons, attachments, outfit and tattoos all
        // come back exactly. Changing the player's model instead would lose those.
        struct MorphBody
        {
            std::array<const char*, 2> models; // the first one this game has is used
            bool animal;
        };

        constexpr std::array<MorphBody, 6> kMorphBodies = { {
            { { "s_m_m_movalien_01" }, false },
            { { "cs_orleans", "ig_orleans" }, false },
            { { "u_m_y_imporage" }, false },
            { { "s_m_y_mime" }, false },
            { { "a_c_chimp" }, true },
            { { "u_m_y_zombie_01" }, false },
        } };

        // No getting into vehicles, no phone (so no quick save) and no switching characters,
        // because the game's own scripts expect the player to be one of the three.
        constexpr std::array<int, 7> kMorphBlockedInputs = { 23, 27, 19, 166, 167, 168, 169 };

        // Story mode only respawns the player as one of its three characters. Dying as anyone else
        // left the screen black for good on 15 November 2026, and being busted most likely does the
        // same. So the body has health to spare, and once the streamer's own share of it is gone,
        // they change back and die as themselves. The health bar hardly moves meanwhile.
        constexpr int kMorphSpareHealth = 5000;
        constexpr int kDeadHealth = 100; // a ped at this health or below is dead

        struct Morph
        {
            Ped original = 0; // the streamer's own ped
            Ped body = 0;     // the one they're playing as meanwhile
            Hash originalModel = 0, bodyModel = 0;
            bool lethal = false; // their share of the body's health ran out, or chat bought Wasted, so they die once they're back
        };
        // Script fiber only. Set from the moment the bodies swap until the streamer is back in their own.
        std::optional<Morph> g_morph;

        // Hidden in place: invisible, frozen, untouchable, and not something aim assist locks on to.
        void Stash(Ped ped, bool stashed)
        {
            SET_ENTITY_VISIBLE(ped, !stashed, false);
            SET_ENTITY_COLLISION(ped, !stashed, false);
            FREEZE_ENTITY_POSITION(ped, stashed);
            SET_ENTITY_INVINCIBLE(ped, stashed, false);
            SET_PED_CAN_BE_TARGETTED(ped, !stashed);
            SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(ped, stashed);
        }

        void DeleteBody(const Morph& m, Ped player)
        {
            Ped body = m.body;
            if (body == player || !DOES_ENTITY_EXIST(body) || GET_ENTITY_MODEL(body) != m.bodyModel) return;
            SET_ENTITY_AS_MISSION_ENTITY(body, true, true);
            DELETE_PED(&body);
        }

        // Runs on the first safe frame after a morph ends, so never while the streamer is dead or in
        // a cutscene. Returns false to be tried again.
        bool ChangeBack(EffectEngine& engine)
        {
            if (!g_morph) return true;
            const Morph m = *g_morph;
            const Player player = PLAYER_ID();
            const Ped current = PLAYER_PED_ID();
            const bool originalThere = m.original != current && DOES_ENTITY_EXIST(m.original) && GET_ENTITY_MODEL(m.original) == m.originalModel;

            if (GET_ENTITY_MODEL(current) != m.bodyModel)
            {
                // Someone else is playing: a save loaded, or the game switched characters. That ped is
                // left alone, and the old one, if it's still there, goes back to the game.
                if (originalThere)
                {
                    Ped original = m.original;
                    Stash(original, false);
                    SET_ENTITY_AS_NO_LONGER_NEEDED(&original);
                }
                DeleteBody(m, current);
                g_morph.reset();
                return true;
            }
            // Put back in mid-air, they'd fall without whatever parachute the body had. Not that it
            // matters if they're about to die.
            if (!m.lethal && IS_ENTITY_IN_AIR(current)) return false;

            if (!originalThere)
            {
                // The game took the streamer's own ped away. This brings their character back, but not
                // what they were carrying or wearing.
                REQUEST_MODEL(m.originalModel);
                if (!HAS_MODEL_LOADED(m.originalModel)) return false;
                log::Warn("The streamer's own ped was gone when a morph ended, so only their character's model was put back");
                SET_PLAYER_MODEL(player, m.originalModel); // this deletes the body
                SET_PED_DEFAULT_COMPONENT_VARIATION(PLAYER_PED_ID());
                SET_MODEL_AS_NO_LONGER_NEEDED(m.originalModel);
                g_morph.reset();
                if (m.lethal) SET_ENTITY_HEALTH(PLAYER_PED_ID(), 0, 0, 0);
                else engine.RetargetAttackers(PLAYER_PED_ID());
                return true;
            }

            if (IS_PED_IN_ANY_VEHICLE(current, false)) CLEAR_PED_TASKS_IMMEDIATELY(current);
            const Vector3 at = GET_ENTITY_COORDS(current, true);
            SET_ENTITY_COORDS_NO_OFFSET(m.original, at.x, at.y, at.z, false, false, false);
            SET_ENTITY_HEADING(m.original, GET_ENTITY_HEADING(current));
            Stash(m.original, false);
            CHANGE_PLAYER_PED(player, m.original, false, false);
            DeleteBody(m, m.original);
            g_morph.reset();
            if (m.lethal)
            {
                log::Info("The streamer changed back from the morph body to die as themselves");
                SET_ENTITY_HEALTH(m.original, 0, 0, 0);
            }
            else engine.RetargetAttackers(m.original);
            return true;
        }

        // The body starts with the streamer's health and armor (plus kMorphSpareHealth) and, unless it's
        // an animal, a copy of the weapon in their hands. Their own ped keeps what it had, damage and all,
        // unless the body's damage would have killed them. `a` is 1 for an animal.
        EffectResult TriggerPlayerMorph(EffectContext& c, int durationSeconds = 30)
        {
            if (g_morph) return Retry("the streamer hasn't changed back from the last one yet");
            if (IS_ENTITY_IN_AIR(c.ped) || IS_PED_SWIMMING(c.ped)) return Retry("the streamer isn't on solid ground");

            // Kept between tries, so a retry waits for the same body rather than drawing another.
            static int pick = -1;
            if (pick < 0)
            {
                // From a random start, the first body this game has.
                const size_t start = (size_t)c.engine.Random(0.0, (double)kMorphBodies.size() - 0.001);
                for (size_t i = 0; i < kMorphBodies.size() && pick < 0; ++i)
                    if (FirstModelInGame(kMorphBodies[(start + i) % kMorphBodies.size()].models)) pick = (int)((start + i) % kMorphBodies.size());
                if (pick < 0) return Fail("this game version doesn't have any of the bodies");
            }
            const MorphBody& option = kMorphBodies[(size_t)pick];
            const Hash model = FirstModelInGame(option.models);
            if (auto wait = NeedModels(c, { model }))
            {
                if (wait->kind == Kind::Fail) pick = -1;
                return *wait;
            }
            pick = -1;

            const Ped original = c.ped;
            Hash held = 0;
            GET_CURRENT_PED_WEAPON(original, &held, true);
            Ped body = CREATE_PED(option.animal ? kPedAnimal : kPedMission, model, c.pos.x, c.pos.y, c.pos.z, c.heading, false, false);
            SET_MODEL_AS_NO_LONGER_NEEDED(model);
            if (!body) return Fail("the game wouldn't spawn it here");

            SET_PED_DEFAULT_COMPONENT_VARIATION(body);
            SET_PED_MAX_HEALTH(body, GET_ENTITY_MAX_HEALTH(original) + kMorphSpareHealth);
            SET_ENTITY_HEALTH(body, GET_ENTITY_HEALTH(original) + kMorphSpareHealth, 0, 0);
            SET_PED_ARMOUR(body, GET_PED_ARMOUR(original));
            if (!option.animal && held && held != kWeaponUnarmed) GIVE_WEAPON_TO_PED(body, held, GET_AMMO_IN_PED_WEAPON(original, held), false, true);

            CHANGE_PLAYER_PED(c.player, body, false, false);
            SET_PED_RELATIONSHIP_GROUP_HASH(body, kPlayerGroup);
            SET_ENTITY_AS_MISSION_ENTITY(original, true, true); // so the game doesn't clear it away
            CLEAR_PED_TASKS_IMMEDIATELY(original);
            Stash(original, true);
            g_morph = Morph{ original, body, GET_ENTITY_MODEL(original), model };
            c.engine.RetargetAttackers(body);

            return StartTimed(c, durationSeconds, [](TimedEffect& t, double)
            {
                for (int input : kMorphBlockedInputs) DISABLE_CONTROL_ACTION(kControls, input, true);
                // An animal keeps to its own attack.
                if (t.a != 0.0)
                    for (int input : kInputsChangeWeapon) DISABLE_CONTROL_ACTION(kControls, input, true);
                // Their share of the body's health is gone, or chat bought Wasted, so it ends now, and they die as themselves.
                if (g_morph && t.ped == g_morph->body &&
                    (g_morph->lethal || GET_ENTITY_HEALTH(g_morph->body) - kMorphSpareHealth <= kDeadHealth))
                {
                    g_morph->lethal = true;
                    t.remaining = 0.0;
                }
            }, [](TimedEffect& t, bool)
            {
                // Dead or being busted in the body, they'd never be brought back, so this can't wait
                // for a safe moment. With the spare health, the body shouldn't die at all.
                if (g_morph && DOES_ENTITY_EXIST(g_morph->body))
                {
                    const bool dead = IS_ENTITY_DEAD(g_morph->body, false) != 0;
                    if (dead || IS_PLAYER_BEING_ARRESTED(PLAYER_ID(), true))
                    {
                        if (dead)
                        {
                            log::Warn("The morph body died outright, so the streamer is changed back straight away");
                            g_morph->lethal = true;
                        }
                        else log::Info("The streamer is being busted in the morph body, so they're changed back straight away");
                        if (ChangeBack(*t.engine)) return;
                    }
                }
                t.engine->WhenSafe(ChangeBack);
            }, option.animal ? 1.0 : 0.0);
        }

        EffectResult PlayerMorph(EffectContext& c) { return TriggerPlayerMorph(c); }

        // ── Wasted ──────────────────────────────────────────────────────────────

        // The game's own death and respawn, wherever the streamer is. It waits while the player is
        // invincible, as Plot Armor makes them. In a morph body, dying would leave the screen black
        // for good, so the morph ends and they die once they're back in their own.
        EffectResult Wasted(EffectContext& c)
        {
            if (GET_PLAYER_INVINCIBLE(c.player)) return Retry("the streamer is invincible right now");
            if (g_morph && c.ped == g_morph->body)
            {
                g_morph->lethal = true; // the morph's tick, or a change back still waiting, acts on this
                return Ok();
            }
            SET_ENTITY_HEALTH(c.ped, 0, 0, 0);
            return Ok();
        }

        // ── Alien abduction ─────────────────────────────────────────────────────

        constexpr const char* kAlienFx = "scr_rcbarry1"; // the Barry missions' particles, aliens included
        constexpr float kLiftSpeed = 3.5f;
        constexpr double kAliensAfter = 1.5, kFlashSeconds = 0.7, kFallSeconds = 20.0;
        constexpr int kRoofDown = 2, kParachuteOnBack = 0, kMarkerCylinder = 1, kBeamShadow = 1;
        constexpr int kWorldVehiclesObjects = 1 | 2 | 16;
        // Only walking: on the way down they can still steer a parachute.
        constexpr std::array<int, 11> kInputsWalk = { kInputSprint, kInputJump, 23, kInputMoveLR, kInputMoveUD, kInputMoveUpOnly,
                                                      kInputMoveDownOnly, kInputMoveLeftOnly, kInputMoveRightOnly, 36, 44 };

        // Script fiber only, and only one of these effects runs at a time.
        struct Abduction
        {
            Vector3 ground;    // where the beam picked them up
            float top = 0;     // the height it lets go at
            Hash alienModel = 0, alienWeapon = 0;
            std::array<Ped, 4> aliens{};
            bool aliensDone = false, parachute = false;
        };
        Abduction g_abduction;

        void DeleteAbductors(bool vanish)
        {
            for (Ped& alien : g_abduction.aliens)
            {
                if (!alien || !DOES_ENTITY_EXIST(alien)) { alien = 0; continue; }
                if (vanish && HAS_NAMED_PTFX_ASSET_LOADED(kAlienFx))
                {
                    const Vector3 p = GET_ENTITY_COORDS(alien, true);
                    USE_PARTICLE_FX_ASSET(kAlienFx);
                    START_PARTICLE_FX_NON_LOOPED_AT_COORD("scr_alien_disintegrate", p.x, p.y, p.z, 0.0f, 0.0f, 0.0f, 1.0f, false, false, false);
                }
                DELETE_PED(&alien);
            }
        }

        // A ring under the beam, every one aiming up at where it lets go.
        void SpawnAbductors(EffectEngine& engine)
        {
            Abduction& a = g_abduction;
            a.aliensDone = true;
            if (!a.alienModel) return;
            const int count = (int)engine.Random(3.0, 4.999);
            const double start = engine.Random(0.0, 6.283185307);
            for (int i = 0; i < count; ++i)
            {
                Vector3 p = RingPoint(a.ground, start + i * 6.283185307 / count, 3.5f);
                float ground = 0;
                if (GET_GROUND_Z_FOR_3D_COORD(p.x, p.y, p.z + 3.0f, &ground, false, false)) p.z = ground;
                const Ped alien = CREATE_PED(kPedMission, a.alienModel, p.x, p.y, p.z, HeadingToward(p, a.ground), false, false);
                if (!alien) continue;
                SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(alien, true);
                if (a.alienWeapon) GIVE_WEAPON_TO_PED(alien, a.alienWeapon, 100, false, true);
                TASK_AIM_GUN_AT_COORD(alien, a.ground.x, a.ground.y, a.top, -1, false, false);
                a.aliens[(size_t)i] = alien;
            }
            SET_MODEL_AS_NO_LONGER_NEEDED(a.alienModel);
        }

        // At the top: a flash, the aliens vanish, and the beam lets go. The bang is on the ground, so
        // it can't knock the streamer into a ragdoll that couldn't open a parachute.
        void LetGo(const TimedEffect& t)
        {
            if (HAS_NAMED_PTFX_ASSET_LOADED(kAlienFx))
            {
                USE_PARTICLE_FX_ASSET(kAlienFx);
                START_PARTICLE_FX_NON_LOOPED_AT_COORD("scr_alien_teleport", t.pos.x, t.pos.y, t.pos.z, 0.0f, 0.0f, 0.0f, 3.0f, false, false, false);
            }
            DeleteAbductors(true);
            const Vector3& g = g_abduction.ground;
            ADD_EXPLOSION(g.x, g.y, g.z, kExpBlimp, 0.0f, true, true, 1.5f, true); // loud and shaking, but invisible and harmless
            PLAY_SOUND_FRONTEND(-1, "ScreenFlash", "WastedSounds", true);
        }

        // Out of the vehicle first, popping the doors unless it's open to the sky. `phase` is 0 going up
        // and 1 coming down, `a` is the time spent in that phase, and `b` the longest the way up may take.
        EffectResult TriggerAlienAbduction(EffectContext& c, float liftHeight = 25.0f)
        {
            const Entity body = c.vehicle ? c.vehicle : c.ped;
            if (IS_ENTITY_IN_AIR(body)) return Retry("the streamer is in the air");
            if (!c.vehicle && IS_PED_SWIMMING(c.ped)) return Retry("the streamer is swimming");

            // The beam needs open sky to lift them through.
            const int test = START_EXPENSIVE_SYNCHRONOUS_SHAPE_TEST_LOS_PROBE(c.pos.x, c.pos.y, c.pos.z + 1.2f, c.pos.x, c.pos.y, c.pos.z + liftHeight + 2.0f,
                                                                              kWorldVehiclesObjects, body, 7);
            BOOL hit = false;
            Vector3 end, normal;
            Entity struck = 0;
            if (GET_SHAPE_TEST_RESULT(test, &hit, &end, &normal, &struck) == 2 && hit) return Retry("the streamer has a roof over their head");

            const Hash alienModel = FirstModelInGame({ "s_m_m_movalien_01", nullptr });
            if (alienModel)
                if (auto wait = NeedModels(c, { alienModel })) return *wait;
            REQUEST_NAMED_PTFX_ASSET(kAlienFx); // used only if it has loaded by the top

            if (c.vehicle)
            {
                const Hash model = GET_ENTITY_MODEL(c.vehicle);
                const bool open = IS_THIS_MODEL_A_BIKE(model) || IS_THIS_MODEL_A_BICYCLE(model) || IS_THIS_MODEL_A_QUADBIKE(model) || IS_THIS_MODEL_A_JETSKI(model) ||
                                  (IS_VEHICLE_A_CONVERTIBLE(c.vehicle, false) && GET_CONVERTIBLE_ROOF_STATE(c.vehicle) == kRoofDown);
                if (!open)
                    for (int door = 0; door < 4; ++door) SET_VEHICLE_DOOR_OPEN(c.vehicle, door, false, true);
                CLEAR_PED_TASKS_IMMEDIATELY(c.ped);
            }

            g_abduction = {};
            g_abduction.ground = c.pos;
            g_abduction.top = c.pos.z + liftHeight;
            g_abduction.alienModel = alienModel;
            g_abduction.alienWeapon = FirstWeaponInGame({ "weapon_raypistol", "weapon_pistol" });
            g_abduction.parachute = c.options.abductParachute;
            if (g_abduction.parachute) GIVE_WEAPON_TO_PED(c.ped, kParachute, 1, false, false);

            const double rise = liftHeight / kLiftSpeed + 3.0; // a little slack, in case something slows them
            return StartTimed(c, rise + kFallSeconds, [](TimedEffect& t, double dt)
            {
                Abduction& a = g_abduction;
                t.a += dt;
                for (int input : kInputsWalk) DISABLE_CONTROL_ACTION(kControls, input, true);

                if (t.phase == 0)
                {
                    // A turquoise spotlight from above, and a see-through column to show the beam itself.
                    const float reach = a.top - a.ground.z + 40.0f;
                    DRAW_SHADOWED_SPOT_LIGHT(t.pos.x, t.pos.y, a.top + 15.0f, 0.0f, 0.0f, -1.0f, 0, 255, 230, reach, 30.0f, 0.0f, 12.0f, 20.0f, kBeamShadow);
                    DRAW_MARKER(kMarkerCylinder, t.pos.x, t.pos.y, a.ground.z - 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 2.5f, 2.5f, reach - 20.0f,
                                0, 255, 230, 70, false, false, 2, false, nullptr, nullptr, false);
                    SET_ENTITY_VELOCITY(t.ped, 0.0f, 0.0f, kLiftSpeed);
                    if (!a.aliensDone && t.a >= kAliensAfter) SpawnAbductors(*t.engine);
                    if (t.pos.z < a.top && t.a < t.b) return;
                    LetGo(t);
                    t.phase = 1;
                    t.a = 0.0;
                    return;
                }

                if (t.a < kFlashSeconds)
                {
                    const int alpha = std::clamp((int)(255.0 * (1.0 - t.a / kFlashSeconds)), 0, 255);
                    DRAW_RECT(0.5f, 0.5f, 1.0f, 1.0f, 200, 255, 250, alpha, false);
                }
                const bool inAir = IS_ENTITY_IN_AIR(t.ped);
                if (a.parachute && inAir && GET_PED_PARACHUTE_STATE(t.ped) == kParachuteOnBack) FORCE_PED_TO_OPEN_PARACHUTE(t.ped);
                if (!inAir && t.a > 0.5) t.remaining = 0.0; // landed
            }, [](TimedEffect&, bool)
            {
                DeleteAbductors(false);
                if (g_abduction.alienModel) SET_MODEL_AS_NO_LONGER_NEEDED(g_abduction.alienModel);
            }, 0.0, rise);
        }

        EffectResult AlienAbduction(EffectContext& c) { return TriggerAlienAbduction(c); }

        EffectResult FiveStars(EffectContext& c)
        {
            if (GET_PLAYER_WANTED_LEVEL(c.player) >= 5) return Fail("the cops are already on it");
            SET_MAX_WANTED_LEVEL(5);
            SET_POLICE_IGNORE_PLAYER(c.player, false);
            SET_DISPATCH_COPS_FOR_PLAYER(c.player, true);
            for (int service : kPoliceDispatch) ENABLE_DISPATCH_SERVICE(service, true);
            SET_PLAYER_WANTED_LEVEL(c.player, 5, false);
            SET_PLAYER_WANTED_LEVEL_NOW(c.player, false);
            // Starts the search where they are, so any head start from earlier is gone.
            SET_PLAYER_WANTED_CENTRE_POSITION(c.player, c.pos.x, c.pos.y, c.pos.z);

            if (c.options.fedsClearSeconds <= 0) return Ok();
            return StartTimed(c, c.options.fedsClearSeconds, nullptr, [](TimedEffect& t, bool cancelled)
            {
                if (!cancelled) CLEAR_PLAYER_WANTED_LEVEL(t.player);
            });
        }
    }

    const std::vector<EffectDef>& AllEffects()
    {
        static const std::vector<EffectDef> effects = {
            // ── Nuisance ──────────────────────────────────────────────────────
            { "ragdoll",   "Banana Peel",      100,  Tier::Nuisance,    "The streamer eats pavement, bailing out of their vehicle first if they have to.", DirectCommand, Ragdoll },
            { "horn",      "Stuck Horn",       100,  Tier::Nuisance,    "Horn and alarm blare for 15 seconds.", NeedsVehicle, StuckHorn },
            { "fog",       "Pea Soup",         100,  Tier::Nuisance,    "Thick fog rolls in.", NoFlags, Fog },
            { "storm",     "Thunderstorm",     150,  Tier::Nuisance,    "Summons a thunderstorm.", NoFlags, Storm },
            { "snow",      "Blizzard",         150,  Tier::Nuisance,    "A blizzard in Los Santos. Sure, why not.", NoFlags, Blizzard },
            { "midnight",  "Midnight",         150,  Tier::Nuisance,    "Skips the clock to midnight.", RiskyInMission, Midnight },
            { "drunk",     "Day Drinking",     200,  Tier::Nuisance,    "15 seconds of stumbling, a swaying camera and backwards controls.", DirectCommand | NeedsHuman, Drunk },
            { "nohud",     "Blackout Visor",   200,  Tier::Nuisance,    "No minimap or HUD for 45 seconds.", NoFlags, NoHud },
            { "legday",    "Leg Day",          200,  Tier::Nuisance,    "No sprinting or jumping for 30 seconds.", NoFlags, LegDay },
            { "jumpscare", "Jump Scare",       300,  Tier::Nuisance,    "Hits at a random moment in the next 40 seconds. No warning.", Silent, JumpScare },
            // ── Disruptive ────────────────────────────────────────────────────
            { "flat",      "Flat Tires",       250,  Tier::Disruptive,  "Pops every tire on the current vehicle.", NeedsVehicle, FlatTires },
            { "butterfingers", "Butterfingers", 250, Tier::Disruptive,  "Drops the weapon in the streamer's hands.", NeedsOnFoot | RiskyInMission | NeedsHuman, Butterfingers },
            { "slowmo",    "Slow Motion",      250,  Tier::Disruptive,  "The whole world crawls for 20 seconds.", NoFlags, SlowMo },
            { "stall",     "Engine Trouble",   300,  Tier::Disruptive,  "The engine cuts out for 10 seconds.", NeedsVehicle, Stall },
            { "yeet",      "Yeet",             300,  Tier::Disruptive,  "Launches the streamer, or their car, into the air.", NoFlags, Yeet },
            { "jam",       "Jammed Gun",       300,  Tier::Disruptive,  "Can't shoot or punch for 20 seconds.", NoFlags, JammedGun },
            { "moon",      "Moon Gravity",     300,  Tier::Disruptive,  "Low gravity and huge jumps for 30 seconds.", NoFlags, MoonGravity },
            { "blackout",  "City Blackout",    300,  Tier::Disruptive,  "Every light in the city goes out for a minute.", NoFlags, Blackout },
            { "snitch",    "Snitch",           300,  Tier::Disruptive,  "Two wanted stars. Somebody talked.", NoFlags, Snitch },
            { "nobrakes",  "Cut Brake Lines",  350,  Tier::Disruptive,  "No brakes or handbrake for 20 seconds.", NeedsVehicle, NoBrakes },
            { "dogs",      "Bad Dogs",         350,  Tier::Disruptive,  "Three rottweilers want a word.", NoFlags, BadDogs },
            { "ice",       "Ice Tires",        350,  Tier::Disruptive,  "Almost no grip for 15 seconds.", NeedsVehicle | DirectCommand, IceTires },
            { "loadout",   "Weapon Roulette",  350,  Tier::Disruptive,  "Forces a ridiculous weapon into the streamer's hands, with no switching back for 5 seconds.", NeedsOnFoot | RiskyInMission | DirectCommand | NeedsHuman, WeaponRoulette },
            { "floorit",   "Stuck Throttle",   400,  Tier::Disruptive,  "The pedal sticks to the floor for 12 seconds. No brakes either.", NeedsVehicle, FloorIt },
            { "honk",      "Exploding Horn",   400,  Tier::Disruptive,  "For 25 seconds, the horn blows up whatever's in front of the car.", NeedsVehicle | DirectCommand, ExplodingHorn },
            { "eject",     "Ejector Seat",     450,  Tier::Disruptive,  "Fires the streamer straight up out of their vehicle.", NeedsVehicle | DirectCommand, Eject },
            { "morph",     "Who Am I?",        450,  Tier::Disruptive,  "For 30 seconds the streamer is an alien, a sasquatch, Impotent Rage, a mime, a chimp or a zombie. Also !whoami.", NeedsOnFoot | RiskyInMission | DirectCommand | NeedsHuman, PlayerMorph, "whoami" },
            // ── Painful ───────────────────────────────────────────────────────
            { "onfire",    "Hot Seat",         500,  Tier::Painful,     "Sets the streamer on fire, or their engine if they're driving.", NoFlags, HotSeat },
            { "tax",       "Tax Collector",    500,  Tier::Painful,     "Takes 10% of the current character's cash (at least $100).", NoFlags, TaxCollector },
            { "mob",       "Angry Mob",        600,  Tier::Painful,     "Six armed locals who've had enough.", NoFlags, AngryMob },
            { "carjack",   "Carjacked",        600,  Tier::Painful,     "A stranger pulls the streamer out and drives off with their car.", NeedsVehicle | RiskyInMission, Carjacked },
            { "pinball",   "Pinball",          600,  Tier::Painful,     "For 20 seconds, every hit bounces the streamer off it like a pinball.", DirectCommand, Pinball },
            { "speed",     "Lock Throttle",    600,  Tier::Painful,     "Top speed and no brakes for 12 seconds.", NeedsVehicle | DirectCommand, LockThrottle },
            // The swarms all run CharacterSwarm. What each one spawns is in kSwarms, above.
            { "zombies",   "Zombie Outbreak",  700,  Tier::Painful,     "Eight zombies close in from every side.", DirectCommand, CharacterSwarm },
            { "clowns",    "Clown Posse",      700,  Tier::Painful,     "Eight clowns with baseball bats close in from every side.", DirectCommand, CharacterSwarm },
            { "lesters",   "Lester Army",      700,  Tier::Painful,     "Eight Lesters with golf clubs.", DirectCommand, CharacterSwarm },
            { "lions",     "Mountain Lions",   800,  Tier::Painful,     "Two mountain lions. In the city, if need be.", NoFlags, MountainLions },
            { "wanted",    "Most Wanted",      800,  Tier::Painful,     "Four wanted stars.", NoFlags, MostWanted },
            { "downgrade", "Downgrade",        800,  Tier::Painful,     "Swaps the streamer's ride for a scooter at the same speed.", NeedsVehicle | RiskyInMission, Downgrade },
            { "freefall",  "Free Fall",        800,  Tier::Painful,     "Teleports the streamer 600m straight up, with a parachute.", RiskyInMission | NeedsHuman, FreeFall },
            { "aliens",    "Alien Invasion",   800,  Tier::Painful,     "Eight aliens with stun guns. Any still standing after 45 seconds go home.", DirectCommand, CharacterSwarm },
            { "cultists",  "Cult Following",   800,  Tier::Painful,     "Eight cultists with machetes.", DirectCommand, CharacterSwarm },
            { "clones",    "Clone Swarm",      900,  Tier::Painful,     "Ten clowns with machetes, golf clubs and nightsticks.", DirectCommand, CloneSwarm },
            { "abduct",    "Alien Abduction",  900,  Tier::Painful,     "A tractor beam lifts the streamer 25m while aliens watch from below, then lets go. Out of a car first, if need be.", DirectCommand | NeedsHuman, AlienAbduction },
            { "warp",      "Wrong Turn",       1000, Tier::Painful,     "Teleports the streamer somewhere inconvenient. Maybe a military base.", RiskyInMission, WrongTurn },
            { "swap",      "Vehicle Roulette", 1000, Tier::Painful,     "Swaps the streamer's ride mid-drive for a tractor, a blimp, a tank or worse. The old one is gone.", NeedsVehicle | RiskyInMission | DirectCommand, VehicleRoulette },
            { "sasquatch", "Sasquatch Sighting", 1000, Tier::Painful,   "Eight sasquatches. While any are standing, punches hit three times as hard.", DirectCommand, CharacterSwarm },
            { "astronauts","Space Cadets",     1000, Tier::Painful,     "Eight astronauts with Up-n-Atomizer ray guns.", DirectCommand, CharacterSwarm },
            { "hitsquad",  "Hit Squad",        1200, Tier::Painful,     "A truck of private military contractors with the streamer's name on a list.", NoFlags, HitSquad },
            { "madmax",    "Mad Max Traffic",  1500, Tier::Painful,     "For 20 seconds, nearby drivers try to ram the streamer.", RiskyInMission | DirectCommand, MadMaxTraffic },
            { "warzone",   "Warzone Rounds",   1500, Tier::Painful,     "For 20 seconds, everyone's bullets explode, the streamer's included.", DirectCommand, WarzoneRounds },
            { "feds",      "Five-Star Sting",  1500, Tier::Painful,     "Five wanted stars, right now.", DirectCommand, FiveStars },
            // ── Devastating ───────────────────────────────────────────────────
            { "carmageddon", "Carmageddon",    2000, Tier::Devastating, "For a minute, vehicles rain out of the sky in front of the streamer, 10 to 15 at a time.", DirectCommand, Carmageddon },
            { "flood",     "Flash Flood",      2000, Tier::Devastating, "A storm, and the water rises around the streamer for 30 seconds.", RiskyInMission | DirectCommand, FlashFlood },
            { "carbomb",   "Car Bomb",         2000, Tier::Devastating, "Six seconds of beeping, then the vehicle blows. Bail out!", NeedsVehicle | RiskyInMission, CarBomb },
            { "airstrike", "Airstrike",        2500, Tier::Devastating, "Eight seconds of rockets walking closer to the streamer.", NoFlags, Airstrike },
            { "tank",      "Army Surplus",     3000, Tier::Devastating, "A hostile tank rolls up.", NoFlags, ArmySurplus },
            { "airsupport","Air Support",      3500, Tier::Devastating, "An attack helicopter with door gunners.", NoFlags, AirSupport },
            { "kill",      "Wasted",           5000, Tier::Devastating, "The streamer drops dead on the spot. Plot Armor makes it wait. Also !wasted.", DirectCommand, Wasted, "wasted" },
            { "nuclear",   "Nuclear Chaos",    50000, Tier::Devastating, "The ultimate sabotage. A countdown, a blinding flash, then everything nearby is gone.", RiskyInMission, NuclearChaos },
            // ── Blessings ─────────────────────────────────────────────────────
            { "clearsky",  "Clear Skies",      100,  Tier::Blessing,    "Sunshine, and the weather goes back to normal.", NoFlags, ClearSkies },
            { "daybreak",  "Daybreak",         100,  Tier::Blessing,    "Skips the clock to noon.", RiskyInMission, Daybreak },
            { "heal",      "Med Kit",          200,  Tier::Blessing,    "Full health.", NoFlags, Heal },
            { "armor",     "Body Armor",       200,  Tier::Blessing,    "Full armor.", NoFlags, Armor },
            { "ammo",      "Ammo Drop",        200,  Tier::Blessing,    "250 rounds for the current weapon.", NeedsHuman, AmmoDrop },
            { "repair",    "Pit Stop",         250,  Tier::Blessing,    "Fully repairs and cleans the current vehicle.", NeedsVehicle, PitStop },
            { "cash",      "Stimulus Check",   250,  Tier::Blessing,    "$5,000 for the current character.", NoFlags, Stimulus },
            { "caffeine",  "Caffeine",         300,  Tier::Blessing,    "Runs faster and jumps absurdly high for 30 seconds.", NoFlags, Caffeine },
            { "clean",     "Clean Record",     400,  Tier::Blessing,    "Clears the wanted level.", NoFlags, CleanRecord },
            { "guns",      "Arms Dealer",      400,  Tier::Blessing,    "A pistol, an SMG and a carbine rifle, loaded.", NeedsHuman, ArmsDealer },
            { "car",       "Gift Car",         500,  Tier::Blessing,    "A supercar appears with the buyer's name on the plate.", NoFlags, GiftCar },
            { "plotarmor", "Plot Armor",       800,  Tier::Blessing,    "20 seconds of invincibility.", NoFlags, PlotArmor },
        };
        return effects;
    }

    const EffectDef* FindEffect(const std::string& id)
    {
        auto key = Lower(id);
        for (auto& e : AllEffects())
            if (key == e.id) return &e;
        return nullptr;
    }

    const EffectDef* FindDirectCommand(const std::string& word)
    {
        auto key = Lower(word);
        for (auto& e : AllEffects())
            if ((e.flags & DirectCommand) && (key == e.id || (e.alias && key == e.alias))) return &e;
        return nullptr;
    }

    const char* TierName(Tier t)
    {
        switch (t)
        {
        case Tier::Nuisance: return "Nuisance";
        case Tier::Disruptive: return "Disruptive";
        case Tier::Painful: return "Painful";
        case Tier::Devastating: return "Devastating";
        case Tier::Blessing: return "Blessing";
        }
        return "?";
    }
}
