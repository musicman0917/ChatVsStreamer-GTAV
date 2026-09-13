using System.Numerics;

namespace ChaosMod.Core.Abstractions
{
    /// <summary>
    /// Game-agnostic facade over whatever engine is actually running the mod.
    /// The GTA/SHVDN implementation (GtaGameContext) is the only place that may
    /// call GTA.Native.Function.Call; everything in ChaosMod.Core talks to the
    /// game only through this interface so the ledger/shop/effect stack can be
    /// reused by a future FiveM front-end without modification.
    /// </summary>
    public interface IGameContext
    {
        IPlayerFacade Player { get; }
        IVehicleFacade Vehicles { get; }
        IPedFacade Peds { get; }
        IWorldFacade World { get; }
        ICameraFacade Camera { get; }
        IEconomyFacade Economy { get; }

        void Log(string message);
    }

    public interface IPlayerFacade
    {
        bool IsInInterior { get; }
        bool IsInVehicle { get; }
        int Health { get; }
        int MaxHealth { get; }
        Vector3 Position { get; }

        void SetHealth(int value);
        void SetArmor(int value);
        void SetWantedLevel(int stars, bool applyNow);
        void ClearWantedLevel();
        void GiveWeapon(uint weaponHash, int ammo, bool equipNow);
        void SetCurrentWeaponAmmo(int ammo);
        uint GetCurrentWeaponHash();
        void SetOnFire();
        void Ragdoll(int minMs, int maxMs);
        void SetSuperJump(bool on);
        void SetInfiniteAmmo(bool on);
        void SetGravityScale(float scale);
        void ApplyControlInversion(bool on);
        void SetHudVisible(bool visible);
        void SetRadarVisible(bool visible);
    }

    public interface IVehicleHandle
    {
        bool IsValid { get; }
    }

    public interface IVehicleFacade
    {
        bool TryGetCurrentVehicle(out IVehicleHandle vehicle);
        bool TryGetNearestVehicle(float radius, out IVehicleHandle vehicle);
        void BurstAllTyres(IVehicleHandle vehicle);
        void KillEngine(IVehicleHandle vehicle);
        void Explode(IVehicleHandle vehicle);
        IVehicleHandle SpawnRandomVehicleNear(Vector3 position, float forwardOffset);
        void LockPlayerControls(bool locked);
    }

    public interface IPedFacade
    {
        void MakeNearbyPedsFlee(float radius);
        void SpawnHostilePedsAround(int count, float radius);
    }

    public interface IWorldFacade
    {
        void SetWeather(string weatherType, float transitionSeconds);
        string GetCurrentWeather();
        void SetClockTime(int hour, int minute, int second);
        void AddExplosion(Vector3 position, int explosionType, float cameraShake, bool isAudible, bool isInvisibleDamage);
    }

    public interface ICameraFacade
    {
        void Shake(string shakeName, float amplitude);
        void StopShake();
        void SetFisheye(bool on);
    }

    public interface IEconomyFacade
    {
        long GetPlayerCash();
        void AddPlayerCash(long amount);
    }
}
