# Chat vs Streamer — GTA V

A GTA V (Story Mode / Free Roam only — **never** GTA Online) ScriptHookVDotNet
mod porting the "Chat vs Streamer" concept: Twitch chat earns points by
watching/chatting/subs/bits/follows/raids/redemptions, then spends them via
`!buy <command>` on sabotages or blessings that hit your character live.

- `src/ChaosMod.Core` — game-agnostic points ledger, shop engine, Twitch
  integration, OAuth, overlay server, clip trigger.
- `src/ChaosMod.GTA` — the SHVDN mod itself: native-call effects, LemonUI menu.
- `src/ChaosMod.Tests` — unit tests (no GTA install required).
- `assets/overlay` — OBS browser-source overlay (Vinewood-neon themed).
- `docs/SETUP.md` — build, deploy, Twitch app registration, and first-run steps.
- `docs/ARCHITECTURE.md` — how it fits together and the roadmap (FiveM, YouTube Live).

Start with `docs/SETUP.md`.
