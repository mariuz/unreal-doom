// What the monster's files share (DoomMonster.cpp, DoomMonsterCombat.cpp, DoomMonsterSmart.cpp):
// Doom's eight walking directions and its random numbers. A named namespace keeps them out of
// other files in a unity build.
#pragma once

#include "CoreMinimal.h"
#include "DoomInfo.h"

namespace DoomMonsterDetail
{
	constexpr int32 DirNone = 8;
	inline const int32 Opposite[9] = { 4, 5, 6, 7, 0, 1, 2, 3, DirNone };

	inline FVector2f DirVector(int32 Dir)
	{
		const float A = FMath::DegreesToRadians(Dir * 45.f);
		return FVector2f(FMath::Cos(A), FMath::Sin(A));
	}

	inline int32 MonsterRand() { return DoomInfo::Random(); }

	inline bool Is(const FDoomThingDef* D, const TCHAR* Name) { return D && D->Name.Equals(Name, ESearchCase::CaseSensitive); }
}
