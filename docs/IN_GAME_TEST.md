# In-game test checklist

The automated tests (`.\build.ps1 -Test`) run both builds of the mod against a simulated game. They prove that chat, points, queues, refunds, timers and clean-up work, and that every native is called with a hash and argument list that exist in both the Legacy and Enhanced references.

They **can't** prove what the game does with those calls. Only GTA V can. Work through this list the first time you run the mod, and after any game patch.

The easiest way to test is the control panel's **Fire** button (`http://localhost:7373/panel`). It skips price and cooldown, so you don't need Twitch or points.

Be out in the open world, past the prologue. Effects wait while a mission or cutscene has taken control, and the first mission happens away from the main map.

## 1. Does it load?

### With RAGE Plugin Hook

Start the game with `StartGame.bat`, load story mode, and check each of these in order. If one fails, the ones after it will too.

| Check | If it fails |
|---|---|
| The BattlEye window doesn't appear | BattlEye is still on. Turn it off in the launcher's settings for GTA V. |
| RPH doesn't show a DirectStorage message | On an NVMe drive, run `rph-files.ps1 -SkipDsCheck`. |
| RPH doesn't say `Could not hook game process` | RPH couldn't open its log from the last run, which `StartGame.bat` clears first. If you used `StartGame.bat` anyway, send in the `crashreport` file from the game folder. |
| `RagePluginHook.log` in the game folder exists and shows `Chat vs Streamer: started` | `Windows error 4551` there means Smart App Control blocked `ChatVsStreamerCore.dll`. If the plugin was never mentioned, press F4 in game and type `LoadPlugin ChatVsStreamer.dll`. If the game crashed instead and there's no log, the launcher probably didn't have admin rights, so use `StartGame.bat`. |
| `%LOCALAPPDATA%\ChatVsStreamerGTAV\ChatVsStreamer.log` shows `Running through RAGE Plugin Hook` | Look in `RagePluginHook.log` for a line starting `Chat vs Streamer:`. |
| `http://localhost:7373/panel` opens | Check the log for `could not bind`. Something else is using port 7373. |
| The panel header shows `game: ready` | The script isn't ticking, or something has taken control. Check that you're past the prologue and walking around. |
| The log shows `Joined #yourchannel` | Check the log for a Twitch notice. The token is probably wrong or expired. |

If a native fails, `RagePluginHook.log` gets one line for it starting `Chat vs Streamer: native 0x`. Send that line in with your report.

**After the first launch:** close the game and start it again with `StartGame.bat`, to check a second launch works too. It moves the last run's RPH log to `%LOCALAPPDATA%\ChatVsStreamerGTAV\RagePluginHook.previous.log`, so look there for anything from before the restart.

### With Script Hook V

First, make sure [Script Hook V's page](https://www.dev-c.com/gtav/scripthookv/) lists your game's exact version. The installer tells you, or see "Which version of GTA V?" in the README. If it isn't listed, stop here: nothing below can work until it is. Expect an "unknown game version" error from Script Hook V if you try anyway.

Then start story mode and check each of these in order. If one fails, the ones after it will too.

| Check | If it fails |
|---|---|
| `%LOCALAPPDATA%\ChatVsStreamerGTAV\ChatVsStreamer.log` exists and was written just now | The plugin didn't load. Check that `ScriptHookV.dll`, `dinput8.dll` and `ChatVsStreamerGTAV.asi` are all next to the game's exe, and that BattlEye is off in the launcher's settings. |
| The log shows `Script Hook V reports game version` | Same as above. |
| `http://localhost:7373/panel` opens | Check the log for `could not bind`. Something else is using port 7373. |
| The panel header shows `game: ready` | The script isn't ticking. Load a save and walk around. |
| The log shows `Joined #yourchannel` | Check the log for a Twitch notice. The token is probably wrong or expired. |

**Reported Enhanced issue:** one March 2025 forum post said Script Hook V's `dinput8.dll` didn't load on Enhanced, and another source mentions `xinput1_4.dll` as the Enhanced loader. Both may be out of date. Script Hook V's own page currently says to use `dinput8.dll`, so start there. If the log never appears, this is the first thing to look into. LSPDFR's Enhanced page also refers to an `xinput1_4.dll` from Script Hook V for people with NVMe drives, so check that if you're on one.

## 2. A real chat purchase

1. In your real Twitch chat, type `!balance`. The bot should answer.
2. Type `!give yourname 100000`, then `!buy fog`.
3. Check that fog rolls in, a ticker appears in the game feed, and a toast appears on the overlay.
4. Check the overlay's leaderboard. Your own channel, the mod's bot, and your channel's chat bots shouldn't be on it.
5. With `BroadcasterToken` set, check that the mod's log says `Listening for channel point rewards on #yourchannel`. Then redeem `Get Chaos Points (Small)` from an account that isn't the channel. The bot should reply `✨ @name redeemed Get Chaos Points (Small) and received 50 chaos points! (balance: …pts)`, the overlay should show a toast, and that account's `!balance` should agree.

Step 5 was tried on 8 November 2026. The first `BroadcasterToken` belonged to the channel but lacked `channel:read:redemptions`, and the log said so. With a new token, the log said `Listening for channel point rewards on #neighborhoodofmusic`. At 15:19 the channel redeemed its own `Get Chaos Points (Small)`, and the mod logged `NeighborhoodofMusic redeemed Get Chaos Points (Small) for 50 points`. The streamer saw the bot's reply in chat and the toast on the overlay.

Steps 1 to 3 were confirmed on 8 November 2026, on Enhanced `1.0.1158.16` through RAGE Plugin Hook. The bot answered `!balance` and `!give`, and `!buy fog`, typed while the game was paused, waited in the queue and ran when the game resumed, with the ticker in the game feed. The overlay's event stream sent the toast, but nobody was watching the overlay itself. `!feds` from real chat gave five stars at the normal 1,500 points. A second `!buy fog`, typed while the game stayed paused, gave up after 2 minutes ("the game is paused" in the log), which refunds the points and tells the viewer in chat. Step 4 came out of that test: the bot account and `LimelightStream`, one of the channel's chat bots, were on the leaderboard. On the next start with the fix, the leaderboard the overlay draws from (`/state`) was empty, because all three accounts with points were left off: the channel, the bot, and `LimelightStream` from `leaderboard_hidden.txt`. A `!give` of 1 point to a real viewer then put that viewer on it, alone.

## 3. Effects

Tick each one off, or note what happened instead. Fire them one at a time.

The ticks below are from Enhanced `1.0.1158.16` through RAGE Plugin Hook on 6 and 7 November 2026. An unticked effect may have run without errors, but nobody has confirmed what it did on screen. Clear the ticks after a game patch.

**On foot**
- [x] `ragdoll`: falls over for 3.5s, and can't get up early by pressing keys.
- [x] `drunk`: for 15s, a stumbling walk, a swaying camera, a blurry screen and backwards controls, then normal. In a car, left and right steering swap instead. Aircraft controls aren't reversed.
- [x] `legday`: can't sprint or jump for 30s
- [x] `jam`: can't shoot or punch for 20s
- [x] `butterfingers`: the gun in your hands drops about 2m away, and you can walk over and pick it back up (needs a weapon out). The drop is confirmed, but picking it back up hasn't been tried.
- [x] `nohud`: minimap and HUD gone for 45s
- [ ] `yeet`: launched into the air (ran, but what it did on foot wasn't confirmed; the vehicle version below is)
- [ ] `onfire`: catches fire (ran, but what it did on foot wasn't confirmed; the vehicle version below is)
- [x] `jumpscare`: nothing for up to 40s, then a bang, a white flash and a camera shake, with no damage

**In a vehicle**
- [x] `horn`: horn **and** alarm for 15s. The horn is re-pressed every 0.4s because the game cuts it short for players, so listen for whether it sounds continuous or stuttery.
- [x] `flat`: all tires go flat
- [x] `stall`: engine off for 10s, then back on
- [x] `nobrakes`: brake and handbrake do nothing for 20s
- [x] `floorit`: the car accelerates on its own for 12s
- [x] `eject`: door opens (or a convertible's roof drops), and you fly about 30m straight up with half the car's speed. You get a parachute for the way down, but it hasn't been checked that there's time to open it. The tester died within about 10s of ejecting, cause not known.
- [ ] `ragdoll`: bails out of the moving car, then stays down for 3.5s. New in a vehicle.
- [x] `carjack`: thrown out, then a stranger drives the car away
- [x] `downgrade`: put on a scooter at the same speed, old car nearby
- [x] `carbomb`: 6 beeps, then the vehicle explodes
- [x] `repair`: fully fixed and clean
- [x] `onfire`: engine catches fire
- [x] `yeet`: the car is launched

**Attackers: do they come at you, and do they fight?**
- [x] `dogs`
- [x] `mob`, `lions`. `clowns` and `zombies` also passed here before they became swarms, and passed again as swarms on 15 November 2026 (see Swarms below).
- [x] `hitsquad`: a truck arrives and four soldiers fight (check they get out or shoot)
- [x] `tank`: a tank drives over and attacks (check it engages)
- [x] `airsupport`: a helicopter flies over with gunners (check it follows and doesn't crash at spawn)

**World**
- [x] `fog`: weather changes
- [x] `storm`, `snow`: weather changes
- [x] `clearsky`: sunny, and the weather resumes changing naturally
- [x] `midnight`, `daybreak`: clock jumps
- [x] `moon`: huge jumps on foot for 30s, then normal. Before the jumps were added, a tester on foot couldn't see a difference. Whether cars feel floaty hasn't been checked.
- [x] `slowmo`: slow for 20s, then normal
- [x] `blackout`: city lights out for 60s, then back (test at night)
- [x] `snitch`: 2 stars. `wanted`: 4 stars. `clean`: stars gone.
- [x] `warp`: teleported; run it a few times to check you don't land inside anything. The first of three was followed by a death within 4 seconds, cause unknown.
- [x] `freefall`: high in the air with a parachute (does the parachute work?)
- [x] `airstrike`: explosions walk closer for 8s
- [x] `nuclear`: 4 beeps, a white flash, a ring of explosions, nearby vehicles blow up, aftershocks, red sky then smog

**Newest effects**
- [x] `carmageddon`: for a minute, vehicles fall from the sky, tumbling, 15 to 60m in front of the camera, 10 to 15 at a time: one batch straight away, then three more within 5s either side of the 15, 30 and 45s marks, for 40 to 60 in all. Spread up to 30m all round, the tester found them too close, and up to 60m all round they saw only explosions. The tester said this version looked great.
- [x] `ice`: the car slides everywhere for 15s, then grips again. It should also end if you get out, which hasn't been tried.
- [x] `pinball`: for 20s, hitting a wall or car throws you off it hard, and harder the faster you hit it. Driving or walking on the ground doesn't. The log gets a `Pinball threw the streamer` line for each bounce, and a `Pinball on foot` summary when it ends. In a car it's brutal: 7 throws in about 4 seconds, all at the 40 m/s cap, and the player died soon after. That strength is deliberate. On foot there are 2.5s between throws to get back up. The game doesn't always report a hit on foot, so the mod also looks for a wall just ahead. In the first test that worked, each of those caught one of two throws, and the tester survived both.
- [x] `swap`: your car turns into one of a scooter, tank, tractor, jet, submarine, bulldozer or blimp at the same speed, with you driving. The old car is deleted, so don't try this in one you want to keep.
- [x] `speed`: top speed with no brakes for 12s
- [x] `honk`: for 25s, the horn sets off an explosion in front of the car, at most one every half second, without hurting you
- [x] `madmax`: for 20s, nearby drivers ram you, then go back to normal, and parked cars stay parked.
- [x] `flood`: a storm, big waves, and the water rises for 30s, then falls. Confirmed on a street. It hasn't been tried at the beach.
- [x] `warzone`: for 20s, your shots and everyone else's explode where they land. Pick a fight with the cops to see the second part.
- [x] `loadout`: a silly weapon is forced into your hands with no switching for 5s. A weapon you didn't own goes away after 30s. The weapon and the lock are confirmed, but the 30 seconds hasn't been checked.
- [x] `clones`: 10 clowns with machetes, golf clubs and nightsticks attack
- [x] `feds`: 5 stars straight away, then helicopters and SWAT

Also try each direct command from real chat, such as `!feds`. It costs the same as `!buy feds`. `!feds` worked from real chat on 8 November 2026, and the others haven't been tried from real chat.

**Swarms** (new on 8 November 2026)

On 15 November 2026 all seven were fired once each, one after another, and the streamer said they all looked right. With earlier swarms still around, they killed the streamer 20s after `cultists`, 10s after `clowns`, 5s after `sasquatch` and 6s after `astronauts`.
- [x] `zombies`, `clowns`, `lesters`, `aliens`, `cultists`, `sasquatch`, `astronauts`: eight attackers come at you from all round, 12 to 25m out, and rush you rather than take cover. Each has a red blip on the minimap, and a blip goes when its attacker dies.
- [x] `lesters`, `cultists`, `sasquatch`, `aliens` and `astronauts` use models nothing else here has tried. Check each one appears and fights. `cultists` was tried off stream first, because some of the Altruist cult's models are nude, and on 15 November 2026 all eight looked right. The mod leaves the attackers' outfits to the game, so if the game varies them, a later swarm could look different.
- [ ] `sasquatch`: punches hurt far more than with `zombies`, then go back to normal once every sasquatch is down. It uses the cutscene model `cs_orleans`. If that misbehaves, `ig_orleans` is the in-game one.
- [ ] `aliens`: the stun guns knock you down. Note whether they can keep you down. After 45s, any still standing should run off and lose their blips.
- [ ] `astronauts`: they fire Up-n-Atomizers, which throw you around.
- [ ] Leave the area during a swarm. A few minutes later they should be gone, with no red blips left on the map.
- [ ] Try one from real chat, such as `!aliens`.

**Morph and abduction** (new on 8 November 2026)

On 15 November 2026 each was fired once on foot, and the streamer said both looked right. `morph` lasted its 30s and changed back without needing its fallback.
- [x] `morph`: you become one of six bodies for 30s, then you're yourself again, with the same weapons (attachments included), outfit and tattoos. Fire it a few times to see more than one body. It swaps which ped you control rather than changing your model. Only one body has been seen so far, and which one wasn't noted.
- [ ] As the chimp, check you can move and attack. Whether a chimp works this way is the biggest unknown.
- [ ] While morphed, you can't get into a car, open the phone or switch characters. As the chimp, you can't pick a weapon, and `guns` waits until you're human again.
- [ ] Fire attackers, then `morph`. They should go after the body, then after you once you change back.
- [ ] Die while morphed. You should change back, die as yourself and respawn as normal. On 15 November 2026 the body died 9s into a morph, with `lesters` around, and the screen stayed black for good: story mode can't respawn the player as anyone but its three characters. The build after that gives the body 5,000 health to spare, and changes you back to die as yourself once your share is gone. That fix needs this test again. `morph` then `kill` is the quickest way to check the change back and the death, but only being killed by attackers tests the spare health. Also try getting busted while morphed, which should change you back straight away.
- [ ] `!whoami` from chat does the same as `!morph`.
- [x] `abduct` on foot: a turquoise beam, and you rise slowly. 3 or 4 aliens appear below, aiming up. Then a flash at about 25m, the aliens vanish, a parachute opens and you float down. Note whether you actually rise, how it looks, and whether the flash has the alien particles. Without them, only the white flash shows.
- [ ] `abduct` in a car (the doors pop and you're pulled out) and on a motorbike (no doors).
- [ ] `abduct` under a bridge or a roof. It should wait until you're out in the open.
- [ ] Switch off "Alien Abduction opens a parachute" in the panel, then `abduct`: you fall from 25m with nothing to break it.

**Wasted** (new on 15 November 2026)
- [ ] `kill` on foot, then again in a car: you die on the spot, and the game's own death screen and respawn follow.
- [ ] `plotarmor`, then `kill`: nothing happens until the 20s of invincibility are up, then you die.
- [ ] `morph`, then `kill`: you change back straight away, die as yourself and respawn as normal.
- [ ] `!kill` and `!wasted` from real chat. Each costs 5,000 points.

**Blessings**
- [x] `heal`: bar fills
- [x] `armor`: bar fills
- [x] `cash`: +$5,000. `tax`: -10% (minimum $100). **Then save, reload and check the amount stuck.** The amounts are confirmed, but the save and reload haven't been tried.
- [x] `ammo`: +250 rounds (needs a gun out)
- [x] `guns`: pistol, SMG and carbine
- [x] `car`: a supercar appears ahead, with the buyer's name on the plate
- [x] `plotarmor`: invincible for 20s, **then definitely not invincible** (let something hurt you)
- [x] `caffeine`: faster running, huge jumps for 30s, then normal

## 4. Guard rails

- [ ] Start a mission, fire `warp`. It should wait; finish the mission and it fires (or the viewer gets refunded after 2 minutes).
- [ ] Fire `plotarmor`, then trigger a cutscene mid-way, for example by stepping into a mission marker. It should end immediately, so you can be hurt again. You're invincible while it runs, so dying isn't a practical way to test this. Deaths have been seen ending `loadout` and `moon` straight away.
- [x] Fire `moon`, then get wasted. Gravity should be normal after respawning. On 8 November 2026 the death ended `moon` with 12s left, and the tester's jumps were normal after respawning. On foot the jumps are what you can see, and the same step that stops them puts gravity back.
- [ ] Open the pause menu during `slowmo`. The timer should freeze. It held still during a pause of about a second, which is too short to be sure. On 8 November 2026 the pause menu held `moon`'s timer at 24s for the 6s it was open, so timers do freeze. Whether slow motion itself carries on after the menu hasn't been checked.
- [x] Press F9: purchases pause. Press F9 again: they resume. Press F7: a random effect fires. F7 used to pick vehicle-only effects on foot, which waited 2 minutes and were dropped. It now only picks what can run where you are. On 8 November 2026, 8 presses each started an effect within a second, and the one car-only pick (`ice`) came while the tester was in a car.
- [ ] Alt-tab to another window and press F7. Nothing should fire. Skipped on 7 November 2026.
- [ ] Switch characters during `drunk`. Neither character should stay drunk. Not tried on 8 November 2026, because the tester's save had only one character unlocked.
- [x] Fire `moon`, then load a save. Gravity should be normal afterwards, and attackers should still fight you when fired after the load. On 8 November 2026 the load ended `moon` with 24s left, jumps were normal afterwards, and `clowns` fired 10s after the load fought the tester. The mod showed "not running" for about 7s in the middle of the load, then carried on by itself.
- [x] Fire several attackers in a row, then leave the area. After a few minutes they should be gone, with no slowdown. On 8 November 2026, `dogs`, `zombies` and `mob` (17 attackers) were fired 5s apart. The tester drove away, and they were gone the second time the tester came back to look. The first look was too soon. The tester didn't mention any slowdown.

## Reporting a problem

Note the effect, what you expected and what happened, and include `ChatVsStreamer.log`. Delete `ChatVsStreamer.ini` from anything you share, because it holds your token.
