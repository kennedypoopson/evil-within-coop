# EvilWithThem: experimental co-op for The Evil Within (2014)

A DLL mod that adds a second player to The Evil Within's campaign. **Alpha, work in progress.** Each player runs
their own copy of the game; a custom UDP netcode shows the other player as a real second Sebastian (an `idPlayer`)
driven by their input.

Only for the **Steam build 2708258** (`EvilWithin.exe` SHA-256
`743df7a126a66470dbfc27b8c30348eea7d29bf948b82705da6e7babc6e67437`). All addresses are hardcoded for that build;
the DLL checks its vtables and refuses to hook a different build.

No game files are included or needed beyond your own install. Nothing in the game folder is changed: the mod is
injected into the running game.

## What works (tested with two game copies on one PC)

- **Second player**: a second `idPlayer` spawned next to you (Ctrl+F8), with the engine's one-player limits lifted
  just for that spawn. Real Sebastian movement, sprint, sneak, aiming, attacks, health and death.
- **Netcode** (UDP, protocol v3, 30 Hz): position + velocity, camera angles, body facing, full movement input and
  the equipped weapon. The remote player is shown Source-engine style, 100 ms in the past, interpolated between
  snapshots (extrapolates up to 250 ms when packets are late). His own movement code runs on the remote input;
  position is only corrected when it drifts.
- **Weapons**: the remote player holds what the other side holds (given to him if he doesn't own it).
- **No friendly fire** between the two players.
- **Two copies on one PC** for testing: the background copy keeps running (no pause) and ignores your keyboard and
  mouse.
- **Enemy damage sync, v1 (first tests)**: hits you land on a character are replayed on the same character (matched
  by its level name) in the other game, with player 2 as the attacker.

## Not done yet

- Enemy AI is still simulated separately in each game (enemies can be in different places).
- Doors, pickups, scripted events, checkpoints and chapter changes are not shared.
- Damage events are sent three times without acknowledgement; spawned enemies with generated names may not match.
- Player 2 looks like Sebastian. A Joseph look was started (the game's alternate-body switch attaches Joseph's
  mesh) but Sebastian's hair, lantern and gun still show.
- Only hand-tested so far. No automated test suite.
- There was one unexplained "R6025 pure virtual function call"; a handler now logs the call stack if it recurs.

## Build

Windows, [MSYS2](https://www.msys2.org/) UCRT64 with clang, CMake and Ninja:

```
pacman -S mingw-w64-ucrt-x86_64-clang mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja
build.bat            (release)   build.bat debug
```

Output: `build\release\ewcoop.dll` and `build\release\inject.exe`.

## Use

See [HOW_TO_PLAY.txt](HOW_TO_PLAY.txt). Short version:

1. Start the game from Steam and load a save. Run `inject.exe` (it injects every running copy).
2. Host: **Ctrl+Shift+K**. Client: put the host's address in `ewhost.txt` next to the DLL, then **Ctrl+Shift+O**.
3. Solo test of the whole netcode: **Ctrl+Shift+E** (player 2 replays you with delay and packet loss).

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
[Co-op Anything](https://github.com/cranebeaker/coop-anything) playbook. MIT licensed (see [LICENSE](LICENSE)).
The Evil Within is a trademark of ZeniMax / Tango Gameworks; this project is unaffiliated and contains no game data.
