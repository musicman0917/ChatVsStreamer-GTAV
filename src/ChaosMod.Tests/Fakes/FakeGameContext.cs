using System.Numerics;
using ChaosMod.Core.Abstractions;

namespace ChaosMod.Tests.Fakes
{
    /// <summary>Minimal no-op IGameContext for exercising effect/shop logic without a real game.</summary>
    public sealed class FakeGameContext : IGameContext
    {
        public IPlayerFacade Player { get; } = new FakePlayerFacade();
        public IVehicleFacade Vehicles { get; } = new FakeVehicleFacade();
        public IPedFacade Peds { get; } = new FakePedFacade();
        public IWorldFacade World { get; } = new FakeWorldFacade();
        public ICameraFacade Camera { get; } = new FakeCameraFacade();
        public IEconomyFacade Economy { get; } = new FakeEconomyFacade();

        public void Log(string message) { }

        private sealed class FakePlayerFacade : IPlayerFacade
        {
            public bool IsInInterior { get; set; }
            public bool IsInVehicle { get; set; }
            public int Health { get; set; } = 100;
            public int MaxHealth { get; set; } = 100;
            public Vector3 Position { get; set; }
            public void SetHealth(int value) => Health = value;
            public void SetArmor(int value) { }
            public void SetWantedLevel(int stars, bool applyNow) { }
            public void ClearWantedLevel() { }
            public void GiveWeapon(uint weaponHash, int ammo, bool equipNow) { }
            public void SetCurrentWeaponAmmo(int ammo) { }
            public uint GetCurrentWeaponHash() => 1;
            public void SetOnFire() { }
            public void Ragdoll(int minMs, int maxMs) { }
            public void SetSuperJump(bool on) { }
            public void SetInfiniteAmmo(bool on) { }
            public void SetGravityScale(float scale) { }
            public void ApplyControlInversion(bool on) { }
            public void SetHudVisible(bool visible) { }
            public void SetRadarVisible(bool visible) { }
        }

        private sealed class FakeVehicleFacade : IVehicleFacade
        {
            public bool HasVehicle { get; set; }
            public bool TryGetCurrentVehicle(out IVehicleHandle vehicle) { vehicle = HasVehicle ? new FakeVehicleHandle() : null; return HasVehicle; }
            public bool TryGetNearestVehicle(float radius, out IVehicleHandle vehicle) { vehicle = HasVehicle ? new FakeVehicleHandle() : null; return HasVehicle; }
            public void BurstAllTyres(IVehicleHandle vehicle) { }
            public void KillEngine(IVehicleHandle vehicle) { }
            public void Explode(IVehicleHandle vehicle) { }
            public IVehicleHandle SpawnRandomVehicleNear(Vector3 position, float forwardOffset) => new FakeVehicleHandle();
            public void LockPlayerControls(bool locked) { }
        }

        private sealed class FakeVehicleHandle : IVehicleHandle { public bool IsValid => true; }

        private sealed class FakePedFacade : IPedFacade
        {
            public void MakeNearbyPedsFlee(float radius) { }
            public void SpawnHostilePedsAround(int count, float radius) { }
        }

        private sealed class FakeWorldFacade : IWorldFacade
        {
            public string Weather { get; set; } = "Clear";
            public void SetWeather(string weatherType, float transitionSeconds) => Weather = weatherType;
            public string GetCurrentWeather() => Weather;
            public void SetClockTime(int hour, int minute, int second) { }
            public void AddExplosion(Vector3 position, int explosionType, float cameraShake, bool isAudible, bool isInvisibleDamage) { }
        }

        private sealed class FakeCameraFacade : ICameraFacade
        {
            public void Shake(string shakeName, float amplitude) { }
            public void StopShake() { }
            public void SetFisheye(bool on) { }
        }

        private sealed class FakeEconomyFacade : IEconomyFacade
        {
            public long Cash { get; set; } = 10000;
            public long GetPlayerCash() => Cash;
            public void AddPlayerCash(long amount) => Cash += amount;
        }
    }
}
