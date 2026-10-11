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
| `UDoomWorld` | `UTickableWorldSubsystem` | The simulation of one tic (`RunTic`): sectors, movers, lights, specials, monsters, things, missiles, the brain. Also the WAD, graphics, sound, map, save games and the menu model | `P_Ticker` and the `p_*.c` files |

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

### Towards co-op and deathmatch

Already in place: the per-player inventory and credit, replicated properties on both states,
and the imported player 2 to 4 and deathmatch starts (`ADoomThingMarker` types 2, 3, 4 and 11).
Still to do:

* Replicate weapons and keys. They are `bool` arrays, which reflected properties cannot hold;
  send them as bit masks.
* Spawn a pawn per player at their start. `UDoomWorld::GetPlayer()` assumes player 0.
* Run the simulation on the server and replicate monsters, things and movers. `UDoomWorld`
  state is not replicated yet.
* Add the co-op columns to the intermission (per-player credit), deathmatch frags, and respawning.

## Rendering and assets

* `ADoomLevel` builds the geometry: procedural mesh chunks, plus static-mesh tiles with
  distance fields in the modern renderer.
* `DoomGraphics` turns the WAD's pictures into textures. The editor module generates the
  materials and Niagara effects in code (`-run=DoomContent`).
* See the [README](../README.md#two-renderers) for the classic (palette-exact) and modern
  (Lumen) renderers.

## Editor

Doom maps can be edited as Unreal levels; see [Editing maps](EditingMaps.md). The pieces:

* `ADoomMapActor`: a subclass of `ADoomLevel` that previews the WAD map in the viewport.
* `ADoomThingMarker`: one per placed thing.
* `ADoomSectorEdit`: per-sector overrides.
* `DoomEditorData`: the editor's copy of the WAD.
* The `DoomUEEditor` module: the **Doom** menu and the import.
