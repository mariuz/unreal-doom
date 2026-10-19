// Writing maps as PWADs: Doom's binary map format (THINGS, LINEDEFS, SIDEDEFS, VERTEXES, SEGS,
// SSECTORS, NODES, SECTORS, REJECT, BLOCKMAP), with nodes from the port's node builder, so a
// generated or edited map plays in any Doom port and opens in any Doom editor.
#pragma once

#include "CoreMinimal.h"

class FDoomMap;

namespace DoomWadWriter
{
	struct FLump
	{
		FString Name;
		TArray<uint8> Data;
	};

	/** The map's lumps, marker first, under MapName (E1M1, MAP01...). The nodes are rebuilt on a
	 *  copy with whole-unit vertices, as vanilla needs; OutNote says what had to be rounded or cut. */
	DOOMUE_API bool MapLumps(const FDoomMap& Map, const FString& MapName, TArray<FLump>& Out, FString* OutNote = nullptr);

	/** A PWAD file holding the lumps. */
	DOOMUE_API bool SaveWad(const TArray<FLump>& Lumps, const FString& Path);

	/** MapLumps then SaveWad: one map in its own PWAD. */
	DOOMUE_API bool SaveMap(const FDoomMap& Map, const FString& MapName, const FString& Path, FString* OutNote = nullptr);
}
