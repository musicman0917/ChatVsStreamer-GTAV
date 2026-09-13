using System;
using ChaosMod.Core.Abstractions;
using GTA;
using GTA.Native;

namespace ChaosMod.GTA.Facades
{
    /// <summary>
    /// Story-mode cash lives in per-protagonist stats (SP0/SP1/SP2_TOTAL_CASH for
    /// Michael/Franklin/Trevor), not a single global "player money" value, and
    /// there is no direct SHVDN property for it — it must go through
    /// STAT_GET_INT/STAT_SET_INT with the stat name's hash. If Rockstar ever
    /// renames these stats in a future title update, cash effects will silently
    /// no-op; that's the first thing to check if so.
    /// </summary>
    public sealed class GtaEconomyFacade : IEconomyFacade
    {
        private static string CurrentProtagonistCashStat()
        {
            var modelHash = (uint)Game.Player.Character.Model.Hash;

            if (modelHash == (uint)PedHash.Michael) return "SP0_TOTAL_CASH";
            if (modelHash == (uint)PedHash.Franklin) return "SP1_TOTAL_CASH";
            if (modelHash == (uint)PedHash.Trevor) return "SP2_TOTAL_CASH";

            // Fallback for online/other peds or a modified protagonist model: default to Michael's slot.
            return "SP0_TOTAL_CASH";
        }

        public long GetPlayerCash()
        {
            var statHash = Game.GenerateHash(CurrentProtagonistCashStat());
            var output = new OutputArgument();
            Function.Call(Hash.STAT_GET_INT, statHash, output, -1);
            return output.GetResult<int>();
        }

        public void AddPlayerCash(long amount)
        {
            var current = GetPlayerCash();
            var updated = Math.Max(0, current + amount);
            var statHash = Game.GenerateHash(CurrentProtagonistCashStat());
            Function.Call(Hash.STAT_SET_INT, statHash, (int)Math.Min(updated, int.MaxValue), true);
        }
    }
}
