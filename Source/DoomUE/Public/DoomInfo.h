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

/** Game rules DeHackEd's Misc and Ammo blocks can change. */
struct FDoomRules
{
	int32 InitialHealth = 100;
	int32 InitialBullets = 50;
	int32 MaxHealth = 200;        // health bonus cap
	int32 MaxArmor = 200;         // armor bonus cap
	int32 GreenArmorClass = 1;
	int32 BlueArmorClass = 2;
	int32 MaxSoulsphere = 200;
	int32 SoulsphereHealth = 100;
	int32 MegasphereHealth = 200;
	int32 GodModeHealth = 100;
	int32 IdfaArmor = 200;
	int32 IdkfaArmor = 200;
	bool bMonstersInfight = false;
	int32 MaxAmmo[4] = { 200, 50, 50, 300 };
	int32 ClipAmmo[4] = { 10, 4, 1, 20 };  // small pickup; boxes are 5x, weapons 2x
};

namespace DoomInfo
{
	DOOMUE_API const FDoomThingDef* FindThing(int32 DoomEdNum);
	DOOMUE_API const FDoomThingDef* FindThingByName(const FString& Name);
	/** Every thing type the port knows (editor type lists). */
	DOOMUE_API TConstArrayView<FDoomThingDef> AllThings();
	DOOMUE_API const FDoomProjectileDef* FindProjectile(const FString& Name);
	DOOMUE_API const FDoomWeaponDef& Weapon(EDoomWeapon W);
	DOOMUE_API int32 MaxAmmo(EDoomAmmo A, bool bBackpack);
	DOOMUE_API FString PickupMessage(EPickup P);

	// patching (DeHackEd)
	DOOMUE_API void ResetTables();
	DOOMUE_API FDoomThingDef* MutableThing(int32 DoomEdNum);
	DOOMUE_API FDoomProjectileDef* MutableProjectile(const FString& Name);
	DOOMUE_API FDoomWeaponDef& MutableWeapon(EDoomWeapon W);
	DOOMUE_API FDoomRules& Rules();

	/** BEX / DEHACKED strings by mnemonic (GOTARMOR, HUSTR_E1M1...), else Default. */
	DOOMUE_API FString Text(const TCHAR* Mnemonic, const FString& Default);
	DOOMUE_API bool HasText(const TCHAR* Mnemonic);
	DOOMUE_API void SetString(const FString& Mnemonic, const FString& Text);
	/** The mnemonic whose current text equals Text (DeHackEd Text blocks), or empty. */
	DOOMUE_API FString FindStringKey(const FString& Text);
	/** Sprite and sound renames (DeHackEd Text blocks, BEX [SPRITES] / [SOUNDS]). */
	DOOMUE_API void SetSpriteRename(const FString& From, const FString& To);
	DOOMUE_API FString SpriteName(const FString& Prefix);
	DOOMUE_API void SetSoundRename(const FString& From, const FString& To);
	DOOMUE_API FString SoundName(const FString& Name);
	/** Par times in seconds ([PARS], UMAPINFO partime); -1 when unknown. */
	DOOMUE_API void SetParTime(const FString& Map, int32 Seconds);
	DOOMUE_API int32 ParTime(const FString& Map);

	/** Doom's P_Random: the 256-entry table is replaced by a seeded generator. */
	DOOMUE_API int32 Random();
}
