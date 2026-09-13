# Chaos Mod — Setup Guide

This mod runs entirely inside **GTA V Story Mode / Free Roam** via ScriptHookVDotNet
(SHVDN). It never touches GTA Online — do not attempt to run it there; modding
Rockstar's live multiplayer service risks your account.

## 1. Prerequisites

- GTA V (Steam/Epic/Rockstar builds all work with SHVDN)
- [Script Hook V](http://www.dev-c.com/gtav/scripthookv/) + [ScriptHookVDotNet3](https://github.com/scripthookvdotnet/scripthookvdotnet/releases) installed and confirmed working (test with any existing SHVDN mod first)
- .NET Framework 4.8 Developer Pack (to build) — https://dotnet.microsoft.com/download/dotnet-framework/net48
- Visual Studio 2022 (or `dotnet build`/MSBuild from the CLI)
- A Twitch account for the channel, and optionally a **separate bot account** for chat (or reuse the broadcaster account as its own bot — "self-botting" is fine here)

## 2. Register a Twitch Developer application

This is the one step the mod can't automate — it needs your own Twitch login:

1. Go to https://dev.twitch.tv/console/apps and create a new application.
2. **OAuth Redirect URL**: `http://localhost:3800/callback` (or whatever port you configure in `[Twitch] OAuthRedirectPort` — it must match exactly).
3. **Category**: anything (e.g. "Application Integration").
4. **Client Type**: Public (there's no client secret involved — the mod uses Twitch's Implicit Grant flow).
5. Copy the generated **Client ID**.

## 3. Build

```bash
dotnet restore
dotnet build ChaosMod.sln -c Release
```

The `.csproj` files pin specific NuGet package versions (TwitchLib.*, LemonUI.SHVDN3,
ScriptHookVDotNet3, System.Data.SQLite.Core, System.Text.Json) that were current at
the time this project was written. If `dotnet restore` reports a version that no
longer exists, bump it to the latest available on nuget.org for that package —
none of this code depends on anything unusual to a specific patch version, except
where noted below (TwitchLib.EventSub.Websockets, which is still evolving).

`ChaosMod.GTA` builds as **x64** (GTA5.exe is a 64-bit process) — this is already set in its `.csproj`; don't override it to AnyCPU.

## 4. Deploy

Copy into your GTA V install's `scripts\` folder:

- `ChaosMod.GTA.dll`
- `ChaosMod.Core.dll`
- All `TwitchLib.*.dll`, `Microsoft.Extensions.*.dll` and other dependency DLLs from the build output
- `LemonUI.SHVDN3.dll`
- **`System.Data.SQLite.dll` AND the native `SQLite.Interop.dll`** — the interop DLL must sit directly in `scripts\`, not in an `x64\` subfolder; SHVDN does not reliably probe subfolders the way a normal app's bin-deployment would. If your build output put it under `runtimes\win-x64\native\` or an `x64\` folder, copy it up to `scripts\` directly.
- The `assets\overlay\` folder, copied to `scripts\ChaosMod\overlay\`

On first launch the mod creates `scripts\ChaosMod\config.ini` — edit it (or use the in-game menu, default key **F10**) to set:

```ini
[Twitch]
ClientId=<your Client ID from step 2>
OAuthRedirectPort=3800
ChannelName=<your twitch channel login, lowercase>
```

## 5. Authorize Twitch

In-game, press **F10** to open the menu, then:

1. **Authorize Bot Account** — opens your browser to Twitch's consent screen for chat scopes. Sign in as whichever account should post chat messages (can be the broadcaster account itself).
2. **Authorize Broadcaster Account** — same flow, but for the channel's own EventSub/Helix/clip permissions. Must be signed in as the broadcaster.
3. After both succeed, **restart the mod** (reload scripts, or restart the game) so it picks up the new tokens and connects.

**Important:** Twitch's Implicit Grant tokens never expire on a schedule but also never refresh automatically — if chat or EventSub silently stops working after a long time, re-run the relevant authorization from the menu.

If either step fails to open `http://localhost:<port>/`, see the URL ACL note below.

## 6. Windows URL ACL (HttpListener permissions)

Both the OAuth callback listener and the overlay web server bind a local port via `HttpListener`. On Windows, a non-admin process often can't bind a port without a one-time reservation. If you see "Access is denied" in `scripts\ChaosMod\chaosmod.log` or the menu, run once, **elevated** (Admin Command Prompt):

```
netsh http add urlacl url=http://localhost:3800/ user=Everyone
netsh http add urlacl url=http://localhost:8420/ user=Everyone
```

(adjust the ports to match your `OAuthRedirectPort` / `[Overlay] Port`).

## 7. OBS overlay

Add a **Browser Source** in OBS pointing at `http://localhost:8420/` (or whatever `[Overlay] Port` is set to). Recommended size 1920x1080. It shows the live shop list, chat feed, purchase/effect alerts, and a leaderboard, GTA/Vinewood-neon themed.

## 8. Testing

- `dotnet test src/ChaosMod.Tests/ChaosMod.Tests.csproj` runs the ledger/shop-engine unit tests against fakes — no GTA install needed for this part.
- Everything else (native calls, OAuth, overlay, clipping) can only be verified in-game. If something misbehaves — a wrong native call, a stat that silently no-ops after a game update, an OAuth redirect mismatch — check `scripts\ChaosMod\chaosmod.log` first.

## 9. Known rough edges to expect

- **PubSub is gone.** Twitch fully decommissioned it in 2025; this mod uses EventSub over WebSocket exclusively for subs/bits/follows/raids/redemptions. That library is still labeled "open beta" upstream — if it breaks after a TwitchLib update, check that package's own samples for the current event-arg shapes.
- **Per-protagonist cash stats** (`SP0/1/2_TOTAL_CASH`) are what the Economy effects read/write. If Rockstar ever renames these in a future title update, cash effects will silently no-op — that's the first thing to check.
- **The Ultimate effect (`ultimate.los_santos_meltdown`) is disabled by default.** Turn it on deliberately from the Effects submenu or `config.ini` once you're happy with how the rest of the mod behaves.
- Test this mod **alone** first before combining with other SHVDN mods — SHVDN3 loads every script into one shared AppDomain, and a bundled library version clash (most commonly `Newtonsoft.Json`) between two mods is a known failure mode.
