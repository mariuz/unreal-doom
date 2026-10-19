# SMART monsters: Unreal's AI framework

**Options > Monster AI** switches the monsters between CLASSIC, Doom's own `A_Chase` (the default),
and SMART. In SMART mode, Unreal's AI framework decides where monsters go. Doom's code still decides
when they attack, what the attacks do, their sounds and every frame they show. The setting is saved
(`UDoomSettings::bSmartMonsters`). `-smartai` and `-classicai` override it for one run.

The design follows the Lyra and ShooterGame pattern: an AI controller with perception, a Behavior
Tree reading a Blackboard, and EQS for picking positions. The navigation is Doom's own, not a nav
mesh (see "Paths" below).

## The pieces (`DoomAI.h`)

* **`ADoomAIController`** possesses each monster in SMART mode, replacing the plain `AAIController`
  that CLASSIC monsters keep. Its **AI Perception** has:
  * **Sight:** 2048 units, the half in front, as Doom's monsters see.
  * **Hearing:** the player's gunfire, reported where Doom floods its noise.

  Sight wakes a monster. Doom's own noise flood still wakes them by sound. An awake monster that
  hears the fight remembers where it came from.
* **Blackboard** (`BB_DoomMonster`): `Target`, `LastKnown`, `CanSee`, `MoveGoal`.
* **Behavior Tree** (`BT_DoomMonster`): a selector under the *Track Target* service, which keeps
  the target, Doom's sight check and the last known position current. Its branches, in priority
  order:
  1. **Take Cover:** hurt below a third of its health, not a melee monster, the target in sight, at
     most every 8 s. Find a spot hidden from the target (EQS), go there and wait a moment.
  2. **Charge:** melee-only monsters (demons, spectres, lost souls) go for the target by the
     shortest way, seen or not.
  3. **Fight:** the target in sight. Find a firing position (EQS) and go there.
  4. **Hunt:** out of sight. Walk round to the target. Doom's monsters always head for their target;
     these find the way.
  5. **Investigate:** no target, but something was seen or heard. Go and look.
  6. Otherwise wait.
* **Custom nodes:**
  * `UBTService_DoomTrackTarget`;
  * `UBTDecorator_DoomCondition` (can see, has a target, knows where, melee only, hurt);
  * `UBTTask_DoomFindSpot`, which runs an EQS query into a blackboard key;
  * `UBTTask_DoomMoveTo`, which walks a path, replans twice a second for a moving target, and gives
    up when stuck so the tree thinks again.
* **EQS queries** (`EQ_DoomAttackSpot`, `EQ_DoomCover`):
  * the *Doom: Spots Around* generator: rings of points around the monster where it fits, on the
    floor of their sector;
  * *Doom: Sees Target*: a filter, keep in sight for attack spots, out of sight for cover;
  * *Doom: Preferred Range* (scored): hitscanners about 320 units, imps 384, cacodemons 448, barons
    512;
  * *Doom: Away From Crowd* (scored): spreads a group out, and avoids where the others are heading;
  * *Doom: Reachable*: filters on a path existing and scores shorter walks higher.
* **Holding fire:** SMART monsters don't fire missiles when another monster stands in the line of
  fire, so they start fewer fights among themselves. Attack timing still uses Doom's random rolls,
  so the attack rate is vanilla's.

The content builder saves the blackboard, the tree and the queries in `/Game/Doom/AI`
(`-run=DoomContent -ai [-force]`, also part of the default build). They open in the Behavior Tree
and EQS editors, where the graph is laid out from the saved nodes. The game uses the saved assets,
and builds the same set in code when they are missing.

## Paths (`FDoomNav`)

A nav mesh doesn't suit Doom:

* the level is procedural geometry generated at load;
* doors and lifts move every tic;
* Doom's movement rules aren't a nav mesh's: steps of 24 or less, no stepping off higher ledges,
  closed doors that monsters open by walking into them.

So paths run over the map's **subsectors**. They are convex, so a straight line inside one is
clear. Subsectors join where their polygons share an edge (E1M1: 682 subsectors, 1,448 joins, built
in about 1.5 ms when first needed).

* **Crossing between sectors** follows Doom's rules: not through blocking or monster-blocking
  lines, a step of 24 or less up or down (flyers ignore steps), enough headroom, and through DR
  doors (special 1) even when shut. Monsters can't open keyed doors or use switches.
* **Pathfinding** is A\* over the joins, crossing each one at the point nearest the way so far, kept
  a radius from its ends.
* **Walking:** a monster walks the path in straight lines (not Doom's eight directions). It turns
  to face the way in 45° steps, as `A_Chase` does, and opens a door it bumps into.

Doom's maps confine many monsters by design (ledges, lifts, switch doors, teleporter-only areas),
and SMART monsters are confined the same way. On E1M1, 5 of the 53 monsters can walk to the start.

## Testing

* **The play-test's `smartai` step** (in `-playtest`) switches to SMART, wakes every monster on
  the player, and runs 10 s. It checks that:
  * every live monster has the AI controller with its tree running;
  * perception senses the player;
  * 70% of the monsters that can walk to the player close in by 64+ units or get it in sight.

  `-smartbaseline` measures the same with CLASSIC monsters. Monsters that closed in or saw the
  player, of those that can walk to it:

  | Map | SMART | CLASSIC |
  |---|---|---|
  | RND00005 | 15 of 15 | 11 of 16 |
  | RND00001 | 12 of 12 | 3 of 11 |
  | RND00004 | 15 of 15 | 13 of 15 |

* **`-run=DoomContent -navtest=all`** (or `E1M1,RND00003`) builds the paths for each map. It counts
  the subsectors joined to and walkable from the start, and how many monsters can walk there.
