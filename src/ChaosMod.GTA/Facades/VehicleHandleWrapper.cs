using ChaosMod.Core.Abstractions;
using GTA;

namespace ChaosMod.GTA.Facades
{
    /// <summary>Wraps a live GTA.Vehicle so Core's IVehicleHandle stays engine-agnostic.</summary>
    public sealed class VehicleHandleWrapper : IVehicleHandle
    {
        public Vehicle Vehicle { get; }
        public bool IsValid => Vehicle != null && Vehicle.Exists();

        public VehicleHandleWrapper(Vehicle vehicle)
        {
            Vehicle = vehicle;
        }

        public static Vehicle Unwrap(IVehicleHandle handle) => (handle as VehicleHandleWrapper)?.Vehicle;
    }
}
