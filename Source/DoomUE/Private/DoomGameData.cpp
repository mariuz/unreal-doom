// UDoomGameData's checks: what Tools > Validate Data reports for DA_DoomGameData, and what the game
// logs when it loads the tables (DoomInfo::ResetTables).
#include "DoomGameData.h"
#include "DoomUE.h"
#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

#define LOCTEXT_NAMESPACE "DoomGameData"

namespace DoomGameDataCheck
{
	FText ThingName(const FDoomThingDef& D)
	{
		return FText::Format(LOCTEXT("ThingName", "Thing {0} ({1})"), DoomText::Number(D.DoomEdNum),
			D.Name.IsEmpty() ? LOCTEXT("NoName", "no name") : FText::FromString(D.Name));
	}

	FText ProjectileName(int32 Index, const FDoomProjectileDef& D)
	{
		return D.Name.IsEmpty() ? FText::Format(LOCTEXT("ProjectileByName", "Projectile {0}"), DoomText::Number(Index))
			: FText::Format(LOCTEXT("ProjectileName", "Projectile {0}"), FText::FromString(D.Name));
	}

	FText WeaponName(const FDoomWeaponDef& D)
	{
		const UEnum* Enum = StaticEnum<EDoomWeapon>();
		return FText::Format(LOCTEXT("WeaponName", "Weapon {0}"), Enum ? Enum->GetDisplayNameTextByValue((int64)D.Weapon) : DoomText::Number((int32)D.Weapon));
	}

	/** Sprite frame letters run from A to ] (Doom's 29 frames); the game reads them upper-cased. */
	bool FramesValid(const FString& Frames)
	{
		for (const TCHAR C : Frames)
		{
			const TCHAR U = FChar::ToUpper(C);
			if (U < TEXT('A') || U > TEXT(']')) return false;
		}
		return true;
	}

	/** Every sprite is a four-character lump prefix (POSS, TROO...). */
	bool SpriteValid(const FString& Sprite)
	{
		return Sprite.Len() == 4;
	}

	/** Sound lumps are DS plus up to six characters. */
	bool SoundValid(const FString& Sound)
	{
		return Sound.Len() <= 6;
	}
}

void UDoomGameData::CheckTables(TConstArrayView<FDoomThingDef> Things, TConstArrayView<FDoomProjectileDef> Projectiles,
	TConstArrayView<FDoomWeaponDef> Weapons, const FDoomRules& Rules, FDoomDataProblems& Out)
{
	using namespace DoomGameDataCheck;
	auto Error = [&Out](const FText& Who, const FText& What) { Out.Errors.Add(FText::Format(LOCTEXT("Problem", "{0}: {1}"), Who, What)); };
	auto Warn = [&Out](const FText& Who, const FText& What) { Out.Warnings.Add(FText::Format(LOCTEXT("Problem", "{0}: {1}"), Who, What)); };

	TMap<FString, int32> ProjectileByName;
	for (int32 i = 0; i < Projectiles.Num(); ++i)
	{
		const FDoomProjectileDef& P = Projectiles[i];
		const FText Who = ProjectileName(i, P);
		if (P.Name.IsEmpty())
		{
			Error(Who, LOCTEXT("ProjectileNoName", "has no name, so no monster can fire it"));
		}
		else if (ProjectileByName.Contains(P.Name))
		{
			Error(Who, FText::Format(LOCTEXT("ProjectileDup", "the name is also projectile {0}'s (monsters fire the first)"), DoomText::Number(ProjectileByName[P.Name])));
		}
		else
		{
			ProjectileByName.Add(P.Name, i);
		}
		if (!SpriteValid(P.Sprite)) Error(Who, FText::Format(LOCTEXT("BadSprite", "sprite \"{0}\" is not four characters"), FText::FromString(P.Sprite)));
		if (P.Fly.IsEmpty()) Error(Who, LOCTEXT("NoFly", "has no flight frames"));
		if (!FramesValid(P.Fly) || !FramesValid(P.Explode)) Error(Who, LOCTEXT("BadProjectileFrames", "frame letters must be A to ]"));
		if (P.Speed <= 0.f) Error(Who, LOCTEXT("ProjectileSpeed", "speed must be above 0"));
		if (P.Radius <= 0.f) Error(Who, LOCTEXT("ProjectileRadius", "radius must be above 0"));
		if (P.DamageMul < 0 || P.SplashDamage < 0) Error(Who, LOCTEXT("ProjectileDamage", "damage can't be negative"));
		if (!SoundValid(P.FireSound) || !SoundValid(P.HitSound)) Warn(Who, LOCTEXT("ProjectileSound", "a sound name is longer than six characters (lumps are DS plus six)"));
	}

	TMap<int32, int32> ThingByNumber;
	TSet<FString> ThingNames;
	for (int32 i = 0; i < Things.Num(); ++i)
	{
		const FDoomThingDef& D = Things[i];
		if (const int32* Other = ThingByNumber.Find(D.DoomEdNum))
		{
			Error(ThingName(D), FText::Format(LOCTEXT("ThingDup", "the number is also {0}'s (maps get the first)"), FText::FromString(Things[*Other].Name)));
		}
		else
		{
			ThingByNumber.Add(D.DoomEdNum, i);
		}
		if (D.Name.IsEmpty())
		{
			Error(ThingName(D), LOCTEXT("ThingNoName", "has no name"));
		}
		else if (ThingNames.Contains(D.Name.ToLower()))
		{
			Warn(ThingName(D), LOCTEXT("ThingNameDup", "another thing has the same name (finding things by name gets the first)"));
		}
		ThingNames.Add(D.Name.ToLower());
	}

	for (const FDoomThingDef& D : Things)
	{
		const FText Who = ThingName(D);
		const bool bMonster = (D.Flags & EThingFlag::Monster) != 0;
		if (!SpriteValid(D.Sprite)) Error(Who, FText::Format(LOCTEXT("BadSprite", "sprite \"{0}\" is not four characters"), FText::FromString(D.Sprite)));
		if (D.Idle.IsEmpty()) Error(Who, LOCTEXT("NoIdle", "has no idle frames, so it has no picture"));
		for (const FString* F : { &D.Idle, &D.Run, &D.Melee, &D.Attack, &D.Pain, &D.Death, &D.XDeath })
		{
			if (!FramesValid(*F))
			{
				Error(Who, FText::Format(LOCTEXT("BadFrames", "frames \"{0}\": frame letters must be A to ]"), FText::FromString(*F)));
			}
		}
		if (D.Radius <= 0.f || D.Height <= 0.f) Error(Who, LOCTEXT("ThingSize", "radius and height must be above 0"));
		if (D.IdleTics <= 0 || D.RunTics <= 0) Error(Who, LOCTEXT("ThingTics", "idle and run tics must be above 0"));
		if (D.PainChance < 0 || D.PainChance > 256) Error(Who, LOCTEXT("PainChance", "pain chance must be 0 to 256"));
		if (bMonster)
		{
			if (D.Health <= 0) Error(Who, LOCTEXT("MonsterHealth", "a monster needs health above 0"));
			if (D.Speed > 0.f && D.Run.IsEmpty()) Error(Who, LOCTEXT("MonsterRun", "a monster that moves needs run frames"));
			if (D.Death.IsEmpty()) Error(Who, LOCTEXT("MonsterDeath", "a monster needs death frames"));
		}
		else if ((D.Flags & EThingFlag::Shootable) && D.Health <= 0)
		{
			Error(Who, LOCTEXT("ShootableHealth", "a shootable thing needs health above 0"));
		}
		const bool bMissile = D.Attack_ == EDoomAttack::Missile || D.Attack_ == EDoomAttack::MeleeOrMissile;
		const bool bMelee = D.Attack_ == EDoomAttack::Melee || D.Attack_ == EDoomAttack::MeleeOrMissile;
		if (!D.Projectile.IsEmpty() && !ProjectileByName.Contains(D.Projectile))
		{
			Error(Who, FText::Format(LOCTEXT("UnknownProjectile", "fires \"{0}\", which isn't in the projectiles"), FText::FromString(D.Projectile)));
		}
		else if (bMissile && D.Projectile.IsEmpty())
		{
			Error(Who, LOCTEXT("NoProjectile", "a missile attack needs a projectile"));
		}
		// a melee-or-missile monster without melee frames only fires (the cacodemon bites with its attack frames)
		if (D.Attack_ == EDoomAttack::Melee && D.Melee.IsEmpty()) Error(Who, LOCTEXT("NoMeleeFrames", "a melee attack needs melee frames"));
		if (bMelee && !D.Melee.IsEmpty() && D.MeleeDamage <= 0) Warn(Who, LOCTEXT("NoMeleeDamage", "a melee attack with no damage"));
		if (D.Attack_ != EDoomAttack::None && D.Attack_ != EDoomAttack::Melee && D.Attack.IsEmpty())
		{
			Error(Who, LOCTEXT("NoAttackFrames", "an attack needs attack frames"));
		}
		if (D.Attack_ != EDoomAttack::None && !bMonster) Warn(Who, LOCTEXT("AttackNotMonster", "has an attack but isn't a monster"));
		if (D.DropType != 0 && !ThingByNumber.Contains(D.DropType))
		{
			Error(Who, FText::Format(LOCTEXT("UnknownDrop", "drops thing {0}, which isn't in the things"), DoomText::Number(D.DropType)));
		}
		if (D.Pickup != EPickup::None && !(D.Flags & EThingFlag::Pickup))
		{
			Warn(Who, LOCTEXT("PickupFlag", "gives a pickup but hasn't the Pickup flag, so it can't be picked up"));
		}
		for (const FString* S : { &D.SeeSound, &D.AttackSound, &D.PainSound, &D.DeathSound, &D.ActiveSound })
		{
			if (!SoundValid(*S))
			{
				Warn(Who, FText::Format(LOCTEXT("ThingSound", "sound \"{0}\" is longer than six characters (lumps are DS plus six)"), FText::FromString(*S)));
			}
		}
	}

	for (const FDoomWeaponDef& W : Weapons)
	{
		const FText Who = WeaponName(W);
		if (!SpriteValid(W.Sprite)) Error(Who, FText::Format(LOCTEXT("BadSprite", "sprite \"{0}\" is not four characters"), FText::FromString(W.Sprite)));
		if (!W.FlashSprite.IsEmpty() && !SpriteValid(W.FlashSprite))
		{
			Error(Who, FText::Format(LOCTEXT("BadFlash", "flash sprite \"{0}\" is not four characters"), FText::FromString(W.FlashSprite)));
		}
		if (W.ReadyFrame.IsEmpty() || !FramesValid(W.ReadyFrame)) Error(Who, LOCTEXT("BadReady", "the ready frame must be a letter A to ]"));
		if (W.Slot < 1 || W.Slot > 7) Error(Who, LOCTEXT("BadSlot", "the slot must be 1 to 7"));
		if (W.Fire.IsEmpty())
		{
			Error(Who, LOCTEXT("NoFire", "has no firing frames"));
		}
		bool bFires = false;
		int32 Tics = 0;
		for (const FDoomWeaponFrame& F : W.Fire)
		{
			bFires |= F.bFire;
			Tics += FMath::Max(0, F.Tics);
			if (F.Tics < 0) Error(Who, LOCTEXT("FrameTics", "a firing frame can't have negative tics (0 skips it)"));
			if (F.Frame.IsEmpty() || !FramesValid(F.Frame)) Error(Who, LOCTEXT("FrameLetter", "a firing frame's letter must be A to ]"));
			if (F.Flash < -1 || F.Flash > 28) Error(Who, LOCTEXT("FrameFlash", "a flash frame must be -1 (none) or 0 to 28"));
		}
		if (!W.Fire.IsEmpty() && Tics == 0) Error(Who, LOCTEXT("NoTics", "the firing frames add up to no tics"));
		if (!W.Fire.IsEmpty() && !bFires) Warn(Who, LOCTEXT("NeverFires", "no firing frame attacks"));
		if (W.Ammo == EDoomAmmo::None && W.AmmoPerShot > 0) Warn(Who, LOCTEXT("AmmoNone", "uses ammo per shot but has no ammo type"));
		if (W.Ammo != EDoomAmmo::None && W.AmmoPerShot <= 0) Warn(Who, LOCTEXT("AmmoFree", "has an ammo type but uses none per shot"));
		if (!SoundValid(W.Sound)) Warn(Who, LOCTEXT("WeaponSound", "the sound is longer than six characters (lumps are DS plus six)"));
	}

	const FText RulesWho = LOCTEXT("Rules", "Rules");
	if (Rules.InitialHealth <= 0) Error(RulesWho, LOCTEXT("InitialHealth", "initial health must be above 0"));
	if (Rules.MaxHealth < Rules.InitialHealth) Warn(RulesWho, LOCTEXT("MaxHealth", "the health bonus cap is below the initial health"));
	if (Rules.MaxSoulsphere <= 0 || Rules.SoulsphereHealth < 0 || Rules.MegasphereHealth < 0 || Rules.GodModeHealth <= 0)
	{
		Error(RulesWho, LOCTEXT("PowerHealth", "soulsphere, megasphere and god mode health must be positive"));
	}
	if (Rules.MaxArmor < 0 || Rules.IdfaArmor < 0 || Rules.IdkfaArmor < 0) Error(RulesWho, LOCTEXT("Armor", "armor amounts can't be negative"));
	if (Rules.GreenArmorClass < 1 || Rules.BlueArmorClass < 1) Error(RulesWho, LOCTEXT("ArmorClass", "armor classes must be 1 or more"));
	if (Rules.InitialBullets < 0) Error(RulesWho, LOCTEXT("InitialBullets", "initial bullets can't be negative"));
	for (int32 i = 0; i < 4; ++i)
	{
		if (Rules.MaxAmmo[i] <= 0 || Rules.ClipAmmo[i] < 0)
		{
			Error(RulesWho, FText::Format(LOCTEXT("Ammo", "ammo {0}: the maximum must be above 0 and the clip 0 or more"), DoomText::Number(i)));
		}
	}
}

void UDoomGameData::Check(FDoomDataProblems& Out) const
{
	// the tables the game would start from (DoomInfo::ResetTables)
	TArray<FDoomThingDef> BuiltThings;
	TArray<FDoomProjectileDef> BuiltProjectiles;
	TArray<FDoomWeaponDef> EffectiveWeapons;
	DoomInfo::BuiltInTables(BuiltThings, BuiltProjectiles, EffectiveWeapons);
	TSet<EDoomWeapon> Seen;
	for (const FDoomWeaponDef& W : Weapons)
	{
		if ((int32)W.Weapon >= EffectiveWeapons.Num())
		{
			Out.Errors.Add(FText::Format(LOCTEXT("WeaponKind", "Weapon entry {0} isn't a weapon the game has"), DoomText::Number((int32)W.Weapon)));
			continue;
		}
		if (Seen.Contains(W.Weapon))
		{
			Out.Warnings.Add(FText::Format(LOCTEXT("WeaponDup", "{0}: listed twice (the last entry wins)"), DoomGameDataCheck::WeaponName(W)));
		}
		Seen.Add(W.Weapon);
		EffectiveWeapons[(int32)W.Weapon] = W;
	}
	CheckTables(Things.Num() ? TConstArrayView<FDoomThingDef>(Things) : TConstArrayView<FDoomThingDef>(BuiltThings),
		Projectiles.Num() ? TConstArrayView<FDoomProjectileDef>(Projectiles) : TConstArrayView<FDoomProjectileDef>(BuiltProjectiles),
		EffectiveWeapons, Rules, Out);
}

#if WITH_EDITOR
EDataValidationResult UDoomGameData::IsDataValid(FDataValidationContext& Context) const
{
	EDataValidationResult Result = Super::IsDataValid(Context);
	FDoomDataProblems Problems;
	Check(Problems);
	for (const FText& E : Problems.Errors) Context.AddError(E);
	for (const FText& W : Problems.Warnings) Context.AddWarning(W);
	if (Problems.Errors.Num())
	{
		return EDataValidationResult::Invalid;
	}
	return Result == EDataValidationResult::NotValidated ? EDataValidationResult::Valid : Result;
}
#endif

#undef LOCTEXT_NAMESPACE
