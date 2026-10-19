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

## The map editor (in the game and in Play In Editor)

A Doom map editor runs over the game itself:

* **Opening it:** **MAP EDITOR** in the main menu, `DoomEdit` in the console, `-mapeditor` on the
  command line, or **Doom > Play in the Map Editor** in the Unreal editor, which starts Play In
  Editor with the map editor open.
* **What it edits:** the map being played (a new random map if none is), with an imported level's
  own edits applied.
* **The view:** a top-down map (Slate: `SDoomMapView`) under UMG panels built in C++
  (`UDoomEditorWidget`): a toolbar, the selection's properties and a status line.

**Modes and keys:**

| Mode | What it does |
|---|---|
| **1 Things** | Select, drag, edit type, angle and skill flags; **T** or a double-click adds the chosen type at the cursor |
| **2 Sectors** | Select; edit floor, ceiling, light, special, tag and flats; delete |
| **3 Lines** | Select; edit special, tag, flags, the textures of both sides and the front offsets (a side shared with other lines is copied first) |
| **4 Vertices** | Select, drag, edit X / Y |
| **D Draw sector** | Click the corners; click the first one again, double-click or press Enter to close |

Drawn sectors (`DoomMapEdit::DrawSector`) may lie:

* in the void;
* inside a sector: their walls are then two-sided to it, and they start as a copy of it;
* against existing walls: those walls become openings between the two sectors.

Corners that land on lines split them.

Other keys and actions:

* Ctrl+Z / Ctrl+Y undo and redo (100 steps).
* Del deletes.
* **G** cycles the grid (1, 8, 16, 32, 64).
* The wheel zooms and the right mouse button pans.
* **Tab** switches to the 3D view: fly with WASD, Q / E and Shift, look with the right mouse
  button, and click to pick.
* **Check map** runs the map checker and goes to the first problem.
* **Save WAD** (Ctrl+S) writes `Saved/Maps/<map>.wad`.
* **Random map** asks the generator for a new map; **New room** starts a new map from one room.
* **F5 Play** closes the editor and plays the edited map from the camera's spot.

**How edits reach the level:**

* The game's clock stays stopped while editing.
* Every edit rebuilds the BSP with the node builder (when geometry changed) and reloads the level
  from the edited map (`UDoomWorld::LoadEditedMap`). Doors close and monsters return to their
  places.
* Edits live as long as the session. Save a WAD to keep them: a saved game reloads the map by its
  name, so it starts from the original.

**Testing:** `-warp=E1M1 -editortest` (also on generated maps) drives the editor through its API:

* raise a floor;
* draw a sector inside the start room and raise it;
* draw a room against a wall, which becomes an opening that a monster path crosses;
* place an imp, and undo and redo;
* check the map, save and read back the WAD;
* play.

Screenshots: `editor_2d.png`, `editor_3d.png`. All 10 steps pass on E1M1 and RND00005.

## The node builder

`FDoomMap::Rebuild()` makes a map's BSP (segs, subsectors and nodes) from its lines and sectors
alone, in the vanilla style: segs are the lines' sides, split where a partition crosses them, and a
convex set of segs becomes a subsector (no minisegs; the subsector polygons are clipped from the
partitions, as for a WAD's own nodes). The partition is the seg line with the fewest splits, then the
best balance, from up to 96 candidates per node. It is what generated maps and new geometry drawn
in the editors are built with; E1M1 takes about 35 ms.

* `-run=DoomContent -nodetest=all` (add `-iwad=freedoom2.wad`) rebuilds every map's nodes and
  compares them with the WAD's: the total floor area within 0.1% and the same sector at sampled
  points inside the map. All 68 Freedoom maps pass (E1M1: 6,713,392 against 6,712,686).
* `-buildnodes` in a `-game` run plays every map on the rebuilt nodes; `-warp=E1M1 -playtest
  -buildnodes` passes.

## Saving maps as PWADs

**Export WAD** on the DoomMap actor writes the map with the level's edits as a PWAD that any Doom
port or editor opens. The edits are the sector and line edits, and the thing markers.

* **Where:** `Saved/Maps/<MapName>.wad` unless **Export Path** says otherwise. The map goes in its
  own slot, or **Export Slot**'s; a generated map goes in as MAP01 or E1M1.
* **Format:** Doom's binary map format (`DoomWadWriter`): THINGS, LINEDEFS, SIDEDEFS, VERTEXES, SEGS,
  SSECTORS, NODES, SECTORS, REJECT (all zero) and BLOCKMAP.
* **Nodes:** fresh, from the node builder, on whole-unit vertices; split vertices are rounded as
  vanilla node builders round them.
* **Headless:** `-run=DoomContent -exportmap=RND0002A[,MAP01] [-out=path.wad]`.
* **Playing it:** `-file=path.wad -warp=E1M1`, here or in another port.
* **Tests:**
  * `-run=DoomContent -wadtest=all` (or `E1M1,RND00003`) writes each map and reads it back: the same
    lines, sides, sectors and things, the same sector at sampled points, no new checker errors. All
    36 Freedoom 1 maps and generated maps pass.
  * `-edittest` exports the edited E1M1 and checks that the edits are in the PWAD.

## Generated maps

`DoomMapGen::Generate` (`DoomMapGen.h`) makes a playable map from a seed:

* **Layout:** 8-12 rooms (`-genrooms=N` for 4-24) on a grid of cells, joined by a random tree of
  corridors, plus a few loops. The exit is in the room furthest from the start.
* **Keys:** up to three keyed doors (blue, yellow, red) on the way to the exit. Each key lies in a
  room reachable before its door, preferably a side room. Loops never join rooms on different sides
  of a keyed door.
* **Rooms:** floors differ by up to 64 units, and the corridors climb them in steps of 24 or less.
  Rooms get some outdoor (sky) ceilings, blinking and glowing lights, pillars, raised platforms and
  nukage pools. Corridors get doors and wide `BIGDOOR` doors.
* **The exit:** a lit alcove with the exit switch.
* **Things:**
  * player and deathmatch starts;
  * monsters that get tougher further from the start, thinned out on the easier skills (Doom 2's
    chaingunners, hell knights and revenants on Doom 2 IWADs);
  * the shotgun at the start, then the chaingun, super shotgun, rocket launcher and plasma gun;
  * ammo for the weapons found so far, health, armour and barrels.
* **Textures:** chosen from the IWAD's own textures and flats.
* **Geometry:** the layout is painted on 32-unit tiles, and the lines are the edges between tiles
  of different sectors, so every sector is closed. Door faces are fronted by the room or corridor,
  so they open from both sides. The node builder makes the BSP.
* **Validation:** before a map is accepted, a walk from the start (steps of 24 or less, 56 units of
  headroom, doors opened as keys are found) must reach every key and the exit. A layout that fails
  is retried with the next sub-seed.

**Playing:**

* A generated map is named `RND` plus five hex digits of its seed: `-warp=RND0002A` plays one.
* **RANDOM MAP** in New Game picks a new seed.
* Saves and network clients regenerate the map from its name.
* The exit leads to the next seed.
* The map borrows one of the IWAD's songs.

**Testing:** `-run=DoomContent -gentest=100[,first seed]` generates maps and runs each through the
map checker; add `-gendump` for `Saved/Gen/<name>.txt` (lines, sectors, things) to plot.

* 100 seeds pass on Freedoom 1 and 2 with no errors and no warnings, every one on its first attempt,
  in 3-8 ms each, with 15-25 BSP splits.
* `-warp=RND00002 -playtest` passes (lifts and teleporters skipped).

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
