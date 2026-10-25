// The random map generator: a playable Doom map from a seed. Rooms on a grid of cells joined by a
// random tree of corridors (with a few loops), stairs where floors differ, doors, up to three keyed
// doors on the way to the exit (each key reachable before its door), an exit switch, monsters that
// get tougher further from the start, weapons, ammo and health. The layout is painted on a grid of
// 32-unit tiles, and the lines are the edges between tiles of different sectors, so every sector is
// closed; the node builder (FDoomMap::Rebuild) makes the BSP.
//
// A generated map is named RND plus five hex digits of its seed (RND0002A): -warp=RND0002A plays
// it, saves and network clients regenerate it from the name, and its exit leads to the next seed.
#pragma once

#include "CoreMinimal.h"

class FDoomMap;

namespace DoomMapGen
{
	struct FOptions
	{
		uint32 Seed = 1;
		/** Rooms in the map (4-24). */
		int32 Rooms = 10;
		/** Doom 2 monsters, weapons and textures (an IWAD with MAP01). */
		bool bDoom2 = false;
		/** Monsters per room scale. */
		float MonsterDensity = 1.f;
	};

	/** What came out: counts for the log and the tests. */
	struct FReport
	{
		int32 Attempts = 0;
		int32 Rooms = 0, Links = 0, Doors = 0, Keys = 0, Monsters = 0, Items = 0;
		DOOMWADTOOLS_API FString ToString() const;
	};

	/** The options a map name stands for: the seed varies the size (8-12 rooms) unless -genrooms=N
	 *  says otherwise. The game, the editor and the commandlets all use this, so a name always makes
	 *  the same map. */
	DOOMWADTOOLS_API FOptions DefaultOptions(uint32 Seed, bool bDoom2);

	/** RND plus the seed's low 20 bits in hex. */
	DOOMWADTOOLS_API FString NameForSeed(uint32 Seed);
	/** Whether a map name is a generated one, and its seed. */
	DOOMWADTOOLS_API bool ParseName(const FString& MapName, uint32& OutSeed);

	/** Whether the WAD has a wall texture (bFlat false) or a flat of that name. */
	using FHasPicture = TFunction<bool(FName Name, bool bFlat)>;

	/** Fills Out (cleared first) with a new map and builds its nodes. HasPicture (optional) limits
	 *  the textures and flats to the WAD's own. False only if no attempt made a valid map. */
	DOOMWADTOOLS_API bool Generate(const FOptions& Options, const FHasPicture& HasPicture, FDoomMap& Out, FReport* OutReport = nullptr);
}
