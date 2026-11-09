# Engine upgrades

How to move DoomUE to a new engine version (5.9 next), and what is known about Unreal Engine 6.

## The checklist

Do it on a branch (`engine/5.9`), with the old engine still installed so you can compare.

1. **Install the engine** from the Epic Games Launcher, and keep the current one.
2. **Run the whole check against it:**

   ```bat
   set UE_ENGINE=C:\Program Files\Epic Games\UE_5.9\Engine
   Tools\upgrade_check.bat
   ```

   It stops at the first step that fails:
   * build the editor and game targets;
   * regenerate the generated assets (materials, effects, AI and input assets);
   * rebake the imported levels' tiles and the WAD tile sets;
   * run data validation and the WAD checks (`-nodetest`, `-wadtest`, `-gentest`);
   * run the automation tests.

   Every tool in `Tools` reads `UE_ENGINE`, so `run_tests.bat`, `package.bat`, `analyze.bat` and
   `check_includes.bat` work against the new engine the same way.
3. **Fix the build.** Read the compiler's deprecation warnings (C4996) as well as the errors:
   * a warning is next version's error;
   * fix it with the replacement API the warning names;
   * don't silence it with `PRAGMA_DISABLE_DEPRECATION_WARNINGS`.

   The targets use `BuildSettingsVersion.Latest` and `EngineIncludeOrderVersion.Latest`, so the new
   engine's include order and build defaults apply at once.
4. **Regenerate, don't resave.** The assets are generated from code, and the WADs are the source
   (README, "Authored assets"), so upgrade them by rebuilding them with the new engine's API.
   Resaving the old files would keep whatever the old engine wrote. Afterwards, `git diff --stat
   Content` should list only regenerated assets:
   * `Content/Doom/Materials`, `Effects`, `AI`, `Input`;
   * `Maps/*_Tiles`.

   Two exceptions are not regenerated:
   * `Content/Doom/Data/DA_DoomGameData` may hold hand edits. Check it loads (the tests do); its
     `-gamedata -force` would discard the edits.
   * `Content/Doom/Maps/E1M1` holds an imported level's edits. Its tiles are rebaked; the level
     itself is upgraded on load and saved by the editor.
5. **Screenshots.** The `DoomUE.Render` tests compare against `Test/Screenshots/<platform>/<RHI>`.
   * Classic must match exactly. A classic difference is a bug.
   * Modern may change with a new Lumen, TSR or shadows. Look at the differences in the report
     (`Saved/Automation/Report/index.html`). Approve the new images only if they are
     improvements, and say which engine version approved them in the commit.
6. **Fragile spots:** the code most likely to break, roughly by risk:
   * The **custom HLSL nodes** in the generated materials (`Custom(...)` in
     `DoomContentMaterials.cpp`). Shader code is not checked until it compiles, and it is where
     Substrate and shader-model changes show first.
   * The **Niagara builder** (`DoomContentEffects.cpp`). It sets lightweight-emitter modules
     through reflection, by property name, and those names are not a stable API.
   * **Scalability levels** (`UDoomWorld::UpdateViewSettings`). Options > Quality assumes
     `BaseScalability.ini`'s level 1 keeps Lumen on (the irradiance volume) and sets
     `r.Upscale.Quality`. Read the new `BaseScalability.ini`. Names of cvars it sets:
     `sg.*`, `r.ScreenPercentage`, `r.Upscale.Quality`, `r.AntiAliasingMethod`.
   * **Mesh building**:
     * `UDoomLevelMeshComponent::CreateBodySetupHelper` (double-sided collision);
     * `FMeshNaniteSettings` (`ADoomLevel::FillTileMesh`);
     * `UStaticMesh::BuildFromMeshDescriptions` (`DoomAssets::SpriteQuad`).
   * **Test plumbing**: the key tests drive Slate (`FSlateApplication::ProcessKeyDownEvent`) and
     unbind `OnGameViewportInputKey`, and the screenshots use `UAutomationBlueprintFunctionLibrary`.
   * **CommonUI and Enhanced Input**: the menu's screen and the generated input assets.
7. **Performance.** Run `-perftest` on E1M1 and MAP29, for classic, for modern on SM5 and SM6, and
   for each quality preset, and compare with the tables in the README. Record the new numbers with
   the engine version.
8. **Packaged build.** Run `Tools\package.bat` and play the result. Some things have never been
   checked in a packaged build: the Dynamic Mesh level, the runtime sprite quad, SM6.
9. **By hand.** Play a level in both renderers, use the menu with keys, mouse and gamepad, import a
   map in the editor, and open the in-game map editor.
10. **Docs and version.**
    * Set `EngineAssociation` in `DoomUE.uproject`.
    * Change the default engine folder in `Tools/*.bat`.
    * Update the README's requirements and build lines.
    * Update the engine notes in AGENTS.md.
    * Merge as usual.

## Unreal Engine 6

### What is known (November 2026)

Epic's own pages could not be fetched automatically, so check them before relying on the details.

* **Announced by Epic** (Unreal Fest Chicago, June 2026; Epic's "The road to Unreal Engine 6" post):
  * UE 6 merges UE 5 and UEFN into one engine.
  * Gameplay programming moves to **Verse**, with a new gameplay framework built on it,
    **Scene Graph**, made of entities and components.
  * Early Access is planned for the end of 2027, and the full release 12 to 18 months later.
  * Blueprints and Actors were to be deprecated once the new framework is mature, with conversion
    tools.
  * glTF and USD become first-class formats.
* **Reported, not confirmed by an Epic page** (Unreal Fest Tokyo, 3 November 2026, from coverage of
  the keynote and press briefing):
  * Actors, Blueprints and the C++ gameplay framework are *removed* from UE 6 from Early Access
    on, instead of phased out.
  * C++ stays for the engine's lower layers (rendering, physics).
  * AI-assisted tools will convert Blueprints and C++ gameplay to Verse; their form is undecided.
  * Projects in production can stay on UE 5.
  * UE 5.9 enters Preview in November 2026.

Sources: [Epic, The road to Unreal Engine 6](https://www.unrealengine.com/news/the-road-to-ue-6),
[Game Developer, June 2026](https://www.gamedeveloper.com/programming/unreal-engine-6-will-merge-ue5-and-uefn-into-a-single-unified-engine-),
[Inven Global, November 2026](https://www.invenglobal.com/articles/28098/epic-games-will-remove-blueprints-and-actors-from-unreal-engine-6).

### What it means for DoomUE

If the Tokyo report holds, UE 6 is a port of the gameplay layer, not an upgrade. The runtime module
(`Source/DoomUE`, about 28,000 lines) by layer, measured in November 2026:

| Layer | Lines | Built on | In UE 6 |
|---|---|---|---|
| WAD tools plugin (`Plugins/DoomWadTools`): WADs, maps, nodes, PWAD writer, the generator | 3,000 | `Core`, `CoreUObject` only | Engine-neutral C++; the likeliest to carry over as is |
| Doom's game data (`DoomInfo` tables, `DA_DoomGameData`, DeHackEd, UMAPINFO) | part of below | data | Carries over as data |
| The world simulation (`UDoomWorld` and its parts: tics, sectors, specials, flow, saves) | 5,800 | a world subsystem, spawning and finding actors | Rules port; the actor lookups change |
| Things (`ADoomMonster`, `ADoomPlayer`, projectiles, Smart AI) | 5,100 | Actors, `ACharacter` movement, AI controllers, behavior trees | Rewritten as Scene Graph entities in Verse |
| Framework and UI (game mode, controller, game state, HUD, menu, finale, input) | 5,400 | the C++ gameplay framework, CommonUI, Enhanced Input | Rewritten |
| Rendering, audio, assets (level meshes, tiles, sprites, graphics, music) | 3,400 | components, Dynamic Mesh, materials, Nanite | Depends on what stays in C++ |
| In-game map editor | 5,300 | Actors, UMG | Rewritten |
| Editor module (`Source/DoomUEEditor`: generators, importers, tests) | 5,200 | editor APIs | Depends on UE 6's editor scripting (Python, MCP) |

### What to do now

These are low-regret steps: they help on UE 5 anyway, whatever UE 6 turns out to be.

* **Stay on the latest UE 5** (5.9 when it ships) with this checklist. Any conversion tool will
  start from the newest UE 5.
* **Keep everything generated.** Materials, effects, input, AI and tiles come from code and the
  WADs, so moving engines means regenerating them, not migrating them. Don't add hand-made assets
  without a generator.
* **Keep the WAD tools engine-neutral.** `DoomWadTools` depends on `Core` and `CoreUObject` only;
  keep it that way (AGENTS.md).
* **Keep Doom's rules in data and plain functions** (the info tables, `DA_DoomGameData`, the
  specials table) rather than spread across actor overrides. Data carries over; overrides don't.

### What not to do yet

* **No Verse rewrite.** Verse and Scene Graph aren't available in UE 5 outside UEFN, and the
  conversion tools aren't defined. A rewrite now would aim at an API that doesn't exist yet.
* **No big refactor** to separate the simulation from the actors on speculation. Decide that
  when the UE 6 Early Access shows what C++ a project may still have (late 2027).

### What to watch

* Whether a project can still have its own C++ modules (plugins, rendering extensions) in UE 6,
  and whether Verse can call them.
* The conversion tools: what they take (C++? Blueprints only?) and what they produce.
* How long UE 5 is supported, and whether 5.9 is the last UE 5.
* Epic's own pages, to confirm or correct the Tokyo report.
