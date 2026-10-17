# Automation tests

DoomUE's checks are Unreal **automation tests** (`Source/DoomUEEditor/Private/Tests/DoomAutomationTests.cpp`).
They run from the editor's **Session Frontend** (Tools > Session Frontend > Automation, filter
`DoomUE`) or headless, in CI:

```bash
Tools/run_tests.bat
```

```bash
Tools/run_tests.bat DoomUE -NullRHI
```

The first command runs everything, the screenshot comparisons included. The second needs no GPU;
the `DoomUE.Render` tests then skip with a warning. A group name runs only that group, for example
`Tools/run_tests.bat DoomUE.Game`. The script exits non-zero when any test fails, and writes an
HTML and JSON report to `Saved/Automation/Report`. Directly:

```bash
"C:/Program Files/Epic Games/UE_5.8/Engine/Binaries/Win64/UnrealEditor.exe" DoomUE.uproject -ExecCmds="Automation RunTests DoomUE; Quit" -unattended -nopause -NullRHI -testexit="Automation Test Queue Empty" -ReportExportPath=Saved/Automation/Report
```

## The tests

| Test | Replaces | Checks |
|---|---|---|
| `DoomUE.Map.E1M1` | the BSP area check | Freedoom's E1M1 has vanilla nodes, 682 subsectors with a total area of 6,712,686 square units (the Pascal port's reference, to rounding), 182 sectors and 1175 lines |
| `DoomUE.Map.NodeFormats` | `-maptest` | E1M1 rebuilt with XNOD, ZNOD, XGLN, ZGL2, XGL3, ZGL3 and glBSP v1, v2, v3 and v5 nodes, and converted to UDMF (`Tests/wads`): 682 subsectors and the same area each time (glBSP v1, with 16-bit vertices: 6,709,159) |
| `DoomUE.Patches.DeHackEd` | `-patchtest` with `test.deh` | Thing, Misc, Ammo, missile, `[PARS]` and `[STRINGS]` changes reach the game tables |
| `DoomUE.Patches.TextBlocks` | `-patchtest` with `text.deh` | Text blocks rename sprites and sounds, and replace a string found by its text |
| `DoomUE.Patches.BoomLumps` | `-patchtest` with `boom.wad` | `SWITCHES` and `ANIMATED` replace the built-in lists |
| `DoomUE.Patches.UMAPINFO` | `-patchtest` with `umapinfo.wad` | Level names, par time, music, sky and next map |
| `DoomUE.Game.SaveLoad` | `-savetest` | On E1M1 with a door moving, corpses, awake monsters and a rocket and a homing tracer in flight: save, scramble the world, load; the state fingerprint matches the saved one |
| `DoomUE.Game.ExitToNextMap` | `-exittest` | Two kills credited to the game state and the player state, the intermission, then E1M2 loaded in the background, with the counters reset and the shells carried over |
| `DoomUE.Editor.EditImportedMap` | `-run=DoomContent -edittest` | The imported E1M1 level opens, a sector edit raises the start room's floor, an imp marker becomes a baron, the level saves (`/Game/Doom/Tests`). It replaces the level open in the editor |
| `DoomUE.Editor.WadTiles` | `-run=DoomContent -baketiles` | Bakes E1M2's tile set, then plays E1M2 from the WAD in the modern renderer: the static chunk is drawn by the saved tiles (what packaged builds rely on) |
| `DoomUE.Render.E1M1Classic`, `E1M1Modern` | `-autotest` screenshots | E1M1's start in each renderer, compared with the approved images in `Test/Screenshots` |

The game tests play in PIE on `/Engine/Maps/Entry`, as a `-game` run would. Steps wait for
conditions, not for fixed times, because background work such as distance-field builds can slow
the editor down a lot. A test that creates meshes finishes their distance fields before it ends. Patches load through
`UDoomWorld::InitWads` with an explicit `FDoomWadSetup`, so no command-line switches are needed.
Because the info tables are global, each test resets them afterwards.

## Screenshots

Doom's 35 Hz clock is stopped (`ADoomGameMode::bClockStopped`) and the test runs exactly 35 tics,
so monsters, animations and flickering lights are in the same state every run. The engine keeps
rendering: pausing the game instead would freeze the camera too. The comparison uses Unreal's
*Low* tolerance. The classic renderer matches its approved image exactly, and the modern one
differs by about 0.07% (Lumen's noise).

When a change alters the picture on purpose, the test fails and the Session Frontend's
**Screenshot Comparison** tab shows the old and new images side by side; **Replace** approves the
new one. A missing image is reported as new: approve it there, or copy the files the run saved under
`Saved/Automation/Tmp/Entry/<Name>/<Platform>/<RHI>/` to the same path under `Test/Screenshots/`.
Approved images are per platform and RHI (`Windows/D3D12_SM5` here).

## The switches

The command-line switches (`-savetest`, `-exittest`, `-patchtest`, `-maptest`, `-autotest`, ...)
still work for a quick look in a standalone `-game` run. The automation tests share their code:
`UDoomWorld::RunSaveTest`, `FDoomMap::SubsectorArea` and `DoomContent::EditTest`.

## The scripted play-test

`-playtest` (with `-warp`) plays the interactive features through, one step after another, and
logs `Playtest <step> PASS/FAIL/SKIP: <details>`, then `Playtest: N passed, M failed, K skipped`
and quits. It stands in for the hand play-testing nobody has done yet, and caught most of the bugs
fixed on the `feature/playtest-fixes` branch: run against the code before that branch, 8 of its
first 11 steps failed on E1M1.

```bash
"C:/Program Files/Epic Games/UE_5.8/Engine/Binaries/Win64/UnrealEditor.exe" DoomUE.uproject -game -warp=E1M1 -playtest -nomusic -windowed -resx=960 -resy=540
```

| Step | Checks |
|---|---|
| `door` | A DR door (special 1) opens to the lowest neighbouring ceiling - 4, waits and closes to the floor |
| `lift` | A lift lowers to the lowest neighbouring floor, waits 3 s and comes back up |
| `teleport` | A teleporter (39/97) puts the player in the destination sector, freezes them briefly, and fires nothing else |
| `splash` | A 128-point blast 40 units away does 104 damage (128 - 40 + the player's 16-unit radius); 160 units away, none |
| `walk` | On the longest level run of the map: top speed about 16 units per tic, and a slide of about 160 units after letting go |
| `rate_*` | Shots in a fixed time with the trigger held: pistol every 14 tics, chaingun 2 per 8, plasma every 3, shotgun every 37, rockets every 20 |
| `flyer` | A cacodemon hit by the player is still flying a second later |
| `target` | 3 s of plasma at a baron in the open (aim kept on it) takes over 100 health |
| `chase` | 90% of the map's monsters, woken and set on the player, walk over 64 units in 5 s (the rest are shut in closets) |
| `automap` | With the map closed, the walls in view become known, beyond the player's own sector |
| `quitprompt` | A left click on the quit prompt cancels it |
| `renderer` | Switching the renderer from the options mid-level keeps the player, the kills and the corpses |
| `intermission` | The level's vanilla par time is known; screenshot `playtest_intermission.png` |

Steps that need something the map lacks (a lift, a teleporter, room in front of the start) are
skipped. Checked maps: Freedoom 1 E1M1, E1M2 and E1M4; Freedoom 2 MAP01 and MAP05 (add
`-iwad=freedoom2.wad`). The steps live in `Source/DoomUE/Private/DoomPlayTest.cpp`.
