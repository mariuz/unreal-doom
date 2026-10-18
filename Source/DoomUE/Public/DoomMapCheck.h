// The map checker: the mistakes that break Doom levels most often, found in a loaded map (with the
// level's edits applied), in the spirit of Doom Builder's map analysis. Run from the DoomMap
// actor's "Check Map" (the Message Log lists the problems; clicking one selects the actor or goes
// there) or headless with -run=DoomContent -checkmap=E1M1.
#pragma once

#include "CoreMinimal.h"

class FDoomMap;
class FDoomGraphics;

struct FDoomMapIssue
{
	enum class ESeverity : uint8 { Error, Warning, Info };
	ESeverity Severity = ESeverity::Warning;
	FString Text;
	/** Where to look (Doom map units) and at what height. */
	FVector2f Where = FVector2f::ZeroVector;
	float Z = 0;
	/** What it is about (INDEX_NONE when not that kind). */
	int32 Line = INDEX_NONE;
	int32 Sector = INDEX_NONE;
	int32 Thing = INDEX_NONE;
};

namespace DoomMapCheck
{
	/** Every problem found; Gfx (optional) checks texture names and switch textures. */
	DOOMUE_API TArray<FDoomMapIssue> Run(const FDoomMap& Map, const FDoomGraphics* Gfx, const FString& MapName);
	/** "3 errors, 1 warning" and each issue, to LogDoom (commandlets, tests). */
	DOOMUE_API void Log(const FString& MapName, const TArray<FDoomMapIssue>& Issues);
	DOOMUE_API int32 Count(const TArray<FDoomMapIssue>& Issues, FDoomMapIssue::ESeverity Severity);
}
