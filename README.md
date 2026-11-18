# Chat vs Streamer for GTA V

A GTA V story mode mod that lets your Twitch viewers earn chaos points by watching and chatting, then spend them to sabotage (or occasionally bless) you in real time. Sister project to [Chat vs Streamer](https://github.com/musicman0917/SDVChatVsStreamer) for Stardew Valley, plus the Skyrim and Witcher 3 versions.

Built by [NeighborhoodofMusic](https://twitch.tv/neighborhoodofmusic).

**For viewers:** every chat command, effect and default price is on [The Chaos Dispatch](https://musicman0917.github.io/ChatVsStreamer-GTAV/).

> **Status: most effects working in the real game.** On Enhanced `1.0.1158.16` through RAGE Plugin Hook, 49 of the 64 effects have been seen working (6 November 2026), fired from the control panel's interface. The 12 newest and the 3 reworked ones (`ragdoll`, `eject`, `drunk`) haven't been tried in-game, and neither have the guard rails in the checklist. The mod has joined real Twitch chat, but a real purchase hasn't been confirmed. The Script Hook V build has never run in the real game. Everything except GTA V itself has automated tests. [docs/IN_GAME_TEST.md](docs/IN_GAME_TEST.md) tracks what's confirmed.

## Which version of GTA V?

The mod reaches the game one of two ways. Both run the same code and call the same natives, each checked against both Legacy and Enhanced.

| | Script Hook V | RAGE Plugin Hook (RPH) |
|---|---|---|
| Use it when | [its page](https://www.dev-c.com/gtav/scripthookv/) lists your exact game version | it doesn't |
| The mod is | `ChatVsStreamerGTAV.asi` | a small .NET plugin that loads `ChatVsStreamerCore.dll` |
| Extra steps | none | see [Install with RAGE Plugin Hook](#install-with-rage-plugin-hook) |

Find your version at the top of the installer's output, or under Properties → Details on `GTA5_Enhanced.exe` or `GTA5.exe`. [modlag.com](https://modlag.com/gta5/enhanced) also tracks Script Hook V's support.

As of 6 November 2026, Script Hook V's latest release (15 July 2026) lists Legacy `1.0.3889.0` and Enhanced `1.0.1158.13`, while Enhanced from the Rockstar launcher is at `1.0.1158.16`. So Legacy uses Script Hook V, and Enhanced from the launcher needs RPH. The copy of RPH bundled with [LSPDFR's Enhanced Preview](https://www.lcpdfr.com/downloads/gta5mods/g17media/52970-lspd-first-response-enhanced-preview/) supports `1.0.1158.16`.

## Story mode only

The mod refuses to run effects in an online session. Rockstar bans for modding online, so before you play GTA Online, take the mod and its hook out of the game folder and turn BattlEye back on. For Script Hook V, which **closes the game if you go into GTA Online**, delete `ChatVsStreamerGTAV.asi` and Script Hook V's files. For RPH, run `rph-files.ps1 -Remove`.

## How it works

```
Twitch chat ──wss──▶ the mod ──natives──▶ GTA V
(IRC over WebSocket)    │    (through Script Hook V or RPH)
                        │
                        └──http://localhost:7373──▶ OBS overlay + control panel
```

The mod owns Twitch chat, the points ledger, the purchase queue and the web server. It never hooks or patches game memory itself: every effect is a sequence of native calls through the hook, so a game patch only needs a new build of the hook, not of this mod.

Natives only ever run on the hook's script fiber, and that fiber never waits on the network or disk. `Mod.h` has the threading rules.

Under RPH, which only loads .NET plugins, [a thin C# plugin](src/rph/plugin/ChatVsStreamerPlugin.cs) loads the C++ core, ticks it once per frame and forwards its native calls. Everything else is shared.

## Install

1. Install [Script Hook V](https://www.dev-c.com/gtav/scripthookv/): copy `ScriptHookV.dll` and `dinput8.dll` from its `bin` folder next to the game's exe. It has to support your exact game version, so after a game patch, wait for a new release.
2. Double-click `RunMe.bat` in the release folder. It finds the game, copies the plugin and asks for your Twitch details. If Script Hook V doesn't list your version, it says so, and offers to set the mod up through RAGE Plugin Hook instead when it can. (The default Rockstar folder needs **Run as administrator**.)
3. If mods won't load in story mode, turn BattlEye off in the Rockstar Games Launcher's settings for GTA V.
4. Start story mode, then add an OBS browser source: `http://localhost:7373/` at 1920×1080.

Your token, settings, points and log are in `%LOCALAPPDATA%\ChatVsStreamerGTAV`, so they survive reinstalls and don't sit in the game folder.

### Install with RAGE Plugin Hook

RPH's readme forbids redistributing it, so it isn't included, and you download it yourself. `RunMe.bat` does the rest with you.

1. Download LSPDFR's Enhanced Preview manual install (`lspdfr_049_9695_Manual_Install_Enhanced_1158_16.zip`) and leave it zipped in your Downloads folder. RPH comes inside it. That exact zip is the only one the installer accepts, and its copy of RPH was built for game version `1.0.1158.16`.
2. Turn BattlEye off in the Rockstar Games Launcher's settings for GTA V.
3. Double-click `RunMe.bat`. When Script Hook V doesn't list your game version, it offers to use RPH, then:
   - copies only RPH out of the zip, leaving LSPDFR itself out of your game
   - adds the `skipdscheck` file if the game is on an NVMe drive, where RPH otherwise stops with a DirectStorage error and closes the game
   - asks how the game should get to write to its own folder, if it can't already (see below)
   - adds the plugin instead of the `.asi`
4. Start the game with `StartGame.bat` every time, **not the launcher's Play button or `RAGEPluginHook.exe`**, and add the OBS source as above.

Each step is skipped once it's done, so running `RunMe.bat` again, to update the mod for example, only does what's left. If the zip isn't there yet, it says where to get it and waits. `install.ps1 -Rph` chooses RPH whatever Script Hook V's page says, and `-RphZip <path>` points it at a zip somewhere else. Each step is also a command of its own in `rph-files.ps1` (`-Install`, `-SkipDsCheck`, `-AllowWrite`, `-Mod`), if you'd rather run them yourself. For any other game version, add a copy of RPH that supports it yourself, and `RunMe.bat` will use it.

RPH writes into the game folder from inside the game, and Program Files only lets admins do that. The installer asks which of these you want:

- **Recommended: give your account write access** (`rph-files.ps1 -AllowWrite`). Nothing then needs admin rights, and `StartGame.bat` just works. The cost is that any program running under your account could then change the game's files, so never start RPH or the game with Run as administrator after this. `-RevokeWrite` undoes it, and so does `-Remove`.
- **Keep the folder as it is.** `StartGame.bat` asks you to quit the launcher, then asks for permission, because the game only gets admin rights if RPH opens the launcher itself. Everything in the game then runs as admin, including the code that reads Twitch chat, so this isn't recommended.

On an NVMe drive, the game holds open every file already in its folder. If RPH's log from the last run is still there, RPH can't open it and the game crashes with a misleading `Could not hook game process`. `StartGame.bat` moves that log to the data folder first, as `RagePluginHook.previous.log`.

**Smart App Control:** this Windows feature can refuse to load an unsigned file, including a build of this mod. If `RagePluginHook.log` shows Windows error 4551 for `ChatVsStreamerCore.dll`, that's what happened.

## Chat commands

| Command | |
|---|---|
| `!balance` / `!points` | Your chaos points |
| `!shop` / `!shop <tier>` | The tiers, or everything in one tier with prices |
| `!buy <name>` | Spend points on an effect |
| `!carmageddon` `!ragdoll` `!ice` `!eject` `!pinball` `!drunk` `!swap` `!speed` `!honk` `!madmax` `!flood` `!warzone` `!loadout` `!clones` `!feds` `!morph` `!abduct` `!kill` | Shortcuts for `!buy` with those effects, at the same price. `!whoami` works for `!morph` too, and `!wasted` for `!kill` |
| `!zombies` `!clowns` `!lesters` `!aliens` `!cultists` `!sasquatch` `!astronauts` | The same, for the swarms |
| `!info <name>` | What an effect does |
| `!give <user> <amount>` | Transfer points (mods mint them instead) |
| `!cvs on` / `off` / `clear` / `status` | Mods only: pause, resume, or clear and refund the queue |

Points come from chatting (with a cooldown), a passive payout for recent chatters (subs earn 1.5×, 2× or 3×), subs, gift subs, bits and raids. Raids of 5 or more also send a hostile mob: one attacker per five raiders, between 3 and 12.

The overlay's leaderboard leaves out your own channel, the mod's bot account, any name with "bot" in it, and the accounts in `leaderboard_hidden.txt` in the data folder. That file starts with common chat bots, such as StreamElements. Twitch lists your channel's bots under **Chat Bots** in the community list beside chat, so add any others from there, one per line. Changes apply within a couple of seconds. Everyone left off still keeps and earns points.

### Channel point rewards

Rewards listed in `channel_point_rewards.txt` in the data folder give chaos points to whoever redeems them, and the bot says so in chat. The file starts with the Stardew Valley version's three: `Get Chaos Points (Small)` for 50, `Get Chaos Points (Medium)` for 150 and `Get Chaos Points (Large)` for 400. Create rewards with those titles on Twitch, or change the file to match yours. Each line is a title, `=`, then the points. Capitals don't matter, and changes apply within a couple of seconds. The mod leaves other rewards alone, so Streamer.bot or anything else can keep handling them.

Twitch only tells your channel's own account about redemptions, so this needs a second token:

1. Make a token for your channel's account, not the bot's, with the `channel:read:redemptions` scope. With a generator like the one the installer suggests, make sure the browser is signed in to Twitch as your channel first.
2. Off stream, add it to `ChatVsStreamer.ini` under `[Twitch]` as `BroadcasterToken=<token>`, then restart the game. If your bot account is your channel, one token with all three scopes can go in both places.
3. The mod's log says `Listening for channel point rewards` once it works, or says why it doesn't. For example, it notices a token that belongs to the bot.

The mod gets redemptions from Twitch EventSub rather than from Helix's list of redemptions, because that list only covers rewards created by the same app. It doesn't mark redemptions as done, so if a reward doesn't skip the request queue, clear that queue on Twitch now and then.

If an effect can't run yet, it waits in the queue. Reasons include a cutscene, a loading screen, the streamer being wasted or a vehicle effect while on foot. If it still can't run after two minutes, or the game refuses it, the viewer is refunded automatically and told why.

## Effects

| Tier | Effects |
|---|---|
| **Nuisance** | `ragdoll` `horn` `fog` `storm` `snow` `midnight` `drunk` `nohud` `legday` `jumpscare` |
| **Disruptive** | `flat` `butterfingers` `slowmo` `stall` `yeet` `jam` `moon` `blackout` `snitch` `nobrakes` `dogs` `floorit` `ice` `loadout` `honk` `eject` `morph` |
| **Painful** | `onfire` `tax` `mob` `pinball` `speed` `carjack` `zombies` `clowns` `lesters` `lions` `wanted` `downgrade` `freefall` `aliens` `cultists` `clones` `abduct` `warp` `swap` `sasquatch` `astronauts` `hitsquad` `madmax` `warzone` `feds` |
| **Devastating** | `carmageddon` `flood` `carbomb` `airstrike` `tank` `airsupport` `kill` `nuclear` |
| **Blessing** | `clearsky` `daybreak` `heal` `armor` `ammo` `repair` `cash` `caffeine` `clean` `guns` `car` `plotarmor` |

Effects are defined once, in the table at the bottom of [src/Effects.cpp](src/Effects.cpp). Chat, the overlay, the panel, `settings.ini` and the commands page all read from it.

Effects that could break a mission (teleports, vehicle swaps, clock changes) wait for the mission to end. You can turn that off in the panel.

### Swarms

`zombies`, `clowns`, `lesters`, `aliens`, `cultists`, `sasquatch` and `astronauts` each surround you with eight attackers, spread all round, 12 to 25m out. They fight like professionals, never take cover and keep closing in. Each has a red blip on the map that comes off when it dies. The mod hands them straight back to the game, which clears them away once they're dead or left behind. While they're alive they still count toward the mod's cap of 40 attackers, so a swarm waits if there isn't room.

- While any sasquatch is standing, every punch or swing from anyone the game controls hits three times as hard.
- Stun guns knock you down without killing you, so after 45 seconds of play (the pause menu doesn't count) any aliens still standing run off.

Each swarm is a row in `kSwarms` in [src/Effects.cpp](src/Effects.cpp): its models, weapons, accuracy, melee boost and retreat time. If this game version lacks a model or weapon, the next one listed is used. A new swarm is a row there, plus a row with the same id in the effect table.

### Morph

`morph` (or `!whoami`) puts you in another body for 30 seconds: an alien, a sasquatch, Impotent Rage, a mime, a chimp or a zombie. Your own ped isn't changed. It waits where you left it, hidden and untouchable, and you're put back in it at the end, so your weapons, attachments, outfit and tattoos come back exactly as they were. Changing your model instead would lose those.

- The body starts with your health and armor and, unless it's the chimp, a copy of the weapon in your hands. Whatever happens to the body stays with the body, unless it would kill you.
- While morphed you can't get into vehicles, use the phone (so no quick saves) or switch characters, because the game's own scripts expect one of the three.
- Attackers on foot switch to the body, then back to you.
- Story mode can only respawn you as Michael, Franklin or Trevor. Dying as anyone else leaves the screen black for good, so the body has 5,000 health to spare, and the health bar hardly moves while you're morphed. Damage still counts: once the body has taken as much as your own health, you change back and die as yourself. Being busted changes you back straight away too.
- If you're in the air when it ends, you change back once you land.
- Effects that hand you a weapon or a parachute wait while you're an animal, including the game's own peyote animals.

### Alien abduction

`abduct` lifts you 25m in a turquoise beam, pulling you out of a vehicle first and popping its doors if it has a roof. Three or four aliens gather underneath, aiming up. At the top there's a flash, the aliens vanish and the beam lets go. A parachute opens on the way down unless that's switched off in the panel. It waits while there's a roof over your head, you're swimming or you're in the air.

### Wasted

`kill` (or `!wasted`) kills you on the spot, and the game's own death screen and respawn follow. It costs 5,000 points by default, more than anything but `nuclear`, and the panel can change that or turn it off. It doesn't wait for a mission to end, so on a mission it fails the mission, as any death would.

- It waits while you're invincible, so Plot Armor holds it off until it wears off.
- If you're morphed, you change back first and die as yourself, because story mode can't respawn you as anyone else.

## Control panel

`http://localhost:7373/panel` replaces the in-game menu the other versions have, because GTA V has no mod menu to plug into. It has every price and toggle, a **Fire** button for each effect, and a chat simulator, so you can test with no Twitch connection. Simulated replies stay on the panel and never reach your real chat.

| Hotkey | Default |
|---|---|
| Fire a random effect (same pool as the Chaos Gods, skipping any that can't run right now) | F7 |
| Pause or resume purchases | F9 |

**Chaos Gods** is off by default. Turn it on and, if nobody buys anything for a while, a random effect fires on its own.

## Building

Needs Visual Studio 2022 (or the Build Tools) with the C++ workload, and CMake 3.25+. No SDK download for Script Hook V: its import library is generated from [src/shv/ScriptHookV.def](src/shv/ScriptHookV.def).

The RPH plugin also needs the .NET SDK and RPH's own SDK: copy `RagePluginHook.dll` and `RagePluginHook.xml` out of the NuGet package `RagePluginHook` 1.124.0 into `third_party/rph_sdk/`. Its license allows using it to develop plugins but forbids uploading or redistributing it, so that folder is git-ignored and never goes in `dist`. Without it, `build.ps1` builds everything else and says so.

```powershell
.\build.ps1           # builds dist\ (the plugin, web pages and installer)
.\build.ps1 -Test     # ...after running the end-to-end tests
.\build.ps1 -Deploy   # ...and runs the installer against your game
```

### Tests

`.\build.ps1 -Test` loads the real plugin against a simulated Script Hook V ([tests/MockScriptHook.cpp](tests/MockScriptHook.cpp)), then drives it with real HTTP requests and chat lines, with time sped up 30×. It covers points, refunds, queueing, game-state gating, every effect, restoring state after timed effects, and the web server's origin checks. It uses a throwaway data folder, so it can't touch your real ledger.

It then runs all of that again on the RPH build, with the test program standing in for the C# plugin, plus what only that build needs: reloading the plugin, noticing a loaded save and polling hotkeys. The C# plugin itself only runs in the real game. [tools/rph_probe](tools/rph_probe/CvsProbe.cs) is a read-only plugin that checks the kinds of native call it depends on there.

If Smart App Control blocks a build, its tests are reported as not run, rather than failed.

Last, [tests/InstallerTest.ps1](tests/InstallerTest.ps1) runs the installer scripts against made-up game folders under `%TEMP%`: every branch of `install.ps1`, beside a stand-in for `rph-files.ps1`, then the real `rph-files.ps1` with a made-up zip. It never touches a real game, RPH or your data folder. The prompts for admin rights and typing answers at a real keyboard are the parts it can't cover. It's reported as not run while GTA V or RPH is running, because the installer refuses to work then.

To try the control panel and overlay with no game running, using the same simulated game at normal speed:

```powershell
.\build\Release\TestHost.exe "$PWD\build" "$PWD" --serve
```

Then open `http://localhost:17380/panel` and `http://localhost:17380/`.

To check the Twitch connection for real (anonymous and read-only, it never posts):

```powershell
.\build\Release\TestHost.exe "$PWD\build" "$PWD" --twitch-smoke yourchannel
```

### The commands page

[docs/index.html](docs/index.html) is The Chaos Dispatch, the viewers' page of commands, effects and point rules, published by GitHub Pages from `main`, folder `/docs`. Its data, [docs/catalog.js](docs/catalog.js), comes from the source: [tools/gen_commands_page.ps1](tools/gen_commands_page.ps1) reads the effect table, the default settings and the point rules, and `build.ps1` runs it every time, so a new effect or price shows up there by itself once it's pushed. The script stops the build if it can't read something, so keep each effect on one line in the table. The page's wording is in `docs/index.html`.

The page shows the default prices. If you host your own copy, a fork publishes the same way.

### Adding a native

Add its name to [tools/natives.txt](tools/natives.txt), then regenerate [src/generated/Natives.g.h](src/generated/Natives.g.h):

```powershell
python -I tools\gen_natives.py --db <folder>
```

`<folder>` holds `natives.json` and `natives_gen9.json` from [alloc8or/gta5-nativedb-data](https://github.com/alloc8or/gta5-nativedb-data), which aren't checked in. The generator refuses any native whose hash or parameters differ between the Legacy and Enhanced data, so a bad signature stops the build and can't crash a stream.

## Adding a platform

Twitch is the only platform for now, but nothing outside [src/TwitchChat.cpp](src/TwitchChat.cpp) knows that. A TikTok or YouTube source only has to produce `ChatMessage` and `ChatEvent` values with prefixed viewer ids (like `tt:name`, so they can't collide in the ledger) and give `Mod::Reply` somewhere to send text.

## Roadmap

- TikTok (Tikfinity) and YouTube (Streamer.bot): port the managers from the Stardew mod
- Channel point redemptions and a Twitch OAuth flow (needs EventSub and Helix)
- In-game chat display, point drops, a public shop page

## Credits

- [Script Hook V](https://www.dev-c.com/gtav/scripthookv/) by Alexander Blade (separate download, not bundled)
- RAGE Plugin Hook by MulleDK19 and LMS (separate download, not bundled)
- Native signatures from [alloc8or's native DB](https://github.com/alloc8or/gta5-nativedb-data)
- Grand Theft Auto V is a trademark of Take-Two Interactive. This is an unofficial fan mod, not affiliated with Rockstar Games or Take-Two.

## License

[MIT](LICENSE). That covers this mod's own code. Script Hook V and RAGE Plugin Hook are separate downloads with their own terms, and neither is part of this repository.
