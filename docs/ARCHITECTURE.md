# Architecture

See the original design plan for full rationale. Summary:

- **ChaosMod.Core** — game-agnostic: points ledger (SQLite), shop engine, effect
  registry/queue, Twitch chat/EventSub/OAuth, overlay HttpListener/WebSocket
  server, clip trigger, ini config. Zero reference to ScriptHookVDotNet/GTA.
- **ChaosMod.GTA** — SHVDN entry point (`ChaosModScript`), `GtaGameContext` (the
  only class allowed to call `GTA.Native.Function.Call`), the concrete effect
  classes' native-call bodies, and the LemonUI menu.
- **ChaosMod.Tests** — xunit tests against Core, using fakes (no GTA/SQLite/network needed).

## The thread rule

SHVDN only allows `Function.Call` from the script's own `Tick` thread.
Chat/EventSub callbacks arrive on background threads. So: purchases are
validated and debited off-thread (pure SQL), then queued
(`EffectExecutionQueue`); only `ChaosModHost.Tick` (called from
`ChaosModScript.OnTick`) ever dequeues and calls `IEffect.CanExecute`/`Execute`.

## Roadmap seams (not built yet)

- **FiveM**: `IGameContext` is the reuse seam, but `Core`'s HttpListener/
  TwitchLib/SQLite stack likely needs to run server-side (FXServer) rather than
  in FiveM's sandboxed client Mono runtime, with thin RPCs to a client-side
  `IGameContext` implementation.
- **YouTube Live chat**: implement `IChatSource` with a polling
  `liveChatMessages.list` loop — no ledger/shop changes needed.

See `docs/SETUP.md` for build/deploy/testing instructions.
