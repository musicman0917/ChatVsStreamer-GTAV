// A stand-in for ScriptHookV.dll, built with the same declarations the mod links
// against. It runs the mod's script on a real Win32 fiber (as Script Hook V does),
// records every native call, and answers from a small simulated game state.
//
// It proves the plumbing, not the game: the real effect of a native is only ever
// checked by playing. See docs/IN_GAME_TEST.md.

#define CVS_SHV_API __declspec(dllexport)
#include "../src/Util.h"
#include "../src/shv/Natives.h"

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

using namespace cvs::nat::hash;
using cvs::Vector3;

namespace
{
    struct Call
    {
        uint64_t hash;
        std::vector<uint64_t> args;
        std::string text; // first string argument, copied
    };

    void (*g_scriptMain)() = nullptr;
    KeyboardHandler g_keyboard = nullptr;
    LPVOID g_hostFiber = nullptr;
    LPVOID g_scriptFiber = nullptr;

    uint64_t g_pending = 0;
    std::vector<uint64_t> g_pendingArgs;
    std::vector<Call> g_calls;
    uint64_t g_result[4]{};

    std::map<std::string, int> g_state;
    int g_nextHandle = 1000;
    std::map<uint32_t, int> g_modelRequests;
    std::map<int, int> g_entityBlips; // entity -> its blip
    std::set<int> g_liveBlips;
    std::map<int, uint32_t> g_entityModels; // what the mod created; anything else is player_one
    std::map<int, int> g_pedTypes;
    // Health the mod set on peds it created. Everyone else, the streamer's own ped included, has 150.
    std::map<int, int> g_entityHealth;

    int State(const char* key) { auto it = g_state.find(key); return it == g_state.end() ? 0 : it->second; }

    VOID CALLBACK FiberMain(LPVOID)
    {
        g_scriptMain();
        // A real script never returns; if this one did, hang here instead of ending the thread.
        while (true) SwitchToFiber(g_hostFiber);
    }

    bool IsString(uint64_t hash, size_t index)
    {
        if (index != 0) return false;
        switch (hash)
        {
        case BEGIN_TEXT_COMMAND_THEFEED_POST: case ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME:
        case SET_WEATHER_TYPE_NOW_PERSIST: case SET_WEATHER_TYPE_NOW: case SHAKE_GAMEPLAY_CAM:
        case ANIMPOSTFX_PLAY: case REQUEST_CLIP_SET: case HAS_CLIP_SET_LOADED: case ADD_RELATIONSHIP_GROUP:
        case REQUEST_NAMED_PTFX_ASSET: case HAS_NAMED_PTFX_ASSET_LOADED: case USE_PARTICLE_FX_ASSET: case START_PARTICLE_FX_NON_LOOPED_AT_COORD:
            return true;
        default:
            return false;
        }
    }

    void WriteVector(uint64_t* dest, float x, float y, float z)
    {
        Vector3 v;
        v.x = x; v.y = y; v.z = z;
        std::memcpy(dest, &v, sizeof(v));
    }

    void Answer(const Call& c)
    {
        std::memset(g_result, 0, sizeof(g_result));
        auto& a = c.args;
        auto setInt = [](int v) { g_result[0] = (uint64_t)(int64_t)v; };

        switch (c.hash)
        {
        case PLAYER_ID: setInt(0); break;
        // The player is ped 100 until the mod gives them another.
        case PLAYER_PED_ID: setInt(State("playerPed") ? State("playerPed") : 100); break;
        case CHANGE_PLAYER_PED: g_state["playerPed"] = (int)a[1]; break;
        case SET_PLAYER_MODEL:
            g_entityModels[g_nextHandle] = (uint32_t)a[1];
            g_state["playerPed"] = g_nextHandle++;
            break;
        case IS_PLAYER_PLAYING: setInt(!State("dead")); break;
        case IS_PLAYER_CONTROL_ON: setInt(!State("noControl")); break;
        case IS_PLAYER_BEING_ARRESTED: setInt(State("arrested")); break;
        case SET_PLAYER_INVINCIBLE: g_state["invincible"] = (int)a[1]; break;
        case GET_PLAYER_INVINCIBLE: setInt(State("invincible")); break;
        case IS_PLAYER_SWITCH_IN_PROGRESS: setInt(State("switching")); break;
        case GET_PLAYER_WANTED_LEVEL: setInt(State("wanted")); break;
        case GET_MISSION_FLAG: setInt(State("mission")); break;
        case IS_CUTSCENE_PLAYING: setInt(State("cutscene")); break;
        case GET_IS_LOADING_SCREEN_ACTIVE: setInt(State("loading")); break;
        case IS_SCREEN_FADED_OUT: setInt(0); break;
        case IS_PAUSE_MENU_ACTIVE: setInt(State("paused")); break;
        case NETWORK_IS_SESSION_STARTED: case NETWORK_IS_GAME_IN_PROGRESS: setInt(State("online")); break;
        case DOES_ENTITY_EXIST: setInt(!State("entitiesGone")); break;
        // The player is ped 100. "othersDead" kills everyone else.
        case IS_ENTITY_DEAD: setInt(State("dead") || ((int)a[0] != 100 && State("othersDead"))); break;
        case GET_ENTITY_COORDS: WriteVector(g_result, 10.0f, 20.0f, 30.0f + (float)State("raisedBy")); break;
        case GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS: WriteVector(g_result, 12.0f, 22.0f, 30.0f); break;
        case GET_ENTITY_HEADING: { float h = 90.0f; std::memcpy(g_result, &h, 4); break; }
        case GET_ENTITY_SPEED: { float s = 25.0f; std::memcpy(g_result, &s, 4); break; }
        case GET_ENTITY_MODEL:
        {
            auto it = g_entityModels.find((int)a[0]);
            setInt((int)(it == g_entityModels.end() ? cvs::Joaat("player_one") : it->second));
            break;
        }
        case GET_PED_TYPE: { auto it = g_pedTypes.find((int)a[0]); setInt(it == g_pedTypes.end() ? 1 : it->second); break; }
        // "othersHurt" takes that much off every ped the mod set the health of.
        case GET_ENTITY_HEALTH:
        {
            auto it = g_entityHealth.find((int)a[0]);
            setInt(it == g_entityHealth.end() ? 150 : it->second - State("othersHurt"));
            break;
        }
        case SET_ENTITY_HEALTH: if (g_entityModels.count((int)a[0])) g_entityHealth[(int)a[0]] = (int)a[1]; break;
        case GET_PED_ARMOUR: setInt(50); break;
        case GET_AMMO_IN_PED_WEAPON: setInt(120); break;
        case IS_ENTITY_IN_AIR: setInt(State("inAir")); break;
        case IS_PED_SWIMMING: setInt(State("swimming")); break;
        case GET_PED_PARACHUTE_STATE: setInt(State("parachuteState")); break;
        case HAS_NAMED_PTFX_ASSET_LOADED: setInt(1); break;
        case IS_THIS_MODEL_A_BIKE: setInt(State("bike")); break;
        case DELETE_PED: *reinterpret_cast<int*>(a[0]) = 0; break;
        case GET_ENTITY_MAX_HEALTH: setInt(200); break;
        case GET_PLAYER_MAX_ARMOUR: setInt(100); break;
        case IS_PED_IN_ANY_VEHICLE: setInt(State("inVehicle")); break;
        case GET_VEHICLE_PED_IS_IN: setInt(State("inVehicle") ? 200 : 0); break;
        case CREATE_PED: case CREATE_PED_INSIDE_VEHICLE: case CREATE_VEHICLE:
            if (State("spawnFails")) break;
            g_entityModels[g_nextHandle] = (uint32_t)a[c.hash == CREATE_VEHICLE ? 0 : c.hash == CREATE_PED ? 1 : 2];
            if (c.hash == CREATE_PED) g_pedTypes[g_nextHandle] = (int)a[0];
            setInt(g_nextHandle++);
            break;
        // "modelMissing" takes every model away, "missingModel" and "missingWeapon" one hash each.
        // "onlyModel" takes away all but one.
        case IS_MODEL_IN_CDIMAGE: case IS_MODEL_VALID:
            setInt(!State("modelMissing") && (uint32_t)a[0] != (uint32_t)State("missingModel") &&
                   (!State("onlyModel") || (uint32_t)a[0] == (uint32_t)State("onlyModel")));
            break;
        case IS_WEAPON_VALID: setInt((uint32_t)a[0] != (uint32_t)State("missingWeapon")); break;
        case ADD_BLIP_FOR_ENTITY:
            g_entityBlips[(int)a[0]] = g_nextHandle;
            g_liveBlips.insert(g_nextHandle);
            setInt(g_nextHandle++);
            break;
        case GET_BLIP_FROM_ENTITY: { auto it = g_entityBlips.find((int)a[0]); setInt(it == g_entityBlips.end() ? 0 : it->second); break; }
        case DOES_BLIP_EXIST: setInt(g_liveBlips.count((int)a[0]) ? 1 : 0); break;
        case REMOVE_BLIP:
            g_liveBlips.erase(*reinterpret_cast<int*>(a[0]));
            *reinterpret_cast<int*>(a[0]) = 0;
            break;
        case REQUEST_MODEL: g_modelRequests[(uint32_t)a[0]]++; break;
        case HAS_MODEL_LOADED: setInt(g_modelRequests[(uint32_t)a[0]] >= State("modelLoadRequests")); break;
        case HAS_CLIP_SET_LOADED: setInt(1); break;
        case GET_CURRENT_PED_WEAPON:
            *reinterpret_cast<uint32_t*>(a[1]) = State("unarmed") ? cvs::Joaat("weapon_unarmed") : cvs::Joaat("weapon_pistol");
            setInt(1);
            break;
        case STAT_GET_INT:
            *reinterpret_cast<int*>(a[1]) = State("cash");
            setInt(1);
            break;
        case STAT_SET_INT: g_state["cash"] = (int)a[1]; setInt(1); break;
        case ADD_RELATIONSHIP_GROUP:
            *reinterpret_cast<uint32_t*>(a[1]) = 0xABCD1234;
            setInt(1);
            break;
        case GET_SAFE_COORD_FOR_PED: setInt(0); break; // forces the fallback path
        case GET_GROUND_Z_FOR_3D_COORD:
            *reinterpret_cast<float*>(a[3]) = 29.0f;
            setInt(1);
            break;
        case GET_CLOSEST_VEHICLE_NODE_WITH_HEADING: setInt(0); break;
        case IS_PED_RAGDOLL: setInt(State("ragdolling")); break;
        case GET_ENTITY_VELOCITY:
            if (!State("stopped")) WriteVector(g_result, 8.0f, 6.0f, 0.0f);
            break;
        case GET_ENTITY_ROTATION: WriteVector(g_result, 1.0f, 2.0f, 90.0f); break;
        case HAS_ENTITY_COLLIDED_WITH_ANYTHING: setInt(State("collided")); break;
        case GET_COLLISION_NORMAL_OF_LAST_HIT_FOR_ENTITY:
            // As measured in the real game, this points into what was hit, not back out of it.
            if (State("floorHit")) WriteVector(g_result, 0.0f, 0.0f, -1.0f);
            else if (State("ceilingHit")) WriteVector(g_result, 0.0f, 0.0f, 1.0f);
            else WriteVector(g_result, 1.0f, 0.0f, 0.0f);
            break;
        case GET_ENTITY_FORWARD_VECTOR: WriteVector(g_result, 0.0f, 1.0f, 0.0f); break;
        case GET_GAMEPLAY_CAM_ROT: WriteVector(g_result, 0.0f, 0.0f, (float)State("camYaw")); break;
        case START_EXPENSIVE_SYNCHRONOUS_SHAPE_TEST_LOS_PROBE: setInt(77); break;
        case GET_SHAPE_TEST_RESULT:
            *reinterpret_cast<int*>(a[1]) = State("wallAhead");
            // As measured in the real game, unlike the collision normal, this points back out of the surface.
            WriteVector(reinterpret_cast<uint64_t*>(a[3]), -1.0f, 0.0f, 0.0f);
            setInt(2);
            break;
        case GET_DISABLED_CONTROL_NORMAL:
        {
            const float v = State(("held" + std::to_string((int)a[1])).c_str()) ? 1.0f : 0.0f;
            std::memcpy(g_result, &v, 4);
            break;
        }
        case IS_CONTROL_PRESSED: setInt(State("hornHeld")); break;
        case GET_VEHICLE_ESTIMATED_MAX_SPEED: { float v = 40.0f; std::memcpy(g_result, &v, 4); break; }
        case IS_VEHICLE_A_CONVERTIBLE: setInt(State("convertible")); break;
        case IS_VEHICLE_SEAT_FREE: setInt(State("noDrivers")); break;
        case GET_PED_IN_VEHICLE_SEAT: setInt((int)a[0] + 200); break;
        // "pedsHostile" puts the world's peds in the mod's hostile group.
        case GET_PED_RELATIONSHIP_GROUP_HASH: setInt(State("pedsHostile") ? (int)0xABCD1234 : 0x1234); break;
        case IS_PED_SHOOTING: setInt(State("pedsShooting")); break;
        case GET_PED_LAST_WEAPON_IMPACT_COORD:
            if (State("pedsShooting"))
            {
                WriteVector(reinterpret_cast<uint64_t*>(a[1]), 50.0f, 60.0f, (float)State("impactZ"));
                setInt(1);
            }
            break;
        case HAS_PED_GOT_WEAPON: setInt(State("ownsWeapon")); break;
        case GET_WATER_HEIGHT:
            *reinterpret_cast<float*>(a[3]) = 0.0f;
            setInt(1);
            break;
        case IS_ENTITY_IN_WATER: setInt(State("inWater")); break;
        default: break;
        }
    }
}

// ── The real Script Hook V interface ──────────────────────────────────────────

void scriptRegister(HMODULE, void (*scriptMain)()) { g_scriptMain = scriptMain; }
void scriptUnregister(HMODULE) { g_scriptMain = nullptr; }
void scriptWait(DWORD) { SwitchToFiber(g_hostFiber); }
void keyboardHandlerRegister(KeyboardHandler handler) { g_keyboard = handler; }
void keyboardHandlerUnregister(KeyboardHandler) { g_keyboard = nullptr; }

void nativeInit(UINT64 hash)
{
    g_pending = hash;
    g_pendingArgs.clear();
}

void nativePush64(UINT64 value) { g_pendingArgs.push_back(value); }

PUINT64 nativeCall()
{
    Call call{ g_pending, g_pendingArgs, {} };
    if (!call.args.empty() && IsString(call.hash, 0) && call.args[0])
        call.text = reinterpret_cast<const char*>(call.args[0]);
    else if (call.hash == SET_VEHICLE_NUMBER_PLATE_TEXT)
        call.text = reinterpret_cast<const char*>(call.args[1]);
    Answer(call);
    g_calls.push_back(std::move(call));
    return g_result;
}

int worldGetAllVehicles(int* arr, int arrSize)
{
    if (arrSize < 2) return 0;
    arr[0] = 501;
    arr[1] = 502;
    return 2;
}

int worldGetAllPeds(int* arr, int arrSize)
{
    if (arrSize < 2) return 0;
    arr[0] = 601;
    arr[1] = 602;
    return 2;
}

eGameVersion getGameVersion() { return (eGameVersion)-1; }

// ── Test controls ─────────────────────────────────────────────────────────────

extern "C"
{
    __declspec(dllexport) void mock_init()
    {
        g_hostFiber = ConvertThreadToFiber(nullptr);
    }

    __declspec(dllexport) void mock_frame()
    {
        if (!g_scriptMain) return;
        if (!g_scriptFiber) g_scriptFiber = CreateFiber(0, FiberMain, nullptr);
        SwitchToFiber(g_scriptFiber);
    }

    // What Script Hook V does when the game reloads: the script starts over from the top.
    __declspec(dllexport) void mock_restart_script()
    {
        if (g_scriptFiber) DeleteFiber(g_scriptFiber);
        g_scriptFiber = nullptr;
    }

    __declspec(dllexport) void mock_set(const char* key, int value) { g_state[key] = value; }
    __declspec(dllexport) int mock_get(const char* key) { return State(key); }
    __declspec(dllexport) void mock_clear_calls() { g_calls.clear(); }
    __declspec(dllexport) void mock_clear_models() { g_modelRequests.clear(); }

    __declspec(dllexport) int mock_count(uint64_t hash)
    {
        int n = 0;
        for (auto& c : g_calls) if (c.hash == hash) ++n;
        return n;
    }

    __declspec(dllexport) int mock_total_calls() { return (int)g_calls.size(); }

    // How many calls to `hash` had this integer as argument `index`.
    __declspec(dllexport) int mock_count_arg(uint64_t hash, int index, int value)
    {
        int n = 0;
        for (auto& c : g_calls)
            if (c.hash == hash && index < (int)c.args.size() && (int)c.args[(size_t)index] == value) ++n;
        return n;
    }

    // True if the most recent call to `hash` had this integer as argument `index`.
    __declspec(dllexport) int mock_last_arg_int(uint64_t hash, int index, int* out)
    {
        for (auto it = g_calls.rbegin(); it != g_calls.rend(); ++it)
        {
            if (it->hash != hash || index >= (int)it->args.size()) continue;
            *out = (int)it->args[(size_t)index];
            return 1;
        }
        return 0;
    }

    __declspec(dllexport) int mock_last_arg_float(uint64_t hash, int index, float* out)
    {
        for (auto it = g_calls.rbegin(); it != g_calls.rend(); ++it)
        {
            if (it->hash != hash || index >= (int)it->args.size()) continue;
            std::memcpy(out, &it->args[(size_t)index], 4);
            return 1;
        }
        return 0;
    }

    // Argument `index` of call number `nth` to `hash`, counting from 0 in the order they were made.
    __declspec(dllexport) int mock_nth_arg_float(uint64_t hash, int nth, int index, float* out)
    {
        for (auto& c : g_calls)
        {
            if (c.hash != hash || index >= (int)c.args.size() || nth-- > 0) continue;
            std::memcpy(out, &c.args[(size_t)index], 4);
            return 1;
        }
        return 0;
    }

    // What the game would act on for a control that's being fed. As measured in the real game,
    // blocking a control throws away what's fed to it, which leaves the player frozen.
    __declspec(dllexport) float mock_fed_control(int input)
    {
        float value = 0.0f;
        for (auto& c : g_calls)
        {
            if (c.args.size() < 2 || (int)c.args[1] != input) continue;
            if (c.hash == DISABLE_CONTROL_ACTION) return 0.0f;
            if (c.hash == SET_CONTROL_VALUE_NEXT_FRAME) std::memcpy(&value, &c.args[2], 4);
        }
        return value;
    }

    // Copies the most recent string argument seen for `hash`.
    __declspec(dllexport) int mock_last_text(uint64_t hash, char* out, int size)
    {
        for (auto it = g_calls.rbegin(); it != g_calls.rend(); ++it)
        {
            if (it->hash != hash) continue;
            strncpy_s(out, (size_t)size, it->text.c_str(), _TRUNCATE);
            return 1;
        }
        return 0;
    }

    // Counts every recorded native except the ones the engine polls each frame, which
    // says whether anything actually happened to the game.
    __declspec(dllexport) int mock_count_actions()
    {
        int n = 0;
        for (auto& c : g_calls)
        {
            switch (c.hash)
            {
            case PLAYER_ID: case PLAYER_PED_ID: case IS_PLAYER_PLAYING: case IS_PLAYER_CONTROL_ON:
            case IS_PLAYER_BEING_ARRESTED: case IS_PLAYER_SWITCH_IN_PROGRESS: case IS_CUTSCENE_PLAYING:
            case GET_IS_LOADING_SCREEN_ACTIVE: case IS_SCREEN_FADED_OUT: case IS_PAUSE_MENU_ACTIVE:
            case NETWORK_IS_SESSION_STARTED: case NETWORK_IS_GAME_IN_PROGRESS: case IS_ENTITY_DEAD:
            case GET_ENTITY_COORDS: case GET_ENTITY_HEADING: case IS_PED_IN_ANY_VEHICLE:
            case GET_VEHICLE_PED_IS_IN: case GET_MISSION_FLAG: case DOES_ENTITY_EXIST:
                break;
            default:
                ++n;
            }
        }
        return n;
    }

    __declspec(dllexport) void mock_key(DWORD key)
    {
        if (g_keyboard) g_keyboard(key, 1, 0, FALSE, FALSE, FALSE, FALSE);
    }

    // The RAGE Plugin Hook build has no Script Hook V to call, so TestHost stands in for its
    // C# plugin and forwards calls through these. The simulated game behind them is the same.
    __declspec(dllexport) PUINT64 mock_call(uint64_t hash, const uint64_t* args, int count)
    {
        nativeInit(hash);
        for (int i = 0; i < count; ++i) nativePush64(args[i]);
        return nativeCall();
    }

    __declspec(dllexport) int mock_vehicles(int* arr, int arrSize) { return worldGetAllVehicles(arr, arrSize); }
    __declspec(dllexport) int mock_peds(int* arr, int arrSize) { return worldGetAllPeds(arr, arrSize); }
}
