using System;
using ChaosMod.Core.Abstractions;

namespace ChaosMod.GTA.Effects
{
    public sealed class FullHealArmorEffect : EffectBase
    {
        public FullHealArmorEffect() : base("blessing.full_heal", "heal", "Full Heal & Armor", "Restores full health and 100 armor.",
            EffectCategory.Blessing, EffectTier.Blessing, 300, TimeSpan.FromSeconds(60)) { }

        public override EffectOutcome Execute(IGameContext ctx, EffectExecutionContext execCtx)
        {
            ctx.Player.SetHealth(ctx.Player.MaxHealth);
            ctx.Player.SetArmor(100);
            return EffectOutcome.Ok();
        }
    }

    public sealed class AmmoResupplyEffect : EffectBase
    {
        public AmmoResupplyEffect() : base("blessing.ammo", "ammo", "Ammo Resupply", "Refills ammo for the player's current weapon.",
            EffectCategory.Blessing, EffectTier.Blessing, 250, TimeSpan.FromSeconds(60)) { }

        public override bool CanExecute(IGameContext ctx, out string failureReason)
        {
            if (ctx.Player.GetCurrentWeaponHash() == 0) { failureReason = "player is unarmed"; return false; }
            failureReason = null;
            return true;
        }

        public override EffectOutcome Execute(IGameContext ctx, EffectExecutionContext execCtx)
        {
            ctx.Player.SetCurrentWeaponAmmo(250);
            return EffectOutcome.Ok();
        }
    }

    public sealed class ClearWantedCashBonusEffect : EffectBase
    {
        public ClearWantedCashBonusEffect() : base("blessing.clear_wanted_cash", "clean", "Clear Wanted + Cash Bonus", "Clears wanted level and gives $2,000.",
            EffectCategory.Blessing, EffectTier.Blessing, 400, TimeSpan.FromSeconds(90)) { }

        public override EffectOutcome Execute(IGameContext ctx, EffectExecutionContext execCtx)
        {
            ctx.Player.ClearWantedLevel();
            ctx.Economy.AddPlayerCash(2000);
            return EffectOutcome.Ok();
        }
    }
}
