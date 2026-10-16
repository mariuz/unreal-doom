// Thing, projectile and weapon definitions: Doom's info tables reduced to what
// the port needs (sprite, size, frame letter sequences, behaviour flags).
//
// The defaults are built in (DoomInfo.cpp). A UDoomGameData asset (DoomGameData.h, made by
// -run=DoomContent -gamedata) holds an editable copy: when it exists the game starts from it,
// and DeHackEd patches then apply on top.
#pragma once

#include "CoreMinimal.h"
#include "DoomInfo.generated.h"

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

/** EThingFlag bits as an enum, for the editor's flag checkboxes (FDoomThingDef::Flags). */
UENUM(meta = (Bitflags, UseEnumValuesAsMaskValuesInEditor = "true"))
enum class EDoomThingFlagBits : int32
{
	None = 0 UMETA(Hidden),
	Solid = 1 << 0, Shootable = 1 << 1, Monster = 1 << 2, Pickup = 1 << 3,
	Hanging = 1 << 4, Float = 1 << 5, CountKill = 1 << 6, CountItem = 1 << 7,
	Shadow = 1 << 8, FullBright = 1 << 9, Barrel = 1 << 10, NoBlood = 1 << 11,
	Boss = 1 << 12, NotInDeathmatch = 1 << 13,
};

UENUM(BlueprintType)
enum class EDoomAttack : uint8
{
	None, Hitscan, Shotgun, Chaingun, Melee, Missile, MeleeOrMissile, Skull, Vile, PainSpawn, Revenant, Mancubus, Spider, Cyber
};

UENUM(BlueprintType)
enum class EPickup : uint8
{
	None, Health, HealthBonus, Soulsphere, Megasphere, GreenArmor, BlueArmor, ArmorBonus,
	Invulnerability, Berserk, Invisibility, RadSuit, Map, LightAmp,
	BlueCard, YellowCard, RedCard, BlueSkull, YellowSkull, RedSkull,
	Clip, ClipBox, Shells, ShellBox, Rocket, RocketBox, Cell, CellPack, Backpack,
	Chainsaw, Shotgun, SuperShotgun, Chaingun, RocketLauncher, Plasma, BFG
};

UENUM(BlueprintType)
enum class EDoomAmmo : uint8 { Bullets, Shells, Rockets, Cells, None };

UENUM(BlueprintType)
enum class EDoomWeapon : uint8 { Fist, Pistol, Shotgun, Chaingun, Rocket, Plasma, BFG, Chainsaw, SuperShotgun, Count UMETA(Hidden) };

/** A thing type: monster, pickup or decoration (Doom's mobjinfo, reduced). Frames are sprite frame letters. */
USTRUCT(BlueprintType)
struct DOOMUE_API FDoomThingDef
{
	GENERATED_BODY()

	/** The number in the map's THINGS lump. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Thing")
	int32 DoomEdNum = 0;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Thing")
	FString Name;
	/** Four-letter sprite prefix (POSS, TROO...). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Thing")
	FString Sprite;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Frames")
	FString Idle = TEXT("A");
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Frames")
	FString Run;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Frames")
	FString Melee;
	/** Missile or ranged attack; the last letter fires. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Frames")
	FString Attack;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Frames")
	FString Pain;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Frames")
	FString Death;
	/** Gibbed death (big damage). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Frames")
	FString XDeath;
	/** Tics per idle frame. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Frames")
	int32 IdleTics = 8;
	/** Tics per run frame (one chase step each). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Frames")
	int32 RunTics = 4;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Frames")
	bool bFullBrightIdle = false;

	/** Map units. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Body")
	float Radius = 20;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Body")
	float Height = 16;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Body")
	int32 Health = 0;
	/** Map units per chase step. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Body")
	float Speed = 0;
	/** Out of 256. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Body")
	int32 PainChance = 0;
	/** EThingFlag bits. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Body", meta = (Bitmask, BitmaskEnum = "/Script/DoomUE.EDoomThingFlagBits"))
	int32 Flags = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Attack", meta = (DisplayName = "Attack Kind"))
	EDoomAttack Attack_ = EDoomAttack::None;
	/** FDoomProjectileDef name for missile attacks. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Attack")
	FString Projectile;
	/** Per roll (d8 / d10 style, see the code). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Attack")
	int32 MeleeDamage = 0;
	/** DoomEdNum dropped on death. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Attack")
	int32 DropType = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Pickup")
	EPickup Pickup = EPickup::None;

	/** Sound lump names without the DS prefix. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sounds")
	FString SeeSound;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sounds")
	FString AttackSound;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sounds")
	FString PainSound;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sounds")
	FString DeathSound;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sounds")
	FString ActiveSound;

	/** Modern renderer point light (alpha 0: none). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Rendering")
	FLinearColor LightColor = FLinearColor::Transparent;
};

/** A missile (Doom's MT_TROOPSHOT, MT_ROCKET...). */
USTRUCT(BlueprintType)
struct DOOMUE_API FDoomProjectileDef
{
	GENERATED_BODY()

	FDoomProjectileDef() = default;
	FDoomProjectileDef(const TCHAR* InName, const TCHAR* InSprite, const TCHAR* InFly, const TCHAR* InExplode, float InSpeed, float InRadius,
		int32 InDamageMul, int32 InSplash, const TCHAR* InFireSound, const TCHAR* InHitSound, const FLinearColor& InLight, bool bInHoming = false)
		: Name(InName), Sprite(InSprite), Fly(InFly), Explode(InExplode), Speed(InSpeed), Radius(InRadius), DamageMul(InDamageMul),
		  SplashDamage(InSplash), FireSound(InFireSound), HitSound(InHitSound), Light(InLight), bHoming(bInHoming) {}

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Projectile")
	FString Name;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Projectile")
	FString Sprite;
	/** Flight frame letters (full bright). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Projectile")
	FString Fly;
	/** Explosion frame letters. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Projectile")
	FString Explode;
	/** Map units per tic. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Projectile")
	float Speed = 10;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Projectile")
	float Radius = 6;
	/** Damage = (1..8) * DamageMul. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Projectile")
	int32 DamageMul = 1;
	/** Rocket: 128. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Projectile")
	int32 SplashDamage = 0;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Projectile")
	FString FireSound;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Projectile")
	FString HitSound;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Projectile")
	FLinearColor Light = FLinearColor::White;
	/** Revenant tracer. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Projectile")
	bool bHoming = false;
};

/** One frame of a weapon's firing sequence. */
USTRUCT(BlueprintType)
struct DOOMUE_API FDoomWeaponFrame
{
	GENERATED_BODY()

	FDoomWeaponFrame() = default;
	FDoomWeaponFrame(TCHAR InFrame, int32 InTics, bool bInFire, int32 InFlash)
		: Frame(FString::Chr(InFrame)), Tics(InTics), bFire(bInFire), Flash(InFlash) {}

	/** Sprite frame letter. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Frame")
	FString Frame = TEXT("A");
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Frame")
	int32 Tics = 4;
	/** Perform the attack on this frame. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Frame")
	bool bFire = false;
	/** -1: no flash, otherwise the flash frame (A + n). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Frame")
	int32 Flash = -1;

	TCHAR Letter() const { return Frame.Len() ? FChar::ToUpper(Frame[0]) : TEXT('A'); }
};

/** A player weapon. */
USTRUCT(BlueprintType)
struct DOOMUE_API FDoomWeaponDef
{
	GENERATED_BODY()

	FDoomWeaponDef() = default;
	FDoomWeaponDef(EDoomWeapon InWeapon, const TCHAR* InSprite, const TCHAR* InFlash, TCHAR InReady, EDoomAmmo InAmmo, int32 InPerShot,
		TArray<FDoomWeaponFrame> InFire, const TCHAR* InSound, int32 InSlot)
		: Weapon(InWeapon), Sprite(InSprite), FlashSprite(InFlash), ReadyFrame(FString::Chr(InReady)), Ammo(InAmmo), AmmoPerShot(InPerShot),
		  Fire(MoveTemp(InFire)), Sound(InSound), Slot(InSlot) {}

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon")
	EDoomWeapon Weapon = EDoomWeapon::Fist;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon")
	FString Sprite;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon")
	FString FlashSprite;
	/** Frame letter while ready. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon")
	FString ReadyFrame = TEXT("A");
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon")
	EDoomAmmo Ammo = EDoomAmmo::None;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon")
	int32 AmmoPerShot = 0;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon")
	TArray<FDoomWeaponFrame> Fire;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon")
	FString Sound;
	/** Number key 1..7. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon")
	int32 Slot = 1;

	TCHAR ReadyLetter() const { return ReadyFrame.Len() ? FChar::ToUpper(ReadyFrame[0]) : TEXT('A'); }
};

/** Game rules DeHackEd's Misc and Ammo blocks can change. */
USTRUCT(BlueprintType)
struct DOOMUE_API FDoomRules
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Health")
	int32 InitialHealth = 100;
	/** Health bonus cap. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Health")
	int32 MaxHealth = 200;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Health")
	int32 MaxSoulsphere = 200;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Health")
	int32 SoulsphereHealth = 100;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Health")
	int32 MegasphereHealth = 200;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Health")
	int32 GodModeHealth = 100;
	/** Armor bonus cap. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Armor")
	int32 MaxArmor = 200;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Armor")
	int32 GreenArmorClass = 1;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Armor")
	int32 BlueArmorClass = 2;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Armor")
	int32 IdfaArmor = 200;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Armor")
	int32 IdkfaArmor = 200;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Ammo")
	int32 InitialBullets = 50;
	/** Bullets, shells, rockets, cells. */
	UPROPERTY(EditAnywhere, Category = "Ammo")
	int32 MaxAmmo[4] = { 200, 50, 50, 300 };
	/** Small pickup; boxes are 5x, weapons 2x. */
	UPROPERTY(EditAnywhere, Category = "Ammo")
	int32 ClipAmmo[4] = { 10, 4, 1, 20 };
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Monsters")
	bool bMonstersInfight = false;
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
	/** Back to the base tables: the UDoomGameData asset (DoomSettings::GameData) if any, else the built-in ones. */
	DOOMUE_API void ResetTables();
	/** The built-in tables (what -gamedata writes into a new asset). */
	DOOMUE_API void BuiltInTables(TArray<FDoomThingDef>& Things, TArray<FDoomProjectileDef>& Projectiles, TArray<FDoomWeaponDef>& Weapons);
	/** Where the base tables came from: the asset's path, or "built-in". */
	DOOMUE_API FString TablesSource();
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
