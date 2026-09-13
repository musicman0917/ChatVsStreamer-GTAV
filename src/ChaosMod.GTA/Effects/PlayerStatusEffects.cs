using System;
using ChaosMod.Core.Abstractions;

namespace ChaosMod.GTA.Effects
{
    public sealed class RagdollEffect : EffectBase
    {
        public RagdollEffect() : base("player.ragdoll", "ragdoll", "Ragdoll", "Forces the player into a ragdoll for a few seconds.",
            EffectCategory.PlayerStatus, EffectTier.Disruptive, 200, TimeSpan.FromSeconds(40)) { }

        public override EffectOutcome Execute(IGameContext ctx, EffectExecutionContext execCtx)
        {
            ctx.Player.Ragdoll(3000, 3000);
            return EffectOutcome.Ok();
        }
    }

    public sealed class SetOnFireEffect : EffectBase
    {
        public SetOnFireEffect() : base("player.set_on_fire", "burn", "Set On Fire", "Sets the player on fire.",
            EffectCategory.PlayerStatus, EffectTier.Painful, 350, TimeSpan.FromSeconds(60)) { }

        public override EffectOutcome Execute(IGameContext ctx, EffectExecutionContext execCtx)
        {
            ctx.Player.SetOnFire();
            return EffectOutcome.Ok();
        }
    }

    /// <summary>
    /// Durational: registered with StatusEffectRunner on a successful Execute,
    /// re-applies the control inversion every frame for 20s, then reverts.
    /// </summary>
    public sealed class InvertControlsEffect : EffectBase, IDurationalEffect
    {
        public override TimeSpan Duration => TimeSpan.FromSeconds(20);

        public InvertControlsEffect() : base("player.invert_controls", "invert", "Invert Controls", "Inverts movement and look controls for 20 seconds.",
            EffectCategory.PlayerStatus, EffectTier.Painful, 450, TimeSpan.FromSeconds(90)) { }

        public override EffectOutcome Execute(IGameContext ctx, EffectExecutionContext execCtx)
        {
            ctx.Player.ApplyControlInversion(true);
            return EffectOutcome.Ok();
        }

        public void Tick(IGameContext ctx, TimeSpan elapsed) => ctx.Player.ApplyControlInversion(true);

        public void Revert(IGameContext ctx) => ctx.Player.ApplyControlInversion(false);
    }
}
