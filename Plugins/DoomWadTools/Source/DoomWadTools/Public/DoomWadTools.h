// DoomWadTools: Doom's data without the game.
//   DoomWad.h        a WAD and its PWADs: lumps by name, later files replacing earlier ones
//   DoomMap.h        a map (vanilla, Hexen and UDMF lines; vanilla, extended and compressed nodes),
//                    and FDoomMap::Rebuild, the node builder (DoomNodeBuilder.cpp)
//   DoomMapGen.h     random maps from a seed
//   DoomWadWriter.h  a map as a PWAD
#pragma once

#include "CoreMinimal.h"

/** WADs and maps: loading, nodes, the generator, PWADs (the game's map checker logs here too). */
DOOMWADTOOLS_API DECLARE_LOG_CATEGORY_EXTERN(LogDoomMap, Log, All);
