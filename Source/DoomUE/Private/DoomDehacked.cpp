#include "DoomDehacked.h"
#include "DoomInfo.h"
#include "DoomUE.h"

namespace
{
	/**
	 * DeHackEd thing numbers are 1-based indices of Doom's mobj type list. Only types the
	 * port models are mapped: monsters, the barrel, missiles and the pickups.
	 * Positive = DoomEdNum of a thing, negative = projectile (see ProjectileFor).
	 */
	int32 MapThingNumber(int32 N)
	{
		static const int32 Table[] = {
			0,
			/* 1 player */ 0, 3004, 9, 64, 0, 66, -7, 0, 67, -10,
			/* 11 */ 65, 3001, 3002, 58, 3005, 3003, -17, 69, 3006, 7,
			/* 21 */ 68, 16, 71, 84, 72, 88, 0, 0, 0, 0,
			/* 31 */ 2035, -32, -33, -34, -35, -36, -37, 0, 0, 0,
			/* 41 */ 0, 0, 0, 2018, 2019, 2014, 2015, 5, 13, 6,
			/* 51 */ 39, 38, 40, 2011, 2012, 2013, 2022, 2023, 2024, 2025,
			/* 61 */ 2026, 2045, 83, 2007, 2048, 2010, 2046, 2047, 17, 2008,
			/* 71 */ 2049, 8, 2006, 2002, 2005, 2003, 2004, 2001, 82 };
		return N >= 0 && N < UE_ARRAY_COUNT(Table) ? Table[N] : 0;
	}

	const TCHAR* ProjectileFor(int32 Code)
	{
		switch (-Code)
		{
		case 7: return TEXT("Tracer");
		case 10: return TEXT("FatShot");
		case 17: return TEXT("BaronBall");
		case 32: return TEXT("ImpBall");
		case 33: return TEXT("CacoBall");
		case 34: return TEXT("Rocket");
		case 35: return TEXT("Plasma");
		case 36: return TEXT("BFGBall");
		case 37: return TEXT("ArachPlasma");
		default: return nullptr;
		}
	}

	/** Sizes and missile speeds are 16.16 fixed point; small values are taken as whole units. */
	float Units(int64 V)
	{
		return FMath::Abs(V) >= 65536 ? V / 65536.f : (float)V;
	}

	FString Unescape(const FString& S)
	{
		return S.Replace(TEXT("\\n"), TEXT("\n")).Replace(TEXT("\\\""), TEXT("\""));
	}
}

FDehackedResult DoomDehacked::Apply(const FString& Source)
{
	FDehackedResult R;
	TArray<FString> Lines;
	Source.Replace(TEXT("\r"), TEXT("")).ParseIntoArray(Lines, TEXT("\n"), false);

	enum class EBlock { None, Thing, Misc, Ammo, Pars, Strings, Skip };
	EBlock Block = EBlock::None;
	int32 ThingCode = 0;
	int32 AmmoIndex = -1;

	for (int32 i = 0; i < Lines.Num(); ++i)
	{
		FString Line = Lines[i].TrimStartAndEnd();
		if (Line.IsEmpty() || Line.StartsWith(TEXT("#")))
		{
			continue;
		}
		// --- block headers ------------------------------------------------------------------
		if (Line.StartsWith(TEXT("[")))
		{
			const FString Section = Line.ToUpper();
			Block = Section == TEXT("[PARS]") ? EBlock::Pars : Section == TEXT("[STRINGS]") ? EBlock::Strings : EBlock::Skip;
			if (Block == EBlock::Skip) ++R.Skipped;
			continue;
		}
		TArray<FString> Words;
		Line.ParseIntoArrayWS(Words);
		const FString Head = Words.Num() ? Words[0].ToLower() : FString();
		if (Words.Num() >= 2 && !Line.Contains(TEXT("=")) && Words[1].IsNumeric())
		{
			if (Head == TEXT("thing"))
			{
				ThingCode = MapThingNumber(FCString::Atoi(*Words[1]));
				Block = ThingCode != 0 ? EBlock::Thing : EBlock::Skip;
				if (!ThingCode) ++R.Skipped;
				continue;
			}
			if (Head == TEXT("misc")) { Block = EBlock::Misc; continue; }
			if (Head == TEXT("ammo")) { Block = EBlock::Ammo; AmmoIndex = FCString::Atoi(*Words[1]); continue; }
			if (Head == TEXT("text") && Words.Num() >= 3)
			{
				// "Text <old length> <new length>": the next characters are the old text
				// then the new one, newlines included. Without the original strings to match,
				// the port skips them.
				int32 Remaining = FCString::Atoi(*Words[1]) + FCString::Atoi(*Words[2]);
				while (Remaining > 0 && i + 1 < Lines.Num())
				{
					++i;
					Remaining -= Lines[i].Len() + 1;
				}
				Block = EBlock::None;
				++R.Skipped;
				continue;
			}
			if (Head == TEXT("frame") || Head == TEXT("pointer") || Head == TEXT("sound") || Head == TEXT("sprite") || Head == TEXT("weapon") || Head == TEXT("cheat"))
			{
				Block = EBlock::Skip;
				++R.Skipped;
				continue;
			}
		}
		if (Head == TEXT("patch") || Head == TEXT("doom"))
		{
			continue; // "Patch File for DeHackEd", "Doom version = 19"
		}

		// --- block contents -----------------------------------------------------------------
		if (Block == EBlock::Pars)
		{
			if (Head == TEXT("par") && (Words.Num() == 3 || Words.Num() == 4))
			{
				const bool bEpisodic = Words.Num() == 4;
				const FString Map = bEpisodic ? FString::Printf(TEXT("E%sM%s"), *Words[1], *Words[2]) : FString::Printf(TEXT("MAP%02d"), FCString::Atoi(*Words[1]));
				DoomInfo::SetParTime(Map, FCString::Atoi(*Words.Last()));
				++R.Pars;
			}
			continue;
		}
		FString Key, Value;
		if (!Line.Split(TEXT("="), &Key, &Value))
		{
			continue;
		}
		Key = Key.TrimStartAndEnd();
		Value = Value.TrimStartAndEnd();

		if (Block == EBlock::Strings)
		{
			// a trailing backslash continues the value on the next line
			while (Value.EndsWith(TEXT("\\")) && i + 1 < Lines.Num())
			{
				Value.LeftChopInline(1);
				Value += Lines[++i].TrimStartAndEnd();
			}
			DoomInfo::SetString(Key, Unescape(Value));
			++R.Strings;
			continue;
		}
		const FString K = Key.ToLower();
		const int64 N = FCString::Atoi64(*Value);
		if (Block == EBlock::Thing)
		{
			if (ThingCode < 0)
			{
				FDoomProjectileDef* P = DoomInfo::MutableProjectile(ProjectileFor(ThingCode));
				if (!P) continue;
				if (K == TEXT("speed")) { P->Speed = Units(N); ++R.Things; }
				else if (K == TEXT("missile damage")) { P->DamageMul = (int32)N; ++R.Things; }
				else if (K == TEXT("width")) { P->Radius = Units(N); ++R.Things; }
				continue;
			}
			FDoomThingDef* T = DoomInfo::MutableThing(ThingCode);
			if (!T) continue;
			if (K == TEXT("hit points")) { T->Health = (int32)N; ++R.Things; }
			else if (K == TEXT("speed")) { T->Speed = Units(N); ++R.Things; }
			else if (K == TEXT("width")) { T->Radius = Units(N); ++R.Things; }
			else if (K == TEXT("height")) { T->Height = Units(N); ++R.Things; }
			else if (K == TEXT("pain chance")) { T->PainChance = (int32)N; ++R.Things; }
			else if (K == TEXT("missile damage")) { T->MeleeDamage = (int32)N; ++R.Things; }
		}
		else if (Block == EBlock::Misc)
		{
			FDoomRules& Rules = DoomInfo::Rules();
			const int32 V = (int32)N;
			if (K == TEXT("initial health")) Rules.InitialHealth = V;
			else if (K == TEXT("initial bullets")) Rules.InitialBullets = V;
			else if (K == TEXT("max health")) Rules.MaxHealth = V;
			else if (K == TEXT("max armor")) Rules.MaxArmor = V;
			else if (K == TEXT("green armor class")) Rules.GreenArmorClass = V;
			else if (K == TEXT("blue armor class")) Rules.BlueArmorClass = V;
			else if (K == TEXT("max soulsphere")) Rules.MaxSoulsphere = V;
			else if (K == TEXT("soulsphere health")) Rules.SoulsphereHealth = V;
			else if (K == TEXT("megasphere health")) Rules.MegasphereHealth = V;
			else if (K == TEXT("god mode health")) Rules.GodModeHealth = V;
			else if (K == TEXT("idfa armor")) Rules.IdfaArmor = V;
			else if (K == TEXT("idkfa armor")) Rules.IdkfaArmor = V;
			else if (K == TEXT("bfg cells/shot")) DoomInfo::MutableWeapon(EDoomWeapon::BFG).AmmoPerShot = V;
			else if (K == TEXT("monsters infight")) Rules.bMonstersInfight = V == 221;
			else continue;
			++R.Misc;
		}
		else if (Block == EBlock::Ammo && AmmoIndex >= 0 && AmmoIndex < 4)
		{
			if (K == TEXT("max ammo")) { DoomInfo::Rules().MaxAmmo[AmmoIndex] = (int32)N; ++R.Ammo; }
			else if (K == TEXT("per ammo")) { DoomInfo::Rules().ClipAmmo[AmmoIndex] = (int32)N; ++R.Ammo; }
		}
	}
	return R;
}
