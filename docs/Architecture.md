# Architecture

How the DoomUE port maps Doom's engine onto Unreal Engine 5.8's classes. For the full
Doom → Unreal table see the [README](../README.md#doom--unreal-engine).

## Gameplay framework

Unreal splits a game into **rules**, **shared state**, **per-player state** and **bodies**. In
a networked game the rules exist only on the server, and the states replicate to every client.
The port follows that split:

| Class | Unreal base | Owns | Doom equivalent |
|---|---|---|---|
| `ADoomGameMode` | `AGameModeBase` | Boot (WADs, title, `-warp`), the **fixed 35 Hz clock**, and the **level flow**: `ExitLevel`, `FinishIntermission`, `NextMapName` (UMAPINFO `next` / `nextsecret`, else Doom's episode and MAPxx order, secret levels included) | `G_Ticker`, `G_ExitLevel`, `G_WorldDone`, `G_DoCompleted` |
| `ADoomGameState` | `AGameStateBase` | `FDoomLevelFlow` (replicated): phase (title, menu, playing, intermission, finale), current map, level tic, kills / items / secrets and their totals, intermission tic, pending map, secret-exit flag | `gamestate`, `leveltime`, `totalkills`..., `wminfo` |
| `ADoomPlayerState` | `APlayerState` | `FDoomInventory` (health, armor, ammo, backpack, weapons, keys, ready weapon) and the player's own share of the level: `LevelKills`, `LevelItems`, `LevelSecrets`, `Frags` (replicated). `CopyProperties` carries the inventory through travel | `player_t` (inventory, `killcount`, `itemcount`, `secretcount`, `frags`) |
| `ADoomPlayer` | `ACharacter` | The marine's body: movement, weapon state machine, powerups, damage, pickups. `Inv()` returns the player state's inventory | `mobj_t` of the player, `P_PlayerThink` |
| `ADoomPlayerController` | `APlayerController` | Enhanced Input, menu and screen keys | `G_Responder` |
| `ADoomHUD` | `AHUD` | Status bar, weapon, automap, menu, intermission, title, finale | `ST_`, `HU_`, `AM_`, `WI_`, `F_` |
| `UDoomWorld` | `UTickableWorldSubsystem` | The simulation of one tic (`RunTic`, server and standalone only; network clients run `ClientTick` for animation and replicated geometry): sectors, movers, lights, specials, monsters, things, missiles, the brain. Also the WAD, graphics, sound, map, save games and the menu model. Its code is split by job: `DoomWorld.cpp` (setup, spawning, the tic), `DoomWorldSectors.cpp` (movers, lights, damage), `DoomWorldSpecials.cpp` (line activation, teleports), `DoomWorldFlow.cpp` (level flow, players, network), `DoomWorldPresentation.cpp` (sound, music, palette), `DoomSave.cpp`, `DoomFinale.cpp` | `P_Ticker` and the `p_*.c` files |

### The clock

`ADoomGameMode::Tick` runs in `TG_PostUpdateWork`, after the frame's movement and physics.
It adds the frame time (capped at 0.25 s) to an accumulator and runs whole tics:

* **Playing:** `UDoomWorld::RunTic()` for each 1/35 s, then
  `UDoomWorld::UpdateMaterialGlobals(SubTic)` for the material parameter collection (the
  fraction of a tic interpolates the animated globals).
* **Intermission and story screens:** their own accumulator advances `IntermissionTic` or
  `UDoomWorld::TicFinale()`.

`UDoomWorld::ResetClock()` (a new map, a loaded game) asks the game mode to start counting again.

### Credit

Kills, items and secrets are counted through `UDoomWorld::CreditKill(Killer)`,
`CreditItem(Player)` and `CreditSecret(Player)`. Each one adds to the game state's level totals
and to the earning player's `ADoomPlayerState`. A missile's kill goes to its shooter, and so
does a kill reported through the player's controller. A new map clears both.

### Save games

The save blob (version 4) keeps the level flow record and, through
`ADoomPlayerState::DoomSerialize`, the inventory and the player's credit. If a pawn has no
Doom player state (a custom game mode), it falls back to a local inventory and default credit,
so saves keep the same layout.

### Multiplayer

Co-op and deathmatch for up to 4 players run on this split; see [Multiplayer](Multiplayer.md).
In short:

* Only the server (where `ADoomGameMode` exists) runs tics.
* Clients build the map named by the replicated flow record.
* Changed sectors and sidedefs arrive through fast arrays on the game state.
* Monsters, things and missiles are replicated actors with replicated sprite state.
* Sounds and short effects are multicast.
* `UDoomWorld::GetPlayers()` covers every player, and `GetPlayer()` is the local one.

## Rendering and assets

* `ADoomLevel` builds the geometry: procedural mesh chunks, plus static-mesh tiles with
  distance fields in the modern renderer.
* `DoomGraphics` turns the WAD's pictures into textures. The editor module generates the
  materials and Niagara effects in code (`-run=DoomContent`).
* See the [README](../README.md#two-renderers) for the classic (palette-exact) and modern
  (Lumen) renderers.

### Game tables

Doom's info tables (`mobjinfo`, `weaponinfo`, the misc rules) are `USTRUCT`s in `DoomInfo.h`:
`FDoomThingDef`, `FDoomProjectileDef`, `FDoomWeaponDef` (with `FDoomWeaponFrame`s) and
`FDoomRules`. `UDoomGameData` (a `UPrimaryDataAsset`, `/Game/Doom/Data/DA_DoomGameData`) holds all of
them and is the version you edit. Game code goes through `DoomInfo::FindThing`, `Weapon`,
`FindProjectile` and `Rules`, which read the runtime copy:

1. `DoomInfo::ResetTables` (when the WADs load) starts from the built-in tables in `DoomInfo.cpp`.
   It then takes the asset's tables when `UDoomSettings::GameData` points at one. Weapons are
   matched by kind, so the asset can list only some.
2. DeHackEd and BEX patches (`DoomDehacked.cpp`) change that copy through `MutableThing`,
   `MutableProjectile`, `MutableWeapon` and `Rules`; the asset is never modified.

The editor module writes the asset from the built-in tables when it is missing, or with
`-run=DoomContent -gamedata -force`. Flags edit as checkboxes (`EDoomThingFlagBits`); frames are
sprite frame letters as text.

### Loading a map

`UDoomWorld` loads a map in steps (`BeginLoad`, then `StepLoad` until done):

1. **Game thread:** the map lumps, the level flow and the editor's sector edits; the level actor
   lays out its chunks (`ADoomLevel::BeginBuild`).
2. **Worker threads (`UE::Tasks`, `ParallelFor`):** every wall texture and flat the map draws,
   with animation frames and switch partners, is composed from its patches. Each gets colour and
   palette-index mip chains (`FDoomGraphics::DecodePrefetch`).
3. **Workers:** with those pictures cached, every chunk's geometry is generated in parallel
   (`ADoomLevel::GenerateChunks`). Meanwhile the game thread creates the GPU textures.
4. **Game thread:** the chunks' mesh sections and collision are committed, and the modern
   renderer bakes or reuses its tiles (`CommitChunks`). Then the things, the players and the music.

`LoadMapAsync` returns after step 1. `ADoomGameMode::Tick` then calls `StepLoad` with an 8 ms budget
per frame, and `ADoomHUD` draws the intermission's "Entering" screen with a progress bar. Starting a
new game, the next map after the intermission and the next map after a story text load this way.
`LoadMap` (the startup `-warp`, save games, network clients, the editor) runs the same steps without
returning in between, and still uses the worker threads.

Collision is cooked once per chunk. The procedural mesh component recooks its whole collision
whenever any section changes, so `ADoomLevel::Commit` adds the sections without collision, then
flags them and triggers one cook. Adding them one by one had cooked E1M1's static chunk 136 times:
1.2 s of a 1.4 s level build.

## Editor

Doom maps can be edited as Unreal levels; see [Editing maps](EditingMaps.md). The pieces:

* `ADoomMapActor`: a subclass of `ADoomLevel` that previews the WAD map in the viewport.
* `ADoomThingMarker`: one per placed thing.
* `ADoomSectorEdit`: per-sector overrides.
* `DoomEditorData`: the editor's copy of the WAD.
* The `DoomUEEditor` module: the **Doom** menu and the import.
