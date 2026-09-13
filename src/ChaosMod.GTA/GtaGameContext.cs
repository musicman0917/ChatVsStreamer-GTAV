using System;
using System.IO;
using ChaosMod.Core.Abstractions;
using ChaosMod.GTA.Facades;

namespace ChaosMod.GTA
{
    /// <summary>
    /// The single implementation of IGameContext, and the only class in the
    /// solution whose facades are allowed to call GTA.Native.Function.Call.
    /// Constructed once by ChaosModScript and only ever touched from OnTick.
    /// </summary>
    public sealed class GtaGameContext : IGameContext
    {
        public IPlayerFacade Player { get; } = new GtaPlayerFacade();
        public IVehicleFacade Vehicles { get; } = new GtaVehicleFacade();
        public IPedFacade Peds { get; } = new GtaPedFacade();
        public IWorldFacade World { get; } = new GtaWorldFacade();
        public ICameraFacade Camera { get; } = new GtaCameraFacade();
        public IEconomyFacade Economy { get; } = new GtaEconomyFacade();

        private readonly string _logPath;

        public GtaGameContext(string rootPath)
        {
            _logPath = Path.Combine(rootPath, "chaosmod.log");
        }

        // Routine/diagnostic logging goes to a plain text file rather than an
        // in-game toast, since Log() fires for every refund/connection error and
        // an on-screen notification per event would spam the player constantly.
        // User-facing outcomes (purchases, refunds) already go through chat.
        public void Log(string message)
        {
            try { File.AppendAllText(_logPath, $"[{DateTime.UtcNow:O}] {message}{Environment.NewLine}"); }
            catch { /* logging must never crash the mod */ }
        }
    }
}
