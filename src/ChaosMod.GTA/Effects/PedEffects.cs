using System;
using ChaosMod.Core.Abstractions;

namespace ChaosMod.GTA.Effects
{
    public sealed class EveryoneFleesEffect : EffectBase
    {
        public EveryoneFleesEffect() : base("ped.everyone_flees", "scatter", "Everyone Flees", "Nearby pedestrians panic and run from the player.",
            EffectCategory.Ped, EffectTier.Nuisance, 200, TimeSpan.FromSeconds(45)) { }

        public override EffectOutcome Execute(IGameContext ctx, EffectExecutionContext execCtx)
        {
            ctx.Peds.MakeNearbyPedsFlee(30f);
            return EffectOutcome.Ok();
        }
    }

    public sealed class PedRiotEffect : EffectBase
    {
        public PedRiotEffect() : base("ped.riot", "pedriot", "Ped Riot", "Spawns a handful of armed, hostile peds near the player.",
            EffectCategory.Ped, EffectTier.Painful, 500, TimeSpan.FromSeconds(90)) { }

        public override EffectOutcome Execute(IGameContext ctx, EffectExecutionContext execCtx)
        {
            ctx.Peds.SpawnHostilePedsAround(4, 15f);
            return EffectOutcome.Ok();
        }
    }

    public sealed class AngryMobEffect : EffectBase
    {
        public AngryMobEffect() : base("ped.angry_mob", "mob", "Angry Mob", "Spawns a larger wave of armed, hostile peds near the player.",
            EffectCategory.Ped, EffectTier.Devastating, 800, TimeSpan.FromSeconds(150)) { }

        public override EffectOutcome Execute(IGameContext ctx, EffectExecutionContext execCtx)
        {
            ctx.Peds.SpawnHostilePedsAround(8, 25f);
            return EffectOutcome.Ok();
        }
    }
}
