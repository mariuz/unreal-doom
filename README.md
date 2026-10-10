# DoomUE

DOOM 1 and DOOM 2 levels rendered and played in **Unreal Engine 5.8**, written in C++.
It reads the original WAD format directly. The project is modelled on
[Castle DOOM](https://github.com/mariuz/castle-doom), the Object Pascal port on
Castle Game Engine: Doom's data stays in Doom's own structures and runs Doom's 35 Hz game
logic, and each subsystem is presented through an Unreal Engine feature.

Freedoom Phase 1 and Phase 2 (BSD licensed, see `Content/WADs/COPYING.txt`) are bundled
and work out of the box. Your own vanilla-format `DOOM.WAD` / `DOOM2.WAD` and PWADs load
with `-iwad=` / `-file=`.

| Classic renderer, E1M1 | Doom 2 (Freedoom Phase 2) MAP01 |
|---|---|
| ![E1M1](docs/screenshot-e1m1-classic.png) | ![MAP01](docs/screenshot-map01.png) |

![Title](docs/screenshot-title.png)

## Doom → Unreal Engine

| Doom | Unreal Engine 5.8 |
|---|---|
| WAD lumps, `PLAYPAL`, `COLORMAP` | `FDoomWad` (plain C++), stacked IWAD + PWADs, last lump wins |
| Patches, `TEXTURE1/2` + `PNAMES`, flats, sprites | `FDoomGraphics` decodes to transient `UTexture2D`s (nearest filtering, box-filtered mip chain uploaded with `UpdateTextureRegions`) |
| `VERTEXES`, `LINEDEFS`, `SIDEDEFS`, `SECTORS`, BSP `NODES/SEGS/SSECTORS` | `FDoomMap`: Doom units and records; subsector polygons built by clipping the map bounds through the BSP. Vanilla nodes, ZDoom extended nodes (`XNOD`/`ZNOD`, `XGLN`/`ZGLN`, `XGL2`/`ZGL2`, `XGL3`/`ZGL3`; the `Z` variants inflated with the engine's zlib) and glBSP GL nodes (`GL_<map>` lumps, versions 1 to 5) all load into the same structures |
| Walls, floors, ceilings | `ADoomLevel`: `UProceduralMeshComponent` chunks (one static, one per movable sector, one per switch line), one section per texture, Doom's pegging rules, masked mid-textures in non-colliding components |
| Sector light, fake contrast, light diminishing | One texel per sector in a light texture updated each tic (`UpdateTextureRegions`), so flickering lights never rebuild meshes. A **Custom HLSL** node runs Doom's `scalelight` / `zlight` colormap selection per pixel from `PixelDepth` |
| Gun flash light, invulnerability, light amp | `UMaterialParameterCollection` (`MPC_Doom`) globals: `ExtraLight`, `FixedColormap`, `Tic` |
| Sky | Unlit material that projects the sky texture from the camera vector (1024 columns per turn, rows at slope `(100-R)/160`). Applied to sky ceilings and to the sky gaps Doom leaves undrawn |
| Animated flats/walls, scrolling walls (48), switches | `UMaterialInstanceDynamic` texture swaps (every 8 tics, or each group's own speed from a Boom `ANIMATED` lump); a `ScrollU` parameter; `SW1`/`SW2` pairs or a Boom `SWITCHES` lump (pairs up to the game's episode level) |
| `DEHACKED` lumps and `-deh` patches | `DoomDehacked`: Thing fields (health, speed, size, pain chance, missile speed and damage), Misc (starting health and bullets, caps, armor classes, BFG cells), Ammo (max and pickup sizes), BEX `[PARS]` and `[STRINGS]`. The WAD's strings name the levels (Freedoom's "E1M1: Outer Prison") and replace pickup messages. Frame, code pointer, sound and sprite edits are skipped (the port uses frame sequences, not Doom's state table) |
| `UMAPINFO` | `FDoomMapInfoSet`: level names and labels, music, sky, next / secret next map, par times, end of game, no intermission, story texts on a tiled backdrop, and boss actions (replacing the vanilla boss specials for that map) |
| Player | `ACharacter` + `UCharacterMovementComponent`: 16×56 capsule, eye at 41, 24-unit steps, Doom's heavy gravity. **Enhanced Input** actions and mapping contexts built in code, with gamepad support |
| Monsters | `ACharacter` possessed by an `AAIController`. Character movement does walking, stepping, ledge avoidance and flying. Decisions follow `p_enemy.c`: look, chase in 8 directions, new-chase-dir, melee/missile range checks, pain chance, infighting, door opening. Also the lost soul charge, arch-vile fire and resurrection, pain elemental spawning, revenant homing, mancubus spreads and boss-death specials |
| Line attacks, autoaim | `LineTraceSingleByChannel` / sphere sweeps, with Doom's `P_AimLineAttack` vertical window |
| Missiles | `UProjectileMovementComponent` (swept, built-in homing for revenant tracers) on a custom `DoomMissile` collision channel that ignores impassable lines, like Doom |
| Damage, splash | Unreal's damage framework: `ApplyPointDamage`, `ApplyRadialDamageWithFalloff` with a visibility check, `TakeDamage` overrides (armor absorption, pain, gibs, splash-immune bosses) |
| Impassable / monster-blocking lines | Invisible collision meshes. Monster capsules use a custom `DoomMonster` object channel that only the monster-block mesh responds to |
| Pickups | Overlap events on capsules |
| Sound effects (`DS*` DMX) | Converted to 16-bit PCM, played as `USoundWaveProcedural` with `USoundAttenuation` (full volume within 200 units, silent at 1200) and `USoundConcurrency` voice limiting |
| Music (`D_*` MUS or MIDI lumps + `GENMIDI`) | `FDoomSong` parses MUS (140 Hz ticks) and standard MIDI (tempo map) into timed events; `FDoomMusicPlayer` is an OPL-style FM synthesizer written for this port: 18 two-operator voices programmed from the WAD's GENMIDI bank (multipliers, ADSR envelopes in dB, 8 waveforms, feedback, FM / additive connection, key scaling, tremolo and vibrato, double-voice instruments, the percussion bank), with stereo panning. It renders on the audio render thread through a `USoundWaveProcedural` subclass on a 2D music `UAudioComponent`: level tracks, Doom 2's MAPxx table, intermission and title music |
| Save games (`p_saveg.c`) | Unreal's **SaveGame** system: `UDoomSaveGame` header (IWAD, map, skill, description, date) plus a versioned `FArchive` blob with sectors, lines, switch textures, movers, light effects, buttons, the player, monsters (AI state, animation, corpses, targets) and things. Loading rebuilds the map without its things, applies the state and rebuilds only the affected geometry. F6 / F9 quick save and load, six menu slots, `-loadgame=N` |
| Status bar, face, weapon sprites, messages, automap, intermission | `AHUD` drawing the WAD's own patches with `UCanvas` in Doom's 320×200 frame at 4:3 |
| Menu (`m_menu.c`) | `FDoomMenu` with the WAD's `M_*` graphics at Doom's positions and the blinking skull: main menu, episodes (and UMAPINFO episodes), skills with a Nightmare prompt, options (end game, messages, mouse sensitivity thermometer, sound volume, plus renderer / field of view / warp rows in the STCFN font), SFX and music thermometers that apply live, six load / save slots in the `M_LS*` boxes, Read This! pages and a quit prompt. F1 to F4 open help, save, load and sound; Y / N answer prompts; changes persist through `UDoomSettings::SaveConfig` |
| Endings (`f_finale.c`) | Episode and chapter texts (`E1TEXT`.., `C1TEXT`.. from the WAD's strings) typed out on their backdrop flats, end pictures (`CREDIT`, `VICTORY2`, `ENDPIC`), the bunny scroller (`PFUB2` to `PFUB1`, then `END0`..`END6`), and the Doom 2 cast call on `BOSSBACK`: each monster walks, attacks and dies when you fire, named by the WAD's `CC_*` strings. UMAPINFO `endpic` / `endbunny` / `endcast` choose the ending |
| MAP30's Icon of Sin | The spawn shooters (type 89) wake when they see you and spit cubes at the spawn targets (87) in turn; each cube hatches a monster with Doom's weighted pick; the brain's death sets off explosions before the level ends |
| Simulation | `UTickableWorldSubsystem` (`UDoomWorld`) running a fixed 35 Hz tic |
| Settings | `UDeveloperSettings` (Project Settings → Game → Doom) |
| Authored assets | **None checked in.** The editor module (`DoomUEEditor`) generates the 4 materials, the parameter collection and a default texture in code with `UMaterialEditingLibrary` on first editor launch, or `-run=DoomContent` |

### Two renderers

* **Classic** (default): unlit, with Doom's palette-space lighting: sector light, fake contrast,
  distance diminishing, full-bright frames, gun flash, and the invulnerability / light-amp colormaps.
  Manual exposure and `ToneCurveAmount = 0` keep the palette colours 1:1.
* **Modern** (`-modern`, or toggle in the menu): lit materials. Sector light becomes emissive
  ambient that **Lumen** GI and reflections spread. Procedural meshes support hardware ray tracing.
  There are dynamic shadow-casting point lights on projectiles, explosions, the muzzle flash and
  light-emitting decorations, plus volumetric `ExponentialHeightFog`, a sun, a real-time-captured
  `SkyLight` and bloom.

## Build and run

Requirements: Unreal Engine 5.8, Visual Studio 2022/2026 with the C++ game workload. Windows is
the platform this was built and tested on.

```bash
"C:/Program Files/Epic Games/UE_5.8/Engine/Build/BatchFiles/Build.bat" DoomUEEditor Win64 Development -Project="E:/Work/SessionFolderUnrealClaude/DoomUE.uproject" -WaitMutex
```

Open `DoomUE.uproject` in the editor once: it generates `Content/Doom/Materials/*`. You can also
generate them headless:

```bash
"C:/Program Files/Epic Games/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" "E:/Work/SessionFolderUnrealClaude/DoomUE.uproject" -run=DoomContent -force
```

Then press Play, or run standalone:

```bash
"C:/Program Files/Epic Games/UE_5.8/Engine/Binaries/Win64/UnrealEditor.exe" "E:/Work/SessionFolderUnrealClaude/DoomUE.uproject" -game -windowed -ResX=1280 -ResY=800
```

The first time each material permutation is used, Unreal compiles shaders on demand. It shows
"Preparing Shaders" and draws untextured surfaces for a few seconds to a few minutes (longer for
modern mode). A packaged build precompiles them. When packaging, `Content/WADs` is staged as
loose files.

### Command line

| Option | |
|---|---|
| `-iwad=freedoom2.wad` | IWAD in `Content/WADs` or an absolute path |
| `-file=a.wad,b.wad` | PWADs on top |
| `-deh=patch.deh[,...]` | DeHackEd / BEX patches after the WADs' own `DEHACKED` lumps |
| `-warp=E1M1` / `-warp=MAP07` / `-warp=7` | start on a map (otherwise the title screen) |
| `-skill=1..5` | |
| `-modern` / `-classic` | renderer |
| `-autotest[=seconds] [-autoyaw=deg] [-autopitch=deg]` | ignore input, screenshot to `Saved/Screenshots/.../autotest_<map>.png`, quit |
| `-nomips` | debug: no texture mip chain |
| `-nomusic` | silence the music |
| `-loadgame=N` | start from save slot N (0 = quicksave) |
| `-maptest=a.wad,b.wad[:MAP]` | load a map from each PWAD (over the IWAD) and log its node format and subsector area |
| `-patchtest` | log the values DeHackEd, UMAPINFO and Boom lumps changed |
| `-endtest [-endpress=N]` | with `-warp`: exit the map straight into its ending and press Enter N times (texts, pictures, bunny, cast) |
| `-brainawake` | wake MAP30's monster spawner at once |
| `-menupage=N [-menuitem=N]` | open a menu page (0 main, 2 skill, 4 options, 5 sound, 6 load, 7 save, 8 help, 9 quit) for screenshots |
| `-killtype=DoomEdNum` | 2 s in, kill every monster of a type (boss-action checks) |
| `-savetest` | with `-warp`: kill and wake some monsters, open a door, save, scramble the world, load, and log PASS/FAIL comparing the state |
| `-dumpmusic=D_E1M1[,seconds]` | render a song to `Saved/music_<lump>.wav` and log notes / RMS / peak |

### Controls

WASD / arrows move, mouse looks, LMB / Ctrl fires, E / Space / RMB uses, 1-7 / mouse wheel
switches weapons, F6 / F9 quick save / load, Tab opens the automap, Shift toggles run (always-run is on by default), and
Esc opens Doom's menu (F1 help, F2 save, F3 load, F4 sound volume; Y / N answer prompts). Options also switch the renderer, field of view and warp to any map. Gamepad works too.

Console (`~`): `DoomMap E1M3`, `DoomGod`, `DoomGiveAll`.

## Source layout

```
Source/DoomUE/            runtime module
  DoomWad          WAD directory, palettes, colormap, map list
  DoomMap          map lumps, BSP, subsector polygons, neighbour queries, 2D line traces
  DoomGraphics     patches, composite textures, flats, sprites, animations, switches -> UTexture2D
  DoomInfo         thing / projectile / weapon tables (sizes, frame letters, behaviour)
  DoomLevel        geometry chunks, materials, sector light texture, blocking collision
  DoomSpriteComponent  8-rotation billboard sprites with animation
  DoomThing        decorations, pickups, barrels, effects
  DoomMonster      monster AI
  DoomProjectile   missiles, BFG spray
  DoomPlayer       movement, inventory, weapons state machine, damage, pickups
  DoomWorld        UTickableWorldSubsystem: tic loop, movers, specials, lights, flow
  DoomSound        DMX -> procedural sound waves
  DoomHUD          status bar, weapon, automap, menu, intermission, title
  DoomPlayerController / DoomGameMode / DoomSettings / DoomAssets
Source/DoomUEEditor/      editor module: material + MPC generation, -run=DoomContent
```

## Status

Verified on this machine (UE 5.8.1, Windows 11): both targets compile cleanly, the materials
generate and compile, and the automated captures above render E1M1 (Freedoom 1), MAP01
(Freedoom 2) and the title screen in classic mode, and E1M1 in modern mode once shaders finish.
E1M1's BSP gives 682 subsector polygons with a total area of 6,712,686 square units, matching
the Pascal port's reference (6,712,683) to rounding. Music: Freedoom's D_E1M1 (12,440 events) and D_RUNNIN render
without clipping at about 16x real time, and start on the title screen and on map load
(listen to `Saved/music_*.wav` from `-dumpmusic`; the FM model approximates an OPL3, it is
not a cycle-accurate emulation). Save games: `-savetest` on E1M1 saves with a door in
motion, 2 corpses and 3 hunting monsters, scrambles the world and loads it back to an
identical state fingerprint (PASS). Missiles in flight are not saved yet.

WAD formats, with the castle-doom test data (`Tests/wads`, regenerated by its
`tools/make_znodes.py` and this repository's `Tools/make_gl_wads.py`): E1M1 rebuilt with
XNOD, ZNOD, XGLN, ZGL2, XGL3, ZGL3 and glBSP v2, v3, v5 nodes all give the vanilla
682 subsectors and area 6,712,686 (glBSP v1 stores 16-bit vertices: 6,709,159);
`test.deh`, `boom.wad` and `umapinfo.wad` apply as expected (`-patchtest`), and killing
E1M1's imps fires the UMAPINFO boss action.

Endings (`-endtest`): E1M8 shows Freedoom's episode text, then `CREDIT`; E3M8 goes to the
bunny; Freedoom 2's MAP30 starts the cast call. With `-brainawake`, MAP30's spawner hatched
hell knights, arachnotrons, a cacodemon and barons at the spawn spots.

Implemented but **not yet play-tested by hand**: everything interactive. That includes doors,
lifts, floors, ceilings, crushers, stairs, donuts, switches, light specials, teleporters, exits,
secrets, damaging floors, pickups, all nine weapons, monster AI and attacks, the automap, the
menu and the intermission. Expect tuning issues: movement feel, monster step and dropoff edge
cases, knockback strength, and HUD offsets.

Not done yet (roughly in priority order):

1. Palette-exact `COLORMAP` lookups (classic mode approximates each colormap row as a darkening in gamma space), spectre fuzz copying the background, and Nightmare respawning.
2. DeHackEd frame / code pointer edits and classic `Text` replacements, UDMF maps.
3. Typed save names (slots are described automatically).
4. Saving missiles in flight; Niagara particle effects, and baking the static chunk to a runtime `UStaticMesh` so software Lumen gets distance fields.
