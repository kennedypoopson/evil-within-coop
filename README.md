# EvilWithThem: experimental co-op for The Evil Within (2014)

A DLL mod that adds a second player to The Evil Within's campaign. **Alpha, work in progress.** Each player runs
their own copy of the game; a custom UDP netcode shows the other player as a real second Sebastian (an `idPlayer`)
driven by their input.

Only for the **Steam build 2708258** (`EvilWithin.exe` SHA-256
`743df7a126a66470dbfc27b8c30348eea7d29bf948b82705da6e7babc6e67437`). All addresses are hardcoded for that build;
the DLL checks its vtables and refuses to hook a different build.

No game files are included or needed beyond your own install. Nothing in the game folder is changed: the mod is
injected into the running game.

## Status (paused, November 2026)

Protocol **v9**. Most features below were tested with two game copies on one PC; a first two-PC session with a
friend connected (Steam P2P join worked once, shared death fired), then joining broke after a death. The last
builds change the Steam join (see "Joining") and are **untested between two PCs**.

### What works (two copies on one PC)

- **Second player**: a second `idPlayer` spawned next to you, with the engine's one-player limits lifted just for
  that spawn. Real Sebastian movement, sprint, sneak, aiming, attacks, health and death.
- **Netcode** (UDP or Steam P2P, 30 Hz): position + velocity, camera angles and aim pitch, body facing, movement
  input, equipped weapon, ammo type, lantern (held + lit), health, body mode, sneak state, and the script
  restrictions on the player (chapter 1 slow walk, no sprint/fire...). Shown Source-style, 100 ms in the past.
- **Shots**: projectile start + direction of the shooter (bolts land in the same spot).
- **Hits on players**: replayed on player 2 so stagger/knockdown/wounded play.
- **Grabs and scripted kills** (`idAnimatedEntity::StartSynchroMotion`): the paired animation is replayed on player 2;
  the Sadist plays his half (his kill state is driven).
- **Enemies**: each enemy is run by the game whose player is nearest (host decides, with hysteresis); that game
  streams its pose and forwards its animation requests/params (`0x7CC920` / `0x7CD8C0`), the other game follows.
  Corpses are excluded. A dead player's game hands its enemies to the living one.
- **Pickups**: an item/bolt picked up in one game is removed in the other; key items are given to both players.
- **Story**: player 2 never fires triggers; a trigger our own player enters is fired for the other game's player
  (far-behind partner is teleported there first). Scripted player animations (look around, kick a door) are
  forwarded while the player is under script control.
- **Death**: one player dying makes the other die too (the game's own "kill" event: full death sequence), so both
  reload the same checkpoint. Restarts and map loads are crash-safe (all level pointers dropped mid-frame).
- **Map changes**: `nextmap` (level transitions / chapter ends) is copied to the other game; the host broadcasts its
  map, the client is warned on mismatch and Ctrl+Shift+M jumps to the host's map/checkpoint. Joining from the
  main menu loads the host's map with `slowmap`.
- **Visiting player**: his campaign save is never written while connected; he plays a separate **co-op character**
  (`ewcoop_character.txt`: inventory + player/weapon upgrade levels), first copied from the host's.
- **Looks**: Joseph look for the partner (Ctrl+F9) or yourself (Ctrl+Shift+X); a joining player is Joseph by
  default. Joseph's mesh rides Sebastian's skeleton (Sebastian's animations; Joseph's own animation set is AI-only).
- **Tools**: Ctrl+Shift+F12 snapshot (screenshots + state of both copies into `snapshots/`), health-drop call chains,
  hardware watchpoints. `tools/research.py`: offloads IDA questions to a cheap model (DeepSeek via OpenRouter) with
  read-only IDA tools (`--annotate` lets it name/comment functions in the IDB).

### Not done / known problems

- **Steam joining between two PCs**: Steam's "Join Game" can't be used (the game's leftover multiplayer code
  handles it and loads the title screen). The client presses Ctrl+Shift+O, which finds the host's lobby through the
  friends list and connects P2P to the host's Steam ID; the host accepts the first packet. Untested after the fix.
- Steam rich presence is set but Steam doesn't show it (the game has no rich presence localization).
- Doors / one-time world state, enemy deaths + burning, traps, weapon effects of player 2, partner HUD, healing
  the partner with a syringe, safe haven.
- Only hand-tested. No automated tests.

## Build

Windows, [MSYS2](https://www.msys2.org/) UCRT64 with clang, CMake and Ninja:

```
pacman -S mingw-w64-ucrt-x86_64-clang mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja
build.bat            (release)   build.bat debug
```

Output: `build\release\ewcoop.dll` and `build\release\inject.exe`.

## Use

See [HOW_TO_PLAY.txt](HOW_TO_PLAY.txt). Short version:

1. Start the game from Steam and load a save. Run `inject.exe` (it injects every running copy). Both players need
   the same build.
2. Host: **Ctrl+Shift+K** (UDP + a friends-only Steam lobby). Client: **Ctrl+Shift+O** (joins a Steam friend who
   hosts; falls back to the address in `ewhost.txt` over UDP/VPN). Do not use Steam's "Join Game".
3. Ctrl+Shift+U stop, Ctrl+Shift+M go to the host's map, Ctrl+F9 partner as Joseph, Ctrl+Shift+X play as Joseph,
   Ctrl+Shift+F12 snapshot. Solo netcode test: **Ctrl+Shift+E**.

Two copies on one PC: in the running copy press **Ctrl+Shift+F1** (releases the game's one-instance mutex), then
run [tools/start_second_copy.bat](tools/start_second_copy.bat), inject, host in one and join in the other.

Each copy logs to `ewcoop_<pid>.log` next to the DLL.

## Where to start reading

- [include/game.h](include/game.h): every engine address and structure used, with how it was found.
- [include/actor_vtables.h](include/actor_vtables.h): the 168 character vtables and their Damage functions (RTTI).
- [src/mod/dllmain.cpp](src/mod/dllmain.cpp): the mod. Sections: second player spawn and its fixes, input
  (`feedPlayer2`), netcode v2/v3 (`netRun`, `applyRemote`, `interpolateRemote`), focus/input isolation, enemy damage
  replay (`ewPreDamage`, `applyEvents`), debugging tools (hardware watchpoints, crash and pure-call loggers).
- [src/inject/inject.cpp](src/inject/inject.cpp): the injector.

Key findings, in short:

| What | Where |
|---|---|
| `gameLocal` global / `RunFrame` (hooked, vtable slot 13) | `0x9AB3AF0` / `0x7A3EB0` |
| `idPlayer::Spawn` MAX_PLAYERS check / per-player record write | `0x8906FC` / `0x9186A0` |
| Player position authority: motion object `player+1720` (`0x852020`) | copied into physics by `0x91F2C0` |
| Give a player a usercmd | `0x88F440(player, &cmd, &cmd)` |
| `IsLocalPlayer` (gates the controller input step) | idPlayer vtable slot 228 |
| View angles / delta angles on the controller (`player+0x2A8` -> vfunc +0x60) | `+20524` / `+19660` |
| Body facing quaternion (motion object +352) | get `0x84F8A0`, set `0x851EB0` |
| Equipped weapon item / SelectWeapon / GiveItem | `player+2728+16` / vtable +2176 / `0x8592A0` |
| Character Damage (all classes) | vtable slot 80 |

## Credits

Reverse engineering and code by kennedypoopson with AI assistants (Claude Code, DeepSeek). Follows the
[Co-op Anything](https://github.com/cranebeaker/coop-anything) playbook.

Copyright (C) 2026 kennedypoopson. Licensed under the **GNU Affero General Public License v3.0** (see
[LICENSE](LICENSE)): you may use, change and share it, but modified versions you distribute or run for others over
a network must also be released under the AGPL with their source. (Releases before November 2026 were MIT.)
The Evil Within is a trademark of ZeniMax / Tango Gameworks; this project is unaffiliated and contains no game data.
