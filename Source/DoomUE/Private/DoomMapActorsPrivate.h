// What the editor actors' files share (DoomMapActors.cpp, DoomMapMarkers.cpp, DoomMapEdits.cpp): the
// player start types and where a marker's feet go. A named namespace keeps them out of other files in
// a unity build.
#pragma once

#include "CoreMinimal.h"
#include "DoomMap.h"
#include "DoomUE.h"

namespace DoomMapActorsDetail
{
	constexpr int32 PlayerStartTypes[] = { 1, 2, 3, 4, 11 };
	inline const TCHAR* const PlayerStartNames[] = { TEXT("Player1Start"), TEXT("Player2Start"), TEXT("Player3Start"), TEXT("Player4Start"), TEXT("DeathmatchStart") };

	inline float MarkerFloorAt(const FDoomMap* Map, const FVector& Location)
	{
		const int32 Sec = Map ? Map->SectorAt(Doom::ToDoom2D(Location)) : INDEX_NONE;
		return Sec >= 0 ? Map->Sectors[Sec].Floor : Location.Z / Doom::Scale;
	}
}
