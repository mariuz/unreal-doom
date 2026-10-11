# Editing Doom maps in the Unreal editor

Every map in the WAD can become a normal Unreal level that you edit with the usual tools and
play with **Play**. The walls and lines still come from the WAD, so change those in a Doom map
editor. Here you edit what sits on top of them: the things, and the sectors' heights, light,
flats, specials and tags.

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

## Playing

**Play** (in the editor or standalone with the level's path) starts the level's map. The game:

1. loads the WAD map,
2. applies every sector edit,
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
