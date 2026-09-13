using System;
using ChaosMod.Core.Abstractions;

namespace ChaosMod.GTA.Effects
{
    public sealed class RobThePlayerEffect : EffectBase
    {
        public RobThePlayerEffect() : base("economy.rob", "rob", "Rob The Player", "Takes $5,000 from the active protagonist's cash.",
            EffectCategory.Economy, EffectTier.Painful, 300, TimeSpan.FromSeconds(60)) { }

        public override EffectOutcome Execute(IGameContext ctx, EffectExecutionContext execCtx)
        {
            ctx.Economy.AddPlayerCash(-5000);
            return EffectOutcome.Ok();
        }
    }

    public sealed class BankruptEffect : EffectBase
    {
        public BankruptEffect() : base("economy.bankrupt", "bankrupt", "Bankrupt", "Takes $25,000 from the active protagonist's cash.",
            EffectCategory.Economy, EffectTier.Devastating, 900, TimeSpan.FromSeconds(180)) { }

        public override EffectOutcome Execute(IGameContext ctx, EffectExecutionContext execCtx)
        {
            ctx.Economy.AddPlayerCash(-25000);
            return EffectOutcome.Ok();
        }
    }
}
