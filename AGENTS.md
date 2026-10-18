# Notes for agents

A handover for the next agent (AI or human) working on this repository. Read [README.md](README.md)
for what the port does; this file covers how to work on it, and the traps already found.

## The project

**DoomUE** is a C++ Unreal Engine 5.8 port of DOOM 1 and 2 that plays any WAD (Freedoom's are in
`Content/WADs`). The repository is public: https://github.com/mariuz/unreal-doom. The owner's earlier
Pascal port, mariuz/castle-doom, is the reference for map geometry (E1M1: 682 subsectors, area
6,712,686).

* `Source/DoomUE`: the game module. `UDoomWorld` (a world subsystem) is the Doom simulation;
  `ADoomGameMode`, `ADoomGameState` and `ADoomPlayerState` follow Unreal's gameplay framework;
  `ADoomLevel` builds the geometry. See [docs/Architecture.md](docs/Architecture.md).
* `Source/DoomUEEditor`: the editor module. `-run=DoomContent` generates materials, effects and the
  game-table asset, and imports maps; the automation tests live in `Private/Tests`.
* `docs/`: Architecture, Testing, Multiplayer, EditingMaps, Lyra. Keep them current with changes.
* `Tools/`: `run_tests.bat`, test-WAD generators, the Lyra export scripts and plugin.
* The roadmap is the "Not done yet" list at the end of the README.

## Building, running, testing

```bash
"C:/Program Files/Epic Games/UE_5.8/Engine/Build/BatchFiles/Build.bat" DoomUEEditor Win64 Development -Project="E:\Work\SessionFolderUnrealClaude\DoomUE.uproject" -WaitMutex -NoHotReload
```

```bash
"C:/Program Files/Epic Games/UE_5.8/Engine/Binaries/Win64/UnrealEditor.exe" "E:/Work/SessionFolderUnrealClaude/DoomUE.uproject" -game -warp=E1M1 -autotest=5 -nomusic -windowed -resx=960 -resy=540
```

```bash
Tools/run_tests.bat DoomUE -NullRHI
```

* **Tests:** 12 automation tests (see [docs/Testing.md](docs/Testing.md)), all passing at the time
  of writing.
  * Run them before merging.
  * Run without `-NullRHI` when rendering changes: that also compares the screenshots.
  * The `-savetest`, `-exittest`, `-patchtest` and similar switches still work in `-game` runs.
  * `-playtest` (with `-warp`) is the scripted play-through of the interactive features: run it on
    E1M1 (and Freedoom 2's MAP01) after gameplay changes; every step should PASS or SKIP. Its steps
    are in `DoomPlayTest.cpp`, listed in [docs/Testing.md](docs/Testing.md).
    * It can't press keys through Enhanced Input (unfocused window; injected actions don't arrive
      either): drive the game through `UDoomWorld` / the HUD instead.
    * `-savetest` doesn't quit by itself; kill only your own test process, never the owner's editor.
  * The engine may live elsewhere: `UE_ENGINE=F:\EpicGames\UE_5.8\Engine` for `run_tests.bat`.
  * `E1M1Modern` compares Lumen lighting: on a GTX 950 with UE 5.8.2 it fails (brighter, 13.8%)
    with the same picture, with or without any changes, so compare against a run without yours. Not
    a settling problem (600 frames change nothing); suspected engine patch (approved on 5.8.1) or GPU.
  * Make steps wait for conditions, not fixed times: background work such as distance-field builds
    can slow the editor a lot.
* **Screenshots** land in `Saved/Screenshots/WindowsEditor/`.
  * `-autotest[=s]` saves `autotest_<map>.png` and quits.
  * Check rendering by looking at these images, never by capturing the desktop.
* **Build times:**
  * A change to a widely included header (`DoomInfo.h`, `DoomWorld.h`) rebuilds most of the game
    module, 5–10 minutes.
  * MSVC occasionally hits an internal compiler error (C1001) on these big rebuilds. Running the
    build again has fixed it every time.
* **Logs:** `Saved/Logs/DoomUE.log`. Grep it for `LogDoom` and for the test switches' `PASS` / `FAIL`
  lines.

## Working conventions the owner uses

* For each roadmap item:
  * work on a feature branch;
  * update the README (status and roadmap) and the docs;
  * merge into `main` with `git merge --no-ff`, then delete the branch.
* **Push only when the owner says "push".**
* End commit messages with the `Co-Authored-By:` line for the model that made the change.
* **The owner often has the DoomUE editor open.** Its Live Coding blocks command-line builds
  ("Unable to build while Live Coding is active").
  * Never kill the editor.
  * Prepare the changes and ask the owner to close it.
* **Privacy:** don't take screenshots of the desktop (it shows the owner's browser and other apps).
  Use in-engine screenshots only.
* Decisions waiting on the owner:
  * Roadmap item 3, DeHackEd frame and code-pointer edits, needs Doom's GPL state table. Bringing
    it in is the owner's licensing call.
  * Ask before adding third-party content.

## Tooling traps (Windows, Git Bash)

* **Git Bash mangles `/Game/...` arguments** into Windows paths. Prefix the command with
  `MSYS_NO_PATHCONV=1`.
* Inline Python heredocs break on some quoting and on `\r\n`. For non-trivial edits, write the
  script to a file first.
* Line endings are mixed: the repo stores LF, and git warns about CRLF. Don't introduce stray `\r`
  characters with `sed`.
* `UnrealEditor-Cmd -run=...` commandlets can't spawn actors into a level. Editor-world work needs
  the real editor with `-ExecutePythonScript=`.

## Code traps already found (don't reintroduce them)

* **The project's collision channels default to Block** (`DoomMonster`, `DoomMissile` in
  `DefaultEngine.ini`), and engine profiles such as `OverlapAllDynamic` don't list them: set both
  responses explicitly on anything that must not stop monsters or missiles.
* **Moving floors' collision lags the floor by a few tics** (async cooking): don't treat an actor
  resting on a moving sector as settled (corpses wait for the sector to stop and a second's rest).
* **Corpses and one-frame things sleep** (`ADoomMonster::WakeCorpse`,
  `UDoomSpriteComponent::bSleepWhenDone` / `bSleepWhenStatic`): anything new that moves a corpse
  must wake it first; `PlaySequence` wakes a sprite. Keep the HUD deterministic (no `FMath::Rand`):
  the classic screenshot is compared pixel for pixel.

* **Game tables are `USTRUCT`s with `FString` fields** (`DoomInfo.h`).
  * Test for empty fields with `.IsEmpty()`. `*Def->Field` compiles but is always true.
  * Compare names with `.Equals()`.
  * Pass `*Field` to `%s`.
  * The base tables come from `Content/Doom/Data/DA_DoomGameData`; DeHackEd patches apply on top.
* **Collision is cooked once per chunk** (`ADoomLevel::Commit`). The procedural mesh recooks the
  whole component on every section change. Don't create sections with collision one by one: that
  was 1.2 s of every load.
* **Winding:** DoomUE's walls are wound against their normals. Its two-sided materials hide that,
  but one-sided materials and one-sided collision don't. The Lyra exporter and LyraDoom both
  rewind triangles to face their normals.
* **Map loading is staged** (`UDoomWorld::BeginLoad` / `StepLoad`).
  * Pictures decode and chunks generate on worker threads, so the picture caches must be filled
    (`FDoomGraphics::CachePrefetch`) before `GenerateChunks` runs.
  * `LoadMapAsync` is used for new games and level transitions; `LoadMap` is synchronous.
* **`InitWads` replaces the graphics**, so it unloads any loaded map first. That fixed a crash the
  tests found.
* **Tile sets:** packaged builds can't build meshes at runtime.
  * `ADoomLevel::FindSavedTiles` first tries an imported level's own tiles, then the WAD's tile set
    (`UDoomMapTiles` under `/Game/Doom/Tiles/<wadset>/<MAP>`).
  * Both are used only when the static chunk's signature matches.
  * `Tools/package.bat` bakes all of them before packaging. They're git-ignored.
  * Anything that changes generated geometry changes the signatures, so re-bake after it.
* **Automation:**
  * In the editor, `AutomationOpenMap` also starts PIE; don't add another `FStartPIECommand`.
  * The info tables are global, so tests reset them afterwards.
  * Screenshot tests stop Doom's clock with `ADoomGameMode::bClockStopped` and run exact tics.
    Never pause the game: that freezes the camera. Material globals must keep updating while the
    clock is stopped.

## Related projects on this machine

* **Lyra Starter Game** at `E:/Work/LyraStarterGame`.
  * E1M1 was converted into `/Game/Maps/L_DoomE1M1` there, an Elimination map; see
    [docs/Lyra.md](docs/Lyra.md).
  * Rebuild it with `-run=DoomContent -exportlyra=E1M1` here, then `Tools/Lyra/build_doom_level.py`
    in the Lyra editor.
  * The `DoomLyraTools` plugin is copied into Lyra's `Plugins` folder.
  * Lyra's editor sometimes crashes inside the Niagara editor at startup; run it again.
* **LyraDoom** at `E:/Work/LyraDoom`.
  * A separate project: DoomUE's game module plus role-based Lyra materials, an open sky and room
    lights.
  * It has its own local git repository with no remote.
  * Its Lyra assets (35 packages, about 235 MB) are git-ignored. `Tools/copy_lyra_assets.py`
    copies them back.
  * **Never commit Epic's Lyra content to a public repository.**
  * Its code changes (role materials, open sky, winding fix) aren't in this repository yet.
    Upstreaming them without the assets is a candidate task.

## Where things stand

* **Done, newest first:**
  * sleeping corpses and one-frame things (no movement, actor or sprite ticks at rest; one batched
    facing pass), and pickups no longer block monsters and missiles;
  * the audit's remaining vanilla differences: specials' timing and sounds, autoaim and the BFG,
    floatok, pain elemental and lost soul deaths, automap controls, INTERPIC for episode 4, Esc;
  * play-test fixes from an audit against vanilla 1.9, with `-playtest` (doors, lifts, teleports,
    splash damage, refire, monster walking, movement feel, automap, prompts, par times);
  * tile sets for maps played straight from the WAD;
  * automation tests;
  * game tables as an editable data asset;
  * asynchronous map loading with a loading screen;
  * the Lyra map fixes;
  * Lyra export;
  * packaged builds with saved tiles;
  * co-op and deathmatch;
  * the gameplay framework split;
  * editable maps in the editor;
  * Niagara effects and static-mesh tiles;
  * palette-exact rendering.
* **Not done:** see the README's roadmap. The interactive features pass the scripted play-test but
  have still never been played by a human, so expect tuning issues in feel and HUD offsets.
