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

* **Tests:** 11 automation tests (see [docs/Testing.md](docs/Testing.md)), all passing at the time
  of writing.
  * Run them before merging.
  * Run without `-NullRHI` when rendering changes: that also compares the screenshots.
  * The `-savetest`, `-exittest`, `-patchtest` and similar switches still work in `-game` runs.
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
  * Roadmap item 1, DeHackEd frame and code-pointer edits, needs Doom's GPL state table. Bringing
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
* **Not done:** see the README's roadmap. Interactive features (doors, lifts, weapons, monster AI,
  menus) are implemented but have never been play-tested by hand, so expect tuning issues there.
