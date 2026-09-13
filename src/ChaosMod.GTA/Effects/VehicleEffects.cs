using System;
using ChaosMod.Core.Abstractions;

namespace ChaosMod.GTA.Effects
{
    public sealed class BlowAllTyresEffect : EffectBase
    {
        public BlowAllTyresEffect() : base("vehicle.blow_tyres", "poptires", "Blow All Tyres", "Bursts every tyre on the player's current vehicle.",
            EffectCategory.Vehicle, EffectTier.Nuisance, 150, TimeSpan.FromSeconds(30)) { }

        public override bool CanExecute(IGameContext ctx, out string failureReason)
        {
            if (!ctx.Player.IsInVehicle) { failureReason = "player is not in a vehicle"; return false; }
            failureReason = null;
            return true;
        }

        public override EffectOutcome Execute(IGameContext ctx, EffectExecutionContext execCtx)
        {
            if (!ctx.Vehicles.TryGetCurrentVehicle(out var vehicle))
                return EffectOutcome.Fail("player is not in a vehicle");
            ctx.Vehicles.BurstAllTyres(vehicle);
            return EffectOutcome.Ok();
        }
    }

    public sealed class KillEngineEffect : EffectBase
    {
        public KillEngineEffect() : base("vehicle.kill_engine", "killengine", "Kill Engine", "Destroys the current vehicle's engine and makes it undriveable.",
            EffectCategory.Vehicle, EffectTier.Disruptive, 250, TimeSpan.FromSeconds(45)) { }

        public override bool CanExecute(IGameContext ctx, out string failureReason)
        {
            if (!ctx.Player.IsInVehicle) { failureReason = "player is not in a vehicle"; return false; }
            failureReason = null;
            return true;
        }

        public override EffectOutcome Execute(IGameContext ctx, EffectExecutionContext execCtx)
        {
            if (!ctx.Vehicles.TryGetCurrentVehicle(out var vehicle))
                return EffectOutcome.Fail("player is not in a vehicle");
            ctx.Vehicles.KillEngine(vehicle);
            return EffectOutcome.Ok();
        }
    }

    public sealed class DetonateVehicleEffect : EffectBase
    {
        public DetonateVehicleEffect() : base("vehicle.detonate", "boomcar", "Detonate Vehicle", "Blows up the player's current vehicle.",
            EffectCategory.Vehicle, EffectTier.Painful, 700, TimeSpan.FromSeconds(120)) { }

        public override bool CanExecute(IGameContext ctx, out string failureReason)
        {
            if (!ctx.Player.IsInVehicle) { failureReason = "player is not in a vehicle"; return false; }
            failureReason = null;
            return true;
        }

        public override EffectOutcome Execute(IGameContext ctx, EffectExecutionContext execCtx)
        {
            if (!ctx.Vehicles.TryGetCurrentVehicle(out var vehicle))
                return EffectOutcome.Fail("player is not in a vehicle");
            ctx.Vehicles.Explode(vehicle);
            return EffectOutcome.Ok();
        }
    }
}
