// UDoomWorld's endings: story text screens between episodes and chapters, end
// pictures, the bunny scroller, the Doom 2 cast call, and MAP30's monster spawner.
// The texts and cast names come from the loaded WAD's string table (DEHACKED / BEX:
// E1TEXT.., C1TEXT.., CC_ZOMBIE..), so each IWAD or PWAD shows its own.
#include "DoomWorld.h"
#include "Engine/World.h"
#include "DoomThing.h"
#include "DoomMonster.h"
#include "DoomPlayer.h"
#include "DoomGraphics.h"
#include "DoomInfo.h"
#include "DoomUE.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace
{
	struct FCastMember
	{
		int32 DoomEdNum;          // 0 = the player
		const TCHAR* Mnemonic;
	};
	const FCastMember GCast[] = {
		{ 3004, TEXT("CC_ZOMBIE") }, { 9, TEXT("CC_SHOTGUN") }, { 65, TEXT("CC_HEAVY") }, { 3001, TEXT("CC_IMP") },
		{ 3002, TEXT("CC_DEMON") }, { 3006, TEXT("CC_LOST") }, { 3005, TEXT("CC_CACO") }, { 69, TEXT("CC_HELL") },
		{ 3003, TEXT("CC_BARON") }, { 68, TEXT("CC_ARACH") }, { 71, TEXT("CC_PAIN") }, { 66, TEXT("CC_REVEN") },
		{ 67, TEXT("CC_MANCU") }, { 64, TEXT("CC_ARCH") }, { 7, TEXT("CC_SPIDER") }, { 16, TEXT("CC_CYBER") },
		{ 0, TEXT("CC_HERO") } };
	constexpr int32 CastCount = UE_ARRAY_COUNT(GCast);
	constexpr int32 CastFrameTics = 6;

	/** Doom's weighted pick of what a spawn cube hatches (out of 256). */
	int32 BrainSpawnType(int32 R)
	{
		if (R < 50) return 3001;   // imp
		if (R < 90) return 3002;   // demon
		if (R < 120) return 58;    // spectre
		if (R < 130) return 71;    // pain elemental
		if (R < 160) return 3005;  // cacodemon
		if (R < 162) return 64;    // arch-vile
		if (R < 172) return 66;    // revenant
		if (R < 192) return 68;    // arachnotron
		if (R < 222) return 67;    // mancubus
		if (R < 246) return 69;    // hell knight
		return 3003;               // baron
	}
}

// ------------------------------------------------------------------------------------------------
// Text screens
// ------------------------------------------------------------------------------------------------

void UDoomWorld::StartFinaleText(const TArray<FString>& Lines, const FString& Backdrop)
{
	FinaleText = Lines;
	FinaleBackdrop = Backdrop;
	FinaleTic = 0;
	FinaleStage = EFinaleStage::Text;
	Flow().Phase = EDoomGameState::Finale;
	PlayMusic(Wad->IsDoom2() ? TEXT("D_READ_M") : TEXT("D_VICTOR"));
}

bool UDoomWorld::StartVanillaText()
{
	FString Key, Backdrop;
	if (Flow().CurrentMap.StartsWith(TEXT("MAP")))
	{
		const int32 N = FCString::Atoi(*Flow().CurrentMap.Mid(3));
		if (Flow().bSecretExit && N == 15) { Key = TEXT("C5TEXT"); Backdrop = TEXT("RROCK13"); }
		else if (Flow().bSecretExit && N == 31) { Key = TEXT("C6TEXT"); Backdrop = TEXT("RROCK19"); }
		else if (N == 6) { Key = TEXT("C1TEXT"); Backdrop = TEXT("SLIME16"); }
		else if (N == 11) { Key = TEXT("C2TEXT"); Backdrop = TEXT("RROCK14"); }
		else if (N == 20) { Key = TEXT("C3TEXT"); Backdrop = TEXT("RROCK07"); }
		else if (N == 30) { Key = TEXT("C4TEXT"); Backdrop = TEXT("RROCK17"); }
	}
	else if (Flow().CurrentMap.Len() == 4 && Flow().CurrentMap[3] == '8' && Flow().PendingMap.IsEmpty())
	{
		static const TCHAR* Backdrops[4] = { TEXT("FLOOR4_8"), TEXT("SFLR6_1"), TEXT("MFLR8_4"), TEXT("MFLR8_3") };
		const int32 E = FMath::Clamp(Flow().CurrentMap[1] - '1', 0, 3);
		Key = FString::Printf(TEXT("E%dTEXT"), E + 1);
		Backdrop = Backdrops[E];
	}
	if (Key.IsEmpty())
	{
		return false;
	}
	// Doom 2's chapter texts need the WAD's strings; an episode ending always gets a screen
	const bool bEpisodeEnd = Flow().CurrentMap.Len() == 4 || Flow().CurrentMap == TEXT("MAP30");
	if (!DoomInfo::HasText(*Key) && !bEpisodeEnd)
	{
		return false;
	}
	FString Text = DoomInfo::Text(*Key, TEXT("Episode complete."));
	StartFinaleText({ Text }, Backdrop);
	return true;
}

bool UDoomWorld::IsFinaleTextComplete() const
{
	int32 Chars = 0;
	for (const FString& L : FinaleText) Chars += L.Len();
	return FinaleTic / 3 >= Chars;
}

void UDoomWorld::StartEnding()
{
	FinaleTic = 0;
	const FDoomMapInfo* Info = MapInfo(Flow().CurrentMap);
	const bool bDoom2 = Wad->IsDoom2();
	Flow().Phase = EDoomGameState::Finale;

	if ((Info && Info->bEndCast) || (!Info && bDoom2 && Flow().CurrentMap == TEXT("MAP30")) || (Info && !Info->bEndBunny && Info->EndPic.IsEmpty() && bDoom2 && Flow().CurrentMap == TEXT("MAP30")))
	{
		FinaleStage = EFinaleStage::Cast;
		PlayMusic(TEXT("D_EVIL"));
		StartCastMember(0);
		return;
	}
	if ((Info && Info->bEndBunny) || (!bDoom2 && Flow().CurrentMap.StartsWith(TEXT("E3"))))
	{
		FinaleStage = EFinaleStage::Bunny;
		PlayMusic(TEXT("D_BUNNY"));
		return;
	}
	FinaleStage = EFinaleStage::Picture;
	if (Info && !Info->EndPic.IsEmpty())
	{
		FinalePicture = Info->EndPic;
	}
	else if (!bDoom2 && Flow().CurrentMap.StartsWith(TEXT("E2")))
	{
		FinalePicture = TEXT("VICTORY2");
	}
	else if (!bDoom2 && Flow().CurrentMap.StartsWith(TEXT("E4")))
	{
		FinalePicture = TEXT("ENDPIC");
	}
	else
	{
		FinalePicture = Gfx->Patch(TEXT("CREDIT")) ? TEXT("CREDIT") : TEXT("HELP2");
	}
}

void UDoomWorld::FinishFinale()
{
	if (FinaleStage == EFinaleStage::Text && !Flow().PendingMap.IsEmpty())
	{
		FinaleText.Reset();
		LoadMapAsync(Flow().PendingMap, false);
		return;
	}
	if (FinaleStage == EFinaleStage::Text)
	{
		FinaleText.Reset();
		StartEnding();
		return;
	}
	// the end: back to the title screen
	FinaleText.Reset();
	UnloadMap();
	Flow().Phase = EDoomGameState::Title;
	PlayTitleMusic();
}

void UDoomWorld::FinaleInput()
{
	switch (FinaleStage)
	{
	case EFinaleStage::Text:
		if (!IsFinaleTextComplete())
		{
			FinaleTic = 1000000; // show the whole text at once
		}
		else
		{
			FinishFinale();
		}
		break;
	case EFinaleStage::Picture:
		if (FinaleTic > Doom::TicRate) FinishFinale();
		break;
	case EFinaleStage::Bunny:
		if (FinaleTic > 1180) FinishFinale(); // after THE END
		break;
	case EFinaleStage::Cast:
		if (!CastCall.bDying)
		{
			// the player shoots the cast member
			const FDoomThingDef* Def = GCast[CastCall.Index].DoomEdNum ? DoomInfo::FindThing(GCast[CastCall.Index].DoomEdNum) : nullptr;
			CastCall.bDying = true;
			CastCall.DeadTics = 0;
			PlaySound(TEXT("SHOTGN"), nullptr);
			PlaySound(Def ? Def->DeathSound : TEXT("PLDETH"), nullptr);
			SetCastSequence(Def ? (!Def->XDeath.IsEmpty() && DoomInfo::Random() < 64 ? Def->XDeath : Def->Death) : TEXT("HIJKLMN"));
		}
		break;
	}
}

// ------------------------------------------------------------------------------------------------
// Cast call
// ------------------------------------------------------------------------------------------------

FString UDoomWorld::CastSprite() const
{
	const int32 Ed = GCast[FMath::Clamp(CastCall.Index, 0, CastCount - 1)].DoomEdNum;
	const FDoomThingDef* Def = Ed ? DoomInfo::FindThing(Ed) : nullptr;
	return Def ? FString(Def->Sprite) : FString(TEXT("PLAY"));
}

FString UDoomWorld::CastName() const
{
	const FCastMember& M = GCast[FMath::Clamp(CastCall.Index, 0, CastCount - 1)];
	const FDoomThingDef* Def = M.DoomEdNum ? DoomInfo::FindThing(M.DoomEdNum) : nullptr;
	FString Fallback = Def ? FString(Def->Name) : FString(TEXT("The Marine"));
	return DoomInfo::Text(M.Mnemonic, Fallback);
}

void UDoomWorld::SetCastSequence(const FString& Frames)
{
	CastCall.Sequence = Frames.IsEmpty() ? FString(TEXT("A")) : Frames;
	CastCall.Frame = 0;
	CastCall.FrameTics = CastFrameTics;
}

void UDoomWorld::StartCastMember(int32 Index)
{
	CastCall = FCastState();
	CastCall.Index = Index % CastCount;
	const int32 Ed = GCast[CastCall.Index].DoomEdNum;
	const FDoomThingDef* Def = Ed ? DoomInfo::FindThing(Ed) : nullptr;
	SetCastSequence(Def ? (!Def->Run.IsEmpty() ? Def->Run : Def->Idle) : TEXT("ABCD"));
	if (Def && !Def->SeeSound.IsEmpty())
	{
		PlaySound(Def->SeeSound, nullptr);
	}
}

void UDoomWorld::TicFinale()
{
	++FinaleTic;
	if (FinaleStage == EFinaleStage::Bunny)
	{
		// the letters of THE END appear one by one with a shot each
		const int32 Stage = (FinaleTic - 1180) / 5;
		if (FinaleTic >= 1180 && (FinaleTic - 1180) % 5 == 0 && Stage <= 6)
		{
			PlaySound(TEXT("PISTOL"), nullptr);
		}
		return;
	}
	if (FinaleStage != EFinaleStage::Cast)
	{
		return;
	}
	const int32 Ed = GCast[CastCall.Index].DoomEdNum;
	const FDoomThingDef* Def = Ed ? DoomInfo::FindThing(Ed) : nullptr;
	if (--CastCall.FrameTics > 0)
	{
		return;
	}
	CastCall.FrameTics = CastFrameTics;
	if (CastCall.Frame + 1 < CastCall.Sequence.Len())
	{
		++CastCall.Frame;
		return;
	}
	if (CastCall.bDying)
	{
		// hold the corpse a moment, then the next member walks on
		if (++CastCall.DeadTics > 6)
		{
			StartCastMember(CastCall.Index + 1);
		}
		return;
	}
	if (CastCall.bAttacking)
	{
		CastCall.bAttacking = false;
		SetCastSequence(Def ? (!Def->Run.IsEmpty() ? Def->Run : Def->Idle) : TEXT("ABCD"));
		return;
	}
	// walked a while: show off an attack
	if (++CastCall.Walked % 3 == 0)
	{
		const FString Attack = Def ? (!Def->Attack.IsEmpty() ? FString(Def->Attack) : FString(Def->Melee)) : FString(TEXT("EF"));
		if (!Attack.IsEmpty())
		{
			CastCall.bAttacking = true;
			SetCastSequence(Attack);
			PlaySound(Def ? (!Def->AttackSound.IsEmpty() ? Def->AttackSound : TEXT("FIRSHT")) : TEXT("PISTOL"), nullptr);
			return;
		}
	}
	CastCall.Frame = 0;
}

// ------------------------------------------------------------------------------------------------
// MAP30: the Icon of Sin's spawn cubes
// ------------------------------------------------------------------------------------------------

void UDoomWorld::TicBrain()
{
	if (BrainExitTics > 0)
	{
		// the brain's death: explosions along the wall, then the end
		if ((Flow().LevelTic & 3) == 0)
		{
			for (const TWeakObjectPtr<ADoomMonster>& M : Monsters)
			{
				if (M.IsValid() && M->Def->Name.Equals(TEXT("BossBrain"), ESearchCase::CaseSensitive))
				{
					const FVector Off((DoomInfo::Random() - 128) * 2.f, (DoomInfo::Random() - 128) * 2.f, DoomInfo::Random() * 0.5f);
					const FVector Pos = M->GetActorLocation() + Off;
					SpawnEffect(TEXT("MISL"), TEXT("BCD"), 8, Pos, true);
					PlaySound(TEXT("BAREXP"), &Pos, 0.6f);
					break;
				}
			}
		}
		if (--BrainExitTics == 0)
		{
			ExitLevel(false);
		}
		return;
	}
	if (BrainShooters.Num() == 0 || BrainTargets.Num() == 0)
	{
		return;
	}
	ADoomPlayer* P = GetPlayer();
	static const bool bForceAwake = FParse::Param(FCommandLine::Get(), TEXT("brainawake"));
	if (!bBrainAwake && bForceAwake)
	{
		bBrainAwake = true;
		BrainTimer = 1;
	}
	if (!bBrainAwake)
	{
		// wakes when a shooter can see the player
		if (!P || P->IsDead())
		{
			return;
		}
		for (const FVector& S : BrainShooters)
		{
			FCollisionObjectQueryParams Obj;
			Obj.AddObjectTypesToQuery(ECC_WorldStatic);
			FHitResult Hit;
			const FVector Eye = S + FVector(0, 0, 32.f * Doom::Scale);
			if (FVector::Dist(Eye, P->GetActorLocation()) < 4096.f * Doom::Scale
				&& !GetWorld()->LineTraceSingleByObjectType(Hit, Eye, P->EyeLocation(), Obj))
			{
				bBrainAwake = true;
				BrainTimer = Doom::TicRate;
				PlaySound(TEXT("BOSSIT"), nullptr);
				break;
			}
		}
		return;
	}

	// spit a cube every few seconds (half as often on the easy skills)
	if (--BrainTimer <= 0)
	{
		BrainTimer = Skill <= 2 ? 300 : 150;
		for (const FVector& S : BrainShooters)
		{
			const FVector From = S + FVector(0, 0, 32.f * Doom::Scale);
			const FVector To = BrainTargets[BrainTargetIndex++ % BrainTargets.Num()];
			const float Speed = 10.f * Doom::Scale; // units per tic
			const int32 Tics = FMath::Max(1, FMath::CeilToInt(FVector::Dist(From, To) / Speed));
			FBrainCube Cube;
			Cube.Visual = SpawnEffect(TEXT("BOSF"), TEXT("ABCD"), 3, From, true, true);
			Cube.Velocity = (To - From) / Tics;
			Cube.Target = To;
			Cube.TicsLeft = Tics;
			BrainCubes.Add(Cube);
			PlaySound(TEXT("BOSPIT"), nullptr);
		}
	}

	for (int32 i = BrainCubes.Num() - 1; i >= 0; --i)
	{
		FBrainCube& C = BrainCubes[i];
		if (C.Visual.IsValid())
		{
			C.Visual->SetActorLocation(C.Visual->GetActorLocation() + C.Velocity);
			if ((Flow().LevelTic % 16) == 0)
			{
				const FVector L = C.Visual->GetActorLocation();
				PlaySound(TEXT("BOSCUB"), &L, 0.6f);
			}
		}
		if (--C.TicsLeft > 0)
		{
			continue;
		}
		// arrival: fire, a teleport sound, and something nasty
		if (C.Visual.IsValid())
		{
			C.Visual->Destroy();
		}
		SpawnEffect(TEXT("FIRE"), TEXT("ABCDEFGH"), 4, C.Target + FVector(0, 0, 28.f * Doom::Scale), true);
		PlaySound(TEXT("TELEPT"), &C.Target);
		if (ADoomMonster* M = SpawnMonster(BrainSpawnType(DoomInfo::Random()), C.Target, DoomInfo::Random() * 360.f / 256.f))
		{
			UE_LOG(LogDoomGame, Log, TEXT("Spawn cube hatched a %s"), *M->Def->Name);
			--Flow().TotalKills; // spawned monsters do not count towards the level's total
			if (P)
			{
				M->WakeUp(P, false);
			}
		}
		BrainCubes.RemoveAtSwap(i);
	}
}
