// The linedef special table (vanilla Doom numbers) and the sector movers' kinds, shared by the
// world's files: line activation (DoomWorldSpecials.cpp), movers (DoomWorldSectors.cpp) and the
// map setup (DoomWorld.cpp). A named namespace: in a unity build an anonymous one would put these
// names into every other file of the same group.
#pragma once

#include "CoreMinimal.h"

namespace DoomWorldDetail
{
	enum class EAct : uint8
	{
		DoorRaise, DoorOpen, DoorClose, DoorClose30, DoorBlazeRaise, DoorBlazeOpen, DoorBlazeClose,
		Lift, LiftBlaze, PlatPerpetual, PlatStop, PlatRaise24Change, PlatRaise32Change, PlatRaiseNextChange,
		FloorRaiseLowestCeil, FloorRaiseCrush, FloorRaiseNext, FloorRaiseNextTurbo, FloorLowerHighest, FloorLowerHighestTurbo,
		FloorLowerLowest, FloorLowerLowestChange, FloorRaise24, FloorRaise24Change, FloorRaise512, FloorRaiseShortest,
		CeilRaiseHighest, CeilLowerFloor, CeilLower8, Crusher, CrusherFast, CrusherSilent, CrusherStop,
		Stairs8, Stairs16, LightMaxNeighbour, LightMinNeighbour, Light255, Light35, LightStrobe,
		Teleport, TeleportMonsterOnly, Exit, SecretExit, Donut
	};

	struct FSpec
	{
		int16 Num;
		TCHAR Trig;      // D manual door, W walk, S switch, G gun
		bool bRepeat;
		EAct Act;
		int8 Lock = 0;   // 1 blue 2 yellow 3 red
		bool bMonster = false;
	};

	inline const FSpec GSpecs[] =
	{
		// manual doors
		{ 1, 'D', true, EAct::DoorRaise, 0, true }, { 26, 'D', true, EAct::DoorRaise, 1 }, { 27, 'D', true, EAct::DoorRaise, 2 }, { 28, 'D', true, EAct::DoorRaise, 3 },
		{ 31, 'D', false, EAct::DoorOpen }, { 32, 'D', false, EAct::DoorOpen, 1 }, { 33, 'D', false, EAct::DoorOpen, 3 }, { 34, 'D', false, EAct::DoorOpen, 2 },
		{ 117, 'D', true, EAct::DoorBlazeRaise }, { 118, 'D', false, EAct::DoorBlazeOpen },
		// tagged doors
		{ 2, 'W', false, EAct::DoorOpen }, { 3, 'W', false, EAct::DoorClose }, { 4, 'W', false, EAct::DoorRaise, 0, true }, { 16, 'W', false, EAct::DoorClose30 },
		{ 29, 'S', false, EAct::DoorRaise }, { 42, 'S', true, EAct::DoorClose }, { 46, 'G', true, EAct::DoorOpen }, { 50, 'S', false, EAct::DoorClose },
		{ 61, 'S', true, EAct::DoorOpen }, { 63, 'S', true, EAct::DoorRaise }, { 75, 'W', true, EAct::DoorClose }, { 76, 'W', true, EAct::DoorClose30 },
		{ 86, 'W', true, EAct::DoorOpen }, { 90, 'W', true, EAct::DoorRaise }, { 103, 'S', false, EAct::DoorOpen },
		{ 105, 'W', true, EAct::DoorBlazeRaise }, { 106, 'W', true, EAct::DoorBlazeOpen }, { 107, 'W', true, EAct::DoorBlazeClose },
		{ 108, 'W', false, EAct::DoorBlazeRaise }, { 109, 'W', false, EAct::DoorBlazeOpen }, { 110, 'W', false, EAct::DoorBlazeClose },
		{ 111, 'S', false, EAct::DoorBlazeRaise }, { 112, 'S', false, EAct::DoorBlazeOpen }, { 113, 'S', false, EAct::DoorBlazeClose },
		{ 114, 'S', true, EAct::DoorBlazeRaise }, { 115, 'S', true, EAct::DoorBlazeOpen }, { 116, 'S', true, EAct::DoorBlazeClose },
		{ 99, 'S', true, EAct::DoorBlazeOpen, 1 }, { 133, 'S', false, EAct::DoorBlazeOpen, 1 }, { 134, 'S', true, EAct::DoorBlazeOpen, 3 },
		{ 135, 'S', false, EAct::DoorBlazeOpen, 3 }, { 136, 'S', true, EAct::DoorBlazeOpen, 2 }, { 137, 'S', false, EAct::DoorBlazeOpen, 2 },
		// lifts and plats
		{ 10, 'W', false, EAct::Lift, 0, true }, { 21, 'S', false, EAct::Lift }, { 62, 'S', true, EAct::Lift }, { 88, 'W', true, EAct::Lift, 0, true },
		{ 120, 'W', true, EAct::LiftBlaze }, { 121, 'W', false, EAct::LiftBlaze }, { 122, 'S', false, EAct::LiftBlaze }, { 123, 'S', true, EAct::LiftBlaze },
		{ 53, 'W', false, EAct::PlatPerpetual }, { 87, 'W', true, EAct::PlatPerpetual }, { 54, 'W', false, EAct::PlatStop }, { 89, 'W', true, EAct::PlatStop },
		{ 14, 'S', false, EAct::PlatRaise32Change }, { 15, 'S', false, EAct::PlatRaise24Change }, { 66, 'S', true, EAct::PlatRaise24Change },
		{ 67, 'S', true, EAct::PlatRaise32Change }, { 20, 'S', false, EAct::PlatRaiseNextChange }, { 22, 'W', false, EAct::PlatRaiseNextChange },
		{ 47, 'G', false, EAct::PlatRaiseNextChange }, { 68, 'S', true, EAct::PlatRaiseNextChange }, { 95, 'W', true, EAct::PlatRaiseNextChange },
		// floors
		{ 5, 'W', false, EAct::FloorRaiseLowestCeil }, { 91, 'W', true, EAct::FloorRaiseLowestCeil }, { 101, 'S', false, EAct::FloorRaiseLowestCeil },
		{ 64, 'S', true, EAct::FloorRaiseLowestCeil }, { 24, 'G', false, EAct::FloorRaiseLowestCeil },
		{ 55, 'S', false, EAct::FloorRaiseCrush }, { 56, 'W', false, EAct::FloorRaiseCrush }, { 65, 'S', true, EAct::FloorRaiseCrush }, { 94, 'W', true, EAct::FloorRaiseCrush },
		{ 18, 'S', false, EAct::FloorRaiseNext }, { 69, 'S', true, EAct::FloorRaiseNext }, { 119, 'W', false, EAct::FloorRaiseNext }, { 128, 'W', true, EAct::FloorRaiseNext },
		{ 129, 'W', true, EAct::FloorRaiseNextTurbo }, { 130, 'W', false, EAct::FloorRaiseNextTurbo }, { 131, 'S', false, EAct::FloorRaiseNextTurbo }, { 132, 'S', true, EAct::FloorRaiseNextTurbo },
		{ 19, 'W', false, EAct::FloorLowerHighest }, { 45, 'S', true, EAct::FloorLowerHighest }, { 83, 'W', true, EAct::FloorLowerHighest }, { 102, 'S', false, EAct::FloorLowerHighest },
		{ 36, 'W', false, EAct::FloorLowerHighestTurbo }, { 70, 'S', true, EAct::FloorLowerHighestTurbo }, { 71, 'S', false, EAct::FloorLowerHighestTurbo }, { 98, 'W', true, EAct::FloorLowerHighestTurbo },
		{ 38, 'W', false, EAct::FloorLowerLowest }, { 23, 'S', false, EAct::FloorLowerLowest }, { 60, 'S', true, EAct::FloorLowerLowest }, { 82, 'W', true, EAct::FloorLowerLowest },
		{ 37, 'W', false, EAct::FloorLowerLowestChange }, { 84, 'W', true, EAct::FloorLowerLowestChange },
		{ 58, 'W', false, EAct::FloorRaise24 }, { 92, 'W', true, EAct::FloorRaise24 }, { 59, 'W', false, EAct::FloorRaise24Change }, { 93, 'W', true, EAct::FloorRaise24Change },
		{ 140, 'S', false, EAct::FloorRaise512 }, { 30, 'W', false, EAct::FloorRaiseShortest }, { 96, 'W', true, EAct::FloorRaiseShortest },
		{ 9, 'S', false, EAct::Donut },
		// ceilings and crushers
		{ 40, 'W', false, EAct::CeilRaiseHighest }, { 41, 'S', false, EAct::CeilLowerFloor }, { 43, 'S', true, EAct::CeilLowerFloor },
		{ 44, 'W', false, EAct::CeilLower8 }, { 72, 'W', true, EAct::CeilLower8 },
		{ 25, 'W', false, EAct::Crusher }, { 73, 'W', true, EAct::Crusher }, { 49, 'S', false, EAct::Crusher },
		{ 6, 'W', false, EAct::CrusherFast }, { 77, 'W', true, EAct::CrusherFast }, { 141, 'W', false, EAct::CrusherSilent },
		{ 57, 'W', false, EAct::CrusherStop }, { 74, 'W', true, EAct::CrusherStop },
		// stairs
		{ 8, 'W', false, EAct::Stairs8 }, { 7, 'S', false, EAct::Stairs8 }, { 100, 'W', false, EAct::Stairs16 }, { 127, 'S', false, EAct::Stairs16 },
		// lights
		{ 12, 'W', false, EAct::LightMaxNeighbour }, { 80, 'W', true, EAct::LightMaxNeighbour }, { 104, 'W', false, EAct::LightMinNeighbour },
		{ 13, 'W', false, EAct::Light255 }, { 81, 'W', true, EAct::Light255 }, { 138, 'S', true, EAct::Light255 },
		{ 35, 'W', false, EAct::Light35 }, { 79, 'W', true, EAct::Light35 }, { 139, 'S', true, EAct::Light35 }, { 17, 'W', false, EAct::LightStrobe },
		// teleports and exits
		{ 39, 'W', false, EAct::Teleport, 0, true }, { 97, 'W', true, EAct::Teleport, 0, true },
		{ 125, 'W', false, EAct::TeleportMonsterOnly, 0, true }, { 126, 'W', true, EAct::TeleportMonsterOnly, 0, true },
		{ 11, 'S', false, EAct::Exit }, { 52, 'W', false, EAct::Exit }, { 51, 'S', false, EAct::SecretExit }, { 124, 'W', false, EAct::SecretExit },
	};

	inline const FSpec* FindSpec(int32 Num)
	{
		static TMap<int32, const FSpec*> Index;
		if (Index.Num() == 0)
		{
			for (const FSpec& S : GSpecs) Index.Add(S.Num, &S);
		}
		const FSpec* const* F = Index.Find(Num);
		return F ? *F : nullptr;
	}

	constexpr float DoorSpeed = 2.f;
	constexpr int32 DoorWait = 150;
	constexpr int32 PlatWait = 105;

	enum EDoorKind { DK_Raise, DK_Open, DK_Close, DK_Close30, DK_RaiseIn5Min, DK_CloseIn30 };
	enum EPlatKind { PK_Lift, PK_Perpetual, PK_RaiseChange, PK_RaiseNextChange };
	enum EFloorKind { FK_RaiseLowestCeil, FK_RaiseCrush, FK_RaiseNext, FK_LowerHighest, FK_LowerHighestPlus8, FK_LowerLowest, FK_LowerLowestChange, FK_Raise24, FK_Raise24Change, FK_Raise512, FK_RaiseShortest };
	enum ECeilKind { CK_RaiseHighest, CK_LowerFloor, CK_Lower8, CK_Crusher, CK_CrusherSilent };
}
