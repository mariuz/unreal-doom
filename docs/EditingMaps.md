# Editing Doom maps in the Unreal editor

Every map in the WAD can become a normal Unreal level that you edit with the usual tools and
play with **Play**. The walls' shapes still come from the WAD, so draw new rooms in a Doom map
editor. Here you edit everything on top of them: the things; the sectors' heights, light, flats,
specials and tags; and the lines' specials, tags, flags and textures.

## Opening a map

* In the level editor, **Doom > Open / Import Map** lists the WAD's maps (Project Settings >
  Game > Doom sets the IWAD and PWADs). The first time you pick a map it is saved as
  `/Game/Doom/Maps/<MAP>.umap`; after that the menu just opens it. Maps not yet imported are
  marked *(import)*.
* Headless: `UnrealEditor-Cmd DoomUE.uproject -run=DoomContent -importmap=E1M1,E1M2` (or
  `-importmap=all`). Add `-force` to replace levels that already exist.

## What the level contains

| Actor | Outliner folder | Purpose |
|---|---|---|
| **DoomMap** (`ADoomMapActor`) | `Doom` | Names the WAD map and draws it in the viewport. *Preview Modern* switches the preview to the lit materials, and *Show Preview* hides it |
| **Doom Thing** markers (`ADoomThingMarker`) | `Doom/Monsters`, `Doom/Items`, `Doom/Decorations`, `Doom/Starts` | One per thing, drawn with its own sprite and an arrow for its facing |
| **Doom Sector Edit** (`ADoomSectorEdit`) | `Doom/Sector Edits` | Overrides for the sector the actor stands in |
| **Doom Line Edit** (`ADoomLineEdit`) | `Doom/Line Edits` | Overrides for the line it was placed by |

## Things

* Move, rotate, duplicate (Alt-drag or Ctrl+D) or delete markers like any actor. The actor's
  yaw is the thing's facing.
* **Thing Type** lists every type the port knows, plus the player and deathmatch starts.
  **DoomEdNum** takes any number, for PWAD-specific types.
* **Skill**: *Easy* covers skills 1 and 2, *Medium* skill 3, and *Hard* skills 4 and 5.
  **Ambush** makes a monster deaf until it sees you. **Multiplayer Only** things are skipped
  in single player.
* Markers snap their feet to the floor of the sector they stand in.
* **Import Things** on the DoomMap actor replaces all markers with the WAD's own THINGS.
  Untick **Use Thing Markers** to play with the WAD's things and ignore the markers.

## Sectors

1. Look at a room in the viewport and press **Add Sector Edit** on the DoomMap actor, or place
   a *Doom Sector Edit* actor and drag it into the room. Its label shows the sector number.
2. **Read From Sector** copies the WAD's values into the fields.
3. Tick the values to override: **Floor Height**, **Ceiling Height** (map units), **Light**
   (0-255), **Floor Flat**, **Ceiling Flat** (lists of the WAD's flats), **Special** (for
   example 9 secret, 7 nukage) and **Tag** (links the sector to line specials).
4. The viewport preview rebuilds after each change.

## Lines

1. Look at the line in the viewport and press **Add Line Edit** on the DoomMap actor: it places a
   *Doom Line Edit* on the line nearest the camera, reads the line's current values and labels
   itself "Line N". Moving the actor makes it edit the line it now stands by.
2. **Read From Line** copies the WAD's values into the fields again.
3. Tick the values to override:
   * **Special** (1 door, 11 exit, 62 lift, 97 teleporter...; 0 removes it) and **Tag** (the
     sectors it works on);
   * **Flags**, all together: *Impassable*, *Block Monsters*, *Secret* (one-sided on the automap,
     monsters can't use it), *Block Sound*, *Not On Automap*, *Upper Unpegged*, *Lower Unpegged*;
   * the **Front** and **Back** sides' *Upper*, *Middle* and *Lower* textures, from the WAD's list
     (`-` for none). A side the WAD shares between several lines is copied first, so only this line
     changes.
4. The viewport preview rebuilds after each change.

## What the specials do (viewport overlay)

With **Show Connections** on the DoomMap actor (on by default), the preview draws:

* a line from every trigger (switch, walk-over or shoot line) to the middle of each sector it tags,
  drawn over the walls and coloured by what it does: doors blue, lifts and platforms cyan,
  teleporters pink, crushers red, floors, ceilings and stairs orange, lights yellow;
* the outlines of those sectors in the same colours (a manual door's sector behind the door line
  too), secrets in magenta and damaging floors in green;
* a red cross on any trigger whose tag matches no sector;
* where doors open to and lifts lower to (Doom's rules: the lowest neighbouring ceiling - 4, the
  lowest neighbouring floor): a dimmer copy of the sector's outline at that height, with a post at
  each corner, drawn over the walls.

## Checking a map

**Check Map** (on the DoomMap actor, or **Doom > Check Map**) lists the map's problems, with your
edits applied, on the *Doom Map Check* page of the Message Log. Clicking an entry's actor selects
it (a thing's marker, a line or sector edit); **Go there** moves the camera to the spot.

* **Errors**: a special whose tag no sector has; a teleporter with no destination in its sectors; a
  door special on a one-sided line; an unknown texture name; a one-sided wall with no middle
  texture; a sector that isn't closed or whose ceiling is below its floor; a thing outside the map;
  a blue, yellow or red lock with no key of its colour on some skill; no player 1 start.
* **Warnings**: a special with tag 0 (in Doom it works on every untagged sector); a missing lower
  or upper texture where the floor or ceiling steps (maps sometimes leave these out on purpose); a
  monster too tall for its room; a thing on no skill level; an unknown thing type; no exit line
  (boss maps excepted).
* **Notes**: switch textures on lines with no special (often decoration), tags with no special,
  missing co-op starts.

Headless: `UnrealEditor-Cmd DoomUE.uproject -run=DoomContent -checkmap=E1M1,E1M2` (or `all`; add
`-iwad=freedoom2.wad` for another IWAD) writes the same list to the log. Freedoom's 68 maps have no
errors; their warnings are missing textures.

## Baked static meshes

Importing a map also saves its static geometry (everything that never moves) as static mesh
tiles, 1024 map units square, in `/Game/Doom/Maps/<MAP>_Tiles/`. The DoomMap actor lists them
under *Baked*, together with a signature of the geometry they were made from.

* The modern renderer draws the level with these tiles, so Lumen gets mesh distance fields and
  surface cards. Each slot is named after its texture and gets the level's own material
  instance at runtime (sector light, animation).
* Packaged games cannot build meshes at runtime, so these saved tiles are how a packaged game
  gets distance fields. Without them it draws the procedural geometry.
* After changing sectors (heights, flats), press **Bake Static Meshes** on the DoomMap actor and
  save the level. Tiles that no longer match the geometry are ignored, with a warning in the log.
  Editor runs then bake fresh tiles at load instead.

## Playing

**Play** (in the editor or standalone with the level's path) starts the level's map. The game:

1. loads the WAD map,
2. applies every sector edit and line edit,
3. spawns the things from the markers,
4. runs everything else, including lines, specials and the following maps, from the WAD.

Exiting goes on to the next map from the WAD.

## Checking it from the command line

* `-run=DoomContent -edittest=E1M1` opens the level as the editor does and builds the preview.
  It then raises the start sector's floor by 48 with a sector edit, turns an imp into a baron,
  and saves `/Game/Doom/Tests/E1M1_Edit`. The log reads `Edittest PASS`.
* `UnrealEditor DoomUE.uproject /Game/Doom/Tests/E1M1_Edit -game -autotest=20` plays it. The
  log line `E1M1: 1 sector edits, 292 thing markers from the level` confirms the edits were
  used.
