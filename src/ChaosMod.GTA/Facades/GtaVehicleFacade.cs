using System;
using System.Linq;
using System.Numerics;
using ChaosMod.Core.Abstractions;
using GTA;
using GTA.Native;

namespace ChaosMod.GTA.Facades
{
    public sealed class GtaVehicleFacade : IVehicleFacade
    {
        // A grab-bag of common models spanning slow/fast/absurd, for "spawn random vehicle".
        private static readonly string[] RandomModels =
        {
            "adder", "brioso", "blista", "bmx", "caddy", "dump", "rhino", "trash", "tractor", "duster"
        };

        public bool TryGetCurrentVehicle(out IVehicleHandle vehicle)
        {
            var current = Game.Player.Character.CurrentVehicle;
            if (current != null && current.Exists())
            {
                vehicle = new VehicleHandleWrapper(current);
                return true;
            }
            vehicle = null;
            return false;
        }

        public bool TryGetNearestVehicle(float radius, out IVehicleHandle vehicle)
        {
            var nearby = World.GetNearbyVehicles(Game.Player.Character.Position, radius)
                .Where(v => v != null && v.Exists())
                .OrderBy(v => v.Position.DistanceTo(Game.Player.Character.Position))
                .FirstOrDefault();

            if (nearby != null)
            {
                vehicle = new VehicleHandleWrapper(nearby);
                return true;
            }
            vehicle = null;
            return false;
        }

        public void BurstAllTyres(IVehicleHandle vehicle)
        {
            var veh = VehicleHandleWrapper.Unwrap(vehicle);
            if (veh == null || !veh.Exists()) return;
            for (var i = 0; i < 8; i++)
                Function.Call(Hash.SET_VEHICLE_TYRE_BURST, veh, i, true, 1000f);
        }

        public void KillEngine(IVehicleHandle vehicle)
        {
            var veh = VehicleHandleWrapper.Unwrap(vehicle);
            if (veh == null || !veh.Exists()) return;
            veh.EngineHealth = 0f;
            Function.Call(Hash.SET_VEHICLE_UNDRIVEABLE, veh, true);
        }

        public void Explode(IVehicleHandle vehicle)
        {
            var veh = VehicleHandleWrapper.Unwrap(vehicle);
            if (veh == null || !veh.Exists()) return;
            Function.Call(Hash.EXPLODE_VEHICLE, veh, true, false);
        }

        public IVehicleHandle SpawnRandomVehicleNear(Vector3 position, float forwardOffset)
        {
            var modelName = RandomModels[new Random().Next(RandomModels.Length)];
            var model = new Model(modelName);
            model.Request(1000);

            var gtaPos = new GTA.Math.Vector3(position.X, position.Y, position.Z);
            var spawnPos = gtaPos + Game.Player.Character.ForwardVector * forwardOffset;
            var vehicle = World.CreateVehicle(model, spawnPos);
            return vehicle != null ? new VehicleHandleWrapper(vehicle) : null;
        }

        public void LockPlayerControls(bool locked)
        {
            // Freezes the vehicle in place rather than disabling individual driving
            // controls, since that only needs a one-shot native rather than a
            // per-frame re-application. Not currently wired to any effect in the
            // initial roster; kept for future vehicle-chaos effects.
            var veh = Game.Player.Character.CurrentVehicle;
            if (veh == null || !veh.Exists()) return;
            veh.IsPositionFrozen = locked;
        }
    }
}
