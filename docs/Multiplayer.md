# Multiplayer

Co-op and deathmatch for up to 4 players. The server runs the game and the clients show it;
this is Unreal's standard server-authoritative replication, not Doom's lockstep networking.
See [Architecture](Architecture.md) for the game mode, game state and player state split it
builds on.

## Playing

Every machine needs the same IWAD (and PWADs) in `Content/WADs`.

Start a listen server (it plays too) on E1M1:

```bash
"C:/Program Files/Epic Games/UE_5.8/Engine/Binaries/Win64/UnrealEditor.exe" "E:/Work/SessionFolderUnrealClaude/DoomUE.uproject" "/Engine/Maps/Entry?listen" -game -warp=E1M1
```

Add `-deathmatch` for deathmatch rules, or `-altdeath` for deathmatch 2.0 (items come back 30 s after
they are taken, and weapons don't stay). `-nomonsters` leaves the monsters out and `-fast` makes them
Nightmare-fast on any skill. Join from another machine, or from a second window on the same one:

```bash
"C:/Program Files/Epic Games/UE_5.8/Engine/Binaries/Win64/UnrealEditor.exe" "E:/Work/SessionFolderUnrealClaude/DoomUE.uproject" 127.0.0.1 -game
```

You can also use the console: `open 127.0.0.1`. The game uses UDP port 7777.

**From the menu:** MULTIPLAYER in the main menu.

* **Hosting:** choose the game (co-op, deathmatch, deathmatch 2.0), the map (or a random one),
  monsters on or off and fast monsters, then pick **HOST GAME**. It opens a listen server with those
  options (`listen?DoomMap=E1M1?DoomRules=1?NoMonsters?Fast`), which the game mode reads.
* **Joining:** type an address in **JOIN ADDRESS** (127.0.0.1 by default) and pick **CONNECT**.

## Rules

| | Co-op | Deathmatch (`-deathmatch`) |
|---|---|---|
| Starts | Doom's player 1-4 starts by player number. A map without them lines players up beside start 1 | A random deathmatch start (type 11), else the co-op starts |
| Things | Single-player things plus those flagged *multiplayer only* (flag 16) | The same without keys (Doom's `MF_NOTDMATCH`) |
| Weapons | Picked up and gone | **Weapons stay**: they remain for the next player, and you cannot take one you carry. With `-altdeath`, weapons are taken like other items |
| Items | Picked up and gone | Gone, or with `-altdeath` back where they were 30 s later, with the item fog (except invulnerability and invisibility) |
| Dying | Press Use to be reborn at your start with a pistol. The level carries on | The same, at a deathmatch start |
| Frags | Counted as in Doom's `P_KillMobj`: another player's kill +1 for them; your own rocket, or a death with no source (slime, crushers), −1; monsters change nothing | Shown on the status bar in place of ARMS |
| Intermission | A row per player: kills, items and secrets as a share of the level's totals, and frags | Frags per player |
| Menu | This player's overlay; the game never pauses | Same |
| Saving | Not in network games | Same |

Each player's face on the status bar and their intermission row sit on their colour square
(`STFB0-3`, `STPB0-3`). The marines wear Doom's colours, as its translation tables recolour the green
ramp: player 1 green, 2 grey, 3 brown, 4 red (`FDoomGraphics::SpriteFrame`'s translation, replicated
with the sprite). Any player can press Enter to leave the intermission.

## What travels over the network

* **Flow** (`ADoomGameState::Flow`): phase, map, level tic, totals, intermission tic, pending
  map, deathmatch flag, and a level serial. A new serial makes clients build the map from
  their own WAD (`UDoomWorld::OnFlowReplicated`). Clients build geometry only: no thinkers, no
  things.
* **Sectors and sidedefs** (`FDoomSectorNetArray`, `FDoomSideNetArray`, fast arrays): after
  each tic the server publishes only what differs from the level as loaded: floor and ceiling
  heights, light, flats, special, and the textures of switched lines. Doors, lifts, crushers,
  stairs, flickering lights and switches all reach clients this way.
* **Monsters, things, missiles**: replicated actors. Each sends its type (`NetType`), and the
  client runs the same `Init*` to set up its capsule, sprite and light. Monsters move through
  `CharacterMovement` replication, missiles through replicated movement (the client doesn't
  simulate them), and things through replicated movement because they ride lifts. Corpses
  (`bNetCorpse`) and burst barrels (`bNetDead`) switch their collision on clients too.
* **Sprites** (`UDoomSpriteComponent::Net`): prefix, sequence, frame time, brightness, fuzz and
  facing. Clients restart a sequence when its serial changes and animate it on a local 35 Hz
  clock.
* **Players**:
  * The inventory (`ADoomPlayerState::Inv`) uses a custom `NetSerialize` that packs weapons and
    keys into bit masks.
  * Credit and frags replicate with the player state.
  * The owner alone receives `FDoomPlayerView`: weapon and flash sprites, bob, powerup and
    palette counters, face state and death view height.
  * Other players see the marine's `Body` sprite (Doom's `PLAY` frames: run, attack, pain,
    death, gibs).
  * Fire, use and weapon changes are server RPCs. Movement and view use `CharacterMovement`
    prediction.
* **Finale screens** (`ADoomGameState::Finale`): the story text, backdrop, picture and the cast call,
  published by the server each finale tic; any player's Enter moves them on (`ServerFinaleInput`).
* **Sounds, effects, particles**:
  * Unreliable multicasts on the game state. Puffs, blood and teleport fog are spawned locally
    on each client.
  * A player's own sounds (weapons, pickups, pain) play in 2D for that player and positionally
    for everyone else.
  * Pickup, lock and secret messages go only to the player concerned.

Level geometry never serves as a character's replicated movement base. The world's own lift
code carries bodies, and the level's runtime-built mesh components cannot be referenced across
the network.

## Testing it

`-nettest` runs a scripted two-process test:

```bash
"C:/Program Files/Epic Games/UE_5.8/Engine/Binaries/Win64/UnrealEditor.exe" "E:/Work/SessionFolderUnrealClaude/DoomUE.uproject" "/Engine/Maps/Entry?listen" -game -warp=E1M1 -nettest
```

```bash
"C:/Program Files/Epic Games/UE_5.8/Engine/Binaries/Win64/UnrealEditor.exe" "E:/Work/SessionFolderUnrealClaude/DoomUE.uproject" 127.0.0.1 -game -nettest
```

**The server**, once the client has joined:
1. opens the first manual door,
2. kills two monsters as the host (in deathmatch, the client too),
3. exits to the intermission after 14 s.

**The client:**
1. fires through the server RPC,
2. presses Use to respawn if it is dead,
3. takes `net_client_play.png` facing the other player and `net_client_inter.png` on the
   intermission.

Both log `NetFingerprint()` every second (map, level serial, phase, monsters alive and dead,
things, the door's ceiling, totals, and each player's health, ammo, kills, frags and colour as that
machine draws it, plus the finale's stage), so the two logs can be compared line by line.

`-hosttest` (on the first) and `-jointest` (on the second) do the same through the menu's
Multiplayer page: a fast deathmatch on E1M1 hosted from the page, joined from it.

## Not done yet

* The host page has no skill choice (the host's own skill setting is used) and no port.
* Joining mid-game spawns the new player at their start. Seamless travel between maps isn't
  used (maps change inside one Unreal level, so it isn't needed today).
