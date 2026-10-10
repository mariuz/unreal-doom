// Thing, projectile and weapon definitions: Doom's info tables reduced to what
// the port needs (sprite, size, frame letter sequences, behaviour flags).
#pragma once

#include "CoreMinimal.h"

namespace EThingFlag
{
	enum : uint32
	{
		Solid = 1 << 0, Shootable = 1 << 1, Monster = 1 << 2, Pickup = 1 << 3,
		Hanging = 1 << 4, Float = 1 << 5, CountKill = 1 << 6, CountItem = 1 << 7,
		Shadow = 1 << 8, FullBright = 1 << 9, Barrel = 1 << 10, NoBlood = 1 << 11,
		Boss = 1 << 12, // splash-immune bosses (cyberdemon, mastermind)
		NotInDeathmatch = 1 << 13,
	};
}

enum class EDoomAttack : uint8
{
	None, Hitscan, Shotgun, Chaingun, Melee, Missile, MeleeOrMissile, Skull, Vile, PainSpawn, Revenant, Mancubus, Spider, Cyber
};

enum class EPickup : uint8
{
	None, Health, HealthBonus, Soulsphere, Megasphere, GreenArmor, BlueArmor, ArmorBonus,
	Invulnerability, Berserk, Invisibility, RadSuit, Map, LightAmp,
	BlueCard, YellowCard, RedCard, BlueSkull, YellowSkull, RedSkull,
	Clip, ClipBox, Shells, ShellBox, Rocket, RocketBox, Cell, CellPack, Backpack,
	Chainsaw, Shotgun, SuperShotgun, Chaingun, RocketLauncher, Plasma, BFG
};

enum class EDoomAmmo : uint8 { Bullets, Shells, Rockets, Cells, None };

enum class EDoomWeapon : uint8 { Fist, Pistol, Shotgun, Chaingun, Rocket, Plasma, BFG, Chainsaw, SuperShotgun, Count };

struct FDoomThingDef
{
	int32 DoomEdNum = 0;
	const TCHAR* Name = TEXT("");
	const TCHAR* Sprite = TEXT("");
	const TCHAR* Idle = TEXT("A");     // frame letters
	const TCHAR* Run = TEXT("");
	const TCHAR* Melee = TEXT("");
	const TCHAR* Attack = TEXT("");    // missile/ranged; last letter fires
	const TCHAR* Pain = TEXT("");
	const TCHAR* Death = TEXT("");
	const TCHAR* XDeath = TEXT("");
	float Radius = 20, Height = 16;
	int32 Health = 0;
	float Speed = 0;        // units per chase step
	int32 RunTics = 4;      // tics per run frame (one chase step each)
	int32 PainChance = 0;   // of 256
	uint32 Flags = 0;
	EDoomAttack Attack_ = EDoomAttack::None;
	const TCHAR* Projectile = TEXT("");
	int32 MeleeDamage = 0;  // per d8/d10 style roll, see code
	EPickup Pickup = EPickup::None;
	const TCHAR* SeeSound = TEXT("");
	const TCHAR* AttackSound = TEXT("");
	const TCHAR* PainSound = TEXT("");
	const TCHAR* DeathSound = TEXT("");
	const TCHAR* ActiveSound = TEXT("");
	int32 DropType = 0;     // DoomEdNum dropped on death
	FLinearColor LightColor = FLinearColor::Transparent; // modern renderer point light
	int32 IdleTics = 8;
	bool bFullBrightIdle = false;
};

struct FDoomProjectileDef
{
	const TCHAR* Name;
	const TCHAR* Sprite;
	const TCHAR* Fly;        // flight frame letters (full bright)
	const TCHAR* Explode;    // explosion frames
	float Speed;             // units per tic
	float Radius;
	int32 DamageMul;         // damage = (1..8) * DamageMul
	int32 SplashDamage;      // rocket: 128
	const TCHAR* FireSound;
	const TCHAR* HitSound;
	FLinearColor Light;
	bool bHoming = false;
};

struct FDoomWeaponFrame
{
	TCHAR Frame;
	int32 Tics;
	bool bFire;     // perform the attack on this frame
	int32 Flash;    // -1 none, otherwise flash frame index ('A' + n)
};

struct FDoomWeaponDef
{
	EDoomWeapon Weapon;
	const TCHAR* Sprite;
	const TCHAR* FlashSprite;
	TCHAR ReadyFrame;
	EDoomAmmo Ammo;
	int32 AmmoPerShot;
	TArray<FDoomWeaponFrame> Fire;
	const TCHAR* Sound;
	int32 Slot;     // key 1..7
};

namespace DoomInfo
{
	DOOMUE_API const FDoomThingDef* FindThing(int32 DoomEdNum);
	DOOMUE_API const FDoomProjectileDef* FindProjectile(const FString& Name);
	DOOMUE_API const FDoomWeaponDef& Weapon(EDoomWeapon W);
	DOOMUE_API int32 MaxAmmo(EDoomAmmo A, bool bBackpack);
	DOOMUE_API FString PickupMessage(EPickup P);

	/** Doom's P_Random: the 256-entry table is replaced by a seeded generator. */
	DOOMUE_API int32 Random();
}
