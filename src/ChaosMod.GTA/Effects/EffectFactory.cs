using System.Collections.Generic;
using ChaosMod.Core.Abstractions;

namespace ChaosMod.GTA.Effects
{
    /// <summary>
    /// The full, fixed effect roster. Deliberately a manual list (no reflection
    /// scanning) — narrow and easy to scan, per the "narrow but polished" scope
    /// decision. Add new effects here as the roster grows.
    /// </summary>
    public static class EffectFactory
    {
        public static IEnumerable<IEffect> CreateAll()
        {
            // Wanted level
            yield return new CopPingEffect();
            yield return new ThreeStarWantedEffect();
            yield return new FiveStarManhuntEffect();

            // Vehicle
            yield return new BlowAllTyresEffect();
            yield return new KillEngineEffect();
            yield return new DetonateVehicleEffect();

            // Ped
            yield return new EveryoneFleesEffect();
            yield return new PedRiotEffect();
            yield return new AngryMobEffect();

            // Player status
            yield return new RagdollEffect();
            yield return new SetOnFireEffect();
            yield return new InvertControlsEffect();

            // Screen/camera
            yield return new DrunkCamEffect();
            yield return new EarthquakeShakeEffect();
            yield return new HideHudEffect();

            // Economy
            yield return new RobThePlayerEffect();
            yield return new BankruptEffect();

            // Weather/time
            yield return new InstantNightEffect();
            yield return new InstantStormEffect();
            yield return new SuddenBlizzardEffect();

            // Blessing
            yield return new FullHealArmorEffect();
            yield return new AmmoResupplyEffect();
            yield return new ClearWantedCashBonusEffect();

            // Ultimate (disabled by default)
            yield return new LosSantosMeltdownEffect();
        }
    }
}
