# Doom maps as Lyra arenas

DoomUE can turn a Doom map into a level for Epic's **Lyra Starter Game**, playable with Lyra's
Shooter Core: its heroes, weapons, abilities, teams, bots and HUD. E1M1 has been built this way as
`/Game/Maps/L_DoomE1M1` in the Lyra project. A Play-menu tile for it,
`/Game/System/Playlists/DA_DoomE1M1`, starts it as an Elimination match.

## What the level uses from Lyra

| Doom | Lyra |
|---|---|
| Walls, floors, ceilings | Static meshes with Lyra's example-content materials, chosen by surface role: panel walls (`MI_MS_Surface_Wall_1`), tile floors (`MI_Tile_1`), matte ceiling panels (`MI_MS_Surface_Wall_2`), metal trim (`MI_MS_Basic_Metal`), orange doors (`MI_MS_Orange_1`), red switches and exits (`MI_MS_Red_1`), glowing light fixtures (`MI_Glass_Tile`), blue liquids (`MI_MS_Blue_2`) |
| Sky | Open to Lyra's sky: a sun on the sky atmosphere, a real-time sky light, volumetric clouds and height fog, Lumen. Doom's sky surfaces stay as an invisible collision shell, so nobody jumps out of the map |
| Lamps and bright rooms | Point lights in the lamps' colours, soft room lights |
| Player 1-4 and deathmatch starts | `LyraPlayerStart` |
| Shotgun, super shotgun | `B_WeaponSpawner` with `WeaponPickupData_Shotgun` |
| Chaingun, rocket launcher, plasma, BFG | `B_WeaponSpawner` with `WeaponPickupData_Rifle` |
| Chainsaw | `B_WeaponSpawner` with `WeaponPickupData_Pistol` |
| Medikit, soulsphere, megasphere, berserk / armour | `B_WeaponSpawner` with `WeaponPickupData_Heal_Big` / `_Heal_Small` |
| Doors | Left open (Lyra has no Doom doors) |
| Lifts | Lowered, with a `B_Launcher_Up` launch pad on each |
| Teleporters | `B_Teleport` pairs: a sender on the teleport line, a receiver at the destination |
| Steps up to 24 map units (72 cm) | Invisible collision ramps (a Lyra character steps 45 cm) |
| Impassable lines (window ledges, railings) | Invisible walls from the lower floor to the higher ceiling |
| Monsters | Lyra's bots, from the experience |
| — | A nav mesh over the whole map (also rebuilt at runtime), and the Elimination experience in World Settings |

## Building it again (or another map)

1. **Export from DoomUE.** The map's geometry is built by DoomUE's own level builder, with doors
   opened and lifts lowered.

   ```bash
   "C:/Program Files/Epic Games/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" "E:/Work/SessionFolderUnrealClaude/DoomUE.uproject" -run=DoomContent -exportlyra=E1M1 -out=E:/Work/LyraStarterGame/Saved/DoomExport/E1M1
   ```

   This writes OBJ tiles (grouped by surface role, Unreal centimetres, planar UVs at 4 m), a
   collision-only `E1M1_Collision.obj` (step ramps, impassable-line walls and the sky shell, wound
   to face the player), and `E1M1.json`. The JSON lists the starts, pickups, lifts, teleporters,
   lights and bounds.

2. **Build the level in Lyra.** This needs the real Lyra editor, because the `pythonscript`
   commandlet has no editor level to place actors in. Starting on an empty map avoids loading
   Lyra's default overview map.

   ```bash
   "C:/Program Files/Epic Games/UE_5.8/Engine/Binaries/Win64/UnrealEditor.exe" "E:/Work/LyraStarterGame/LyraStarterGame.uproject" /Engine/Maps/Entry -ExecutePythonScript="E:/Work/SessionFolderUnrealClaude/Tools/Lyra/build_doom_level.py"
   ```

   Set `DOOM_EXPORT` to another export folder for another map, and `DOOM_QUIT=1` to close the
   editor when done. The script imports the tiles into `/Game/DoomArena/<MAP>/Meshes`. The OBJ
   importer mirrors Y, so the script pre-mirrors it. It assigns the materials, sets per-polygon
   collision, places the actors, rebuilds navigation, and saves the level and the playlist tile.

3. **The `DoomLyraTools` plugin** (`Tools/Lyra/DoomLyraTools` here; copy it to the Lyra
   project's `Plugins` folder and build Lyra once) provides `SetPropertyFromText` and `ListProperties`. Python cannot set `ALyraWorldSettings::DefaultGameplayExperience`
   (it is `EditDefaultsOnly`) or Blueprint variables whose names contain spaces (such as the launch
   pad's *Launch Velocity*); the plugin sets them through reflection.

4. **Checking it.** The plugin's runtime module saves in-engine screenshots and quits, without
   capturing the desktop. It also logs where every pawn is, which is how a falling-through-the-floor
   problem shows up.

   ```bash
   "C:/Program Files/Epic Games/UE_5.8/Engine/Binaries/Win64/UnrealEditor.exe" "E:/Work/LyraStarterGame/LyraStarterGame.uproject" /Game/Maps/L_DoomE1M1 -game -doomshot=50 -doomquit=70
   ```

   Verified on E1M1:
   * The Elimination experience loads from World Settings, and the warm-up and playing phases run.
   * The hero and three bots stand on the Doom floors and fight. The red team scored a kill
     within the first minute.
   * After 75 seconds, all four pawns are still on Doom floors: none has walked through a wall or
     an impassable line, or jumped out through the sky.

## Notes

* Collision is per-polygon and one-sided, so winding matters. DoomUE's level builder winds walls
  the other way from floors (its materials are two-sided, so it never shows). The exporter
  therefore winds every triangle to face its normal. Before that, floors held but bots walked
  through walls into the void between rooms and fell out of the world.
* The OBJ importer mirrors Y and keeps the face winding, so the script mirrors positions back and
  leaves the faces alone. Reversing every face makes the floors face down instead.
* The launch pads keep `B_Launcher_Up`'s default launch. Its only tuning variable is `UpOverride`
  (250 by default, found with `ListProperties`). The default already clears E1M1's two lifts.

* Lyra 5.8's editor sometimes crashes inside the Niagara editor while it loads (before any script
  runs). Running the command again works.
* Doom's 3x scale fits a Lyra mannequin: Doom's 56-unit player is 168 cm, doors 216 cm and higher.
* Things without a Lyra counterpart (ammo, keys, decorations, barrels) are left out. Doom's
  monsters are replaced by Lyra's bots.
