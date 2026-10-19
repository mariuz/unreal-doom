// Map editing operations on an FDoomMap, shared by the runtime map editor and the tests: finding
// what is under a point, drawing new sectors (which split the lines they touch and share the walls
// they run along), deleting sectors and things, and a starter map. Geometry changes leave the BSP
// stale: call FDoomMap::Rebuild() after them.
#pragma once

#include "CoreMinimal.h"
#include "DoomMap.h"

namespace DoomMapEdit
{
	/** P on the grid (Grid <= 0: unchanged). */
	DOOMUE_API FVector2f Snap(const FVector2f& P, float Grid);

	/** Nearest within Radius map units, or INDEX_NONE. */
	DOOMUE_API int32 VertexNear(const FDoomMap& Map, const FVector2f& P, float Radius);
	DOOMUE_API int32 LineNear(const FDoomMap& Map, const FVector2f& P, float Radius);
	DOOMUE_API int32 ThingNear(const FDoomMap& Map, const FVector2f& P, float Radius);
	/** The sector P is really inside (the BSP answers for points outside the map too). */
	DOOMUE_API int32 SectorInside(const FDoomMap& Map, const FVector2f& P);

	/** Splits Line at P (on it): the second half is a new line with copies of the sides. Returns
	 *  the new vertex. */
	DOOMUE_API int32 SplitLine(FDoomMap& Map, int32 Line, const FVector2f& P);

	/**
	 * A new sector from a closed polygon (3+ points, no edge crossing another). It may lie in the
	 * void or inside one sector (its walls are then two-sided to that sector), and its edges may run
	 * along existing one-sided walls, which become two-sided doorways between the two sectors. Lines
	 * its corners touch are split. Template gives the heights, flats and light (a sector drawn inside
	 * another takes that one's heights when Template's are equal); Wall the wall texture.
	 * Returns the new sector, or INDEX_NONE with OutError saying why.
	 */
	DOOMUE_API int32 DrawSector(FDoomMap& Map, TArray<FVector2f> Points, const FDoomSector& Template, FName Wall, FString& OutError);

	/** Removes a sector: its walls go, and walls it shared become one-sided walls of the other sector. */
	DOOMUE_API void DeleteSector(FDoomMap& Map, int32 Sector);

	/** Drops vertices, sides and sectors nothing uses any more (indices are renumbered). */
	DOOMUE_API void Compact(FDoomMap& Map);

	/** A room to start a new map from: 512 x 512 with a player start, built and ready to load. */
	DOOMUE_API void MakeStarterMap(FDoomMap& Map, const FString& Name, FName Wall, FName Floor, FName Ceiling);

	/** The data an edit can change (undo / redo snapshots). */
	struct FSnapshot
	{
		TArray<FVector2f> Vertices;
		TArray<FDoomLine> Lines;
		TArray<FDoomSide> Sides;
		TArray<FDoomSector> Sectors;
		TArray<FDoomMapThing> Things;
	};
	DOOMUE_API FSnapshot Take(const FDoomMap& Map);
	/** Puts a snapshot back and rebuilds the BSP. */
	DOOMUE_API void Restore(FDoomMap& Map, const FSnapshot& S);
}
