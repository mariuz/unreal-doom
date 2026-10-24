// What the marine's files share (DoomPlayer.cpp, DoomPlayerWeapons.cpp): random numbers and the
// weapon positions and order. A named namespace keeps them out of other files in a unity build.
#pragma once

#include "CoreMinimal.h"
#include "DoomInfo.h"

namespace DoomPlayerDetail
{
	inline int32 PlayerRand() { return DoomInfo::Random(); }
	constexpr float WeaponTop = 32.f;
	constexpr float WeaponBottom = 128.f;
	constexpr float RaiseSpeed = 6.f;

	inline const EDoomWeapon CycleOrder[] = {
		EDoomWeapon::Fist, EDoomWeapon::Chainsaw, EDoomWeapon::Pistol, EDoomWeapon::Shotgun, EDoomWeapon::SuperShotgun,
		EDoomWeapon::Chaingun, EDoomWeapon::Rocket, EDoomWeapon::Plasma, EDoomWeapon::BFG };
}
