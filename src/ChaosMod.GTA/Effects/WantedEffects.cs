using System;
using ChaosMod.Core.Abstractions;

namespace ChaosMod.GTA.Effects
{
    public sealed class CopPingEffect : EffectBase
    {
        public CopPingEffect() : base("wanted.cop_ping", "copping", "Cop Ping", "Instant 1-star wanted level.",
            EffectCategory.WantedLevel, EffectTier.Nuisance, 150, TimeSpan.FromSeconds(45)) { }

        public override EffectOutcome Execute(IGameContext ctx, EffectExecutionContext execCtx)
        {
            ctx.Player.SetWantedLevel(1, true);
            return EffectOutcome.Ok();
        }
    }

    public sealed class ThreeStarWantedEffect : EffectBase
    {
        public ThreeStarWantedEffect() : base("wanted.instant_3star", "3star", "3-Star Wanted", "Instant 3-star wanted level.",
            EffectCategory.WantedLevel, EffectTier.Painful, 400, TimeSpan.FromSeconds(90)) { }

        public override EffectOutcome Execute(IGameContext ctx, EffectExecutionContext execCtx)
        {
            ctx.Player.SetWantedLevel(3, true);
            return EffectOutcome.Ok();
        }
    }

    public sealed class FiveStarManhuntEffect : EffectBase
    {
        public FiveStarManhuntEffect() : base("wanted.5star_manhunt", "5star", "5-Star Manhunt", "Instant 5-star wanted level.",
            EffectCategory.WantedLevel, EffectTier.Devastating, 900, TimeSpan.FromSeconds(180)) { }

        public override EffectOutcome Execute(IGameContext ctx, EffectExecutionContext execCtx)
        {
            ctx.Player.SetWantedLevel(5, true);
            return EffectOutcome.Ok();
        }
    }
}
