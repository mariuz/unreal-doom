// ADoomPlayer: weapons: choosing and cycling them, raising and lowering, firing (Doom's weapon
// states), autoaim, hitscan and melee attacks.
#include "DoomPlayer.h"
#include "DoomPlayerPrivate.h"
#include "Engine/World.h"
#include "Components/SkeletalMeshComponent.h"
#include "DoomWorld.h"
#include "DoomMonster.h"
#include "DoomThing.h"
#include "DoomPlayerController.h"
#include "DoomSettings.h"
#include "DoomLevel.h"
#include "Interfaces/MovementBaseInterface.h"
#include "DoomGraphics.h"
#include "DoomUE.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/PointLightComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "EnhancedInputComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/DamageEvents.h"
#include "DoomSpriteComponent.h"
#include "DoomGameState.h"
#include "DoomGameMode.h"
#include "Net/UnrealNetwork.h"

void ADoomPlayer::SelectSlot(int32 Slot)
{
	if (IsDead() || InputLocked())
	{
		return;
	}
	if (!HasAuthority())
	{
		ServerSelectSlot(Slot);
		return;
	}
	SelectSlotNow(Slot);
}

void ADoomPlayer::SelectSlotNow(int32 Slot)
{
	if (IsDead())
	{
		return;
	}
	EDoomWeapon W = EDoomWeapon::Count;
	switch (Slot)
	{
	case 1:
		W = (Inv().Weapons[(int32)EDoomWeapon::Chainsaw] && !(Inv().Ready == EDoomWeapon::Chainsaw && View.bBerserk)) && Inv().Ready != EDoomWeapon::Chainsaw
			? EDoomWeapon::Chainsaw : EDoomWeapon::Fist;
		break;
	case 2: W = EDoomWeapon::Pistol; break;
	case 3:
		W = (Inv().Weapons[(int32)EDoomWeapon::SuperShotgun] && Inv().Ready != EDoomWeapon::SuperShotgun) ? EDoomWeapon::SuperShotgun : EDoomWeapon::Shotgun;
		if (!Inv().Weapons[(int32)W]) W = EDoomWeapon::SuperShotgun;
		break;
	case 4: W = EDoomWeapon::Chaingun; break;
	case 5: W = EDoomWeapon::Rocket; break;
	case 6: W = EDoomWeapon::Plasma; break;
	case 7: W = EDoomWeapon::BFG; break;
	default: break;
	}
	if (W != EDoomWeapon::Count && Inv().Weapons[(int32)W] && W != Inv().Ready)
	{
		PendingWeapon = W;
	}
}

void ADoomPlayer::CycleWeapon(int32 Dir)
{
	if (!HasAuthority())
	{
		ServerCycleWeapon(Dir);
		return;
	}
	CycleWeaponNow(Dir);
}

void ADoomPlayer::CycleWeaponNow(int32 Dir)
{
	int32 Cur = 0;
	for (int32 i = 0; i < UE_ARRAY_COUNT(DoomPlayerDetail::CycleOrder); ++i) if (DoomPlayerDetail::CycleOrder[i] == Inv().Ready) Cur = i;
	for (int32 k = 1; k < UE_ARRAY_COUNT(DoomPlayerDetail::CycleOrder); ++k)
	{
		const EDoomWeapon W = DoomPlayerDetail::CycleOrder[(Cur + Dir * k + UE_ARRAY_COUNT(DoomPlayerDetail::CycleOrder) * 2) % UE_ARRAY_COUNT(DoomPlayerDetail::CycleOrder)];
		const FDoomWeaponDef& D = DoomInfo::Weapon(W);
		if (Inv().Weapons[(int32)W] && (D.Ammo == EDoomAmmo::None || Inv().Ammo[(int32)D.Ammo] >= D.AmmoPerShot))
		{
			PendingWeapon = W;
			return;
		}
	}
}

void ADoomPlayer::SetWeaponSprite()
{
	const FDoomWeaponDef& D = DoomInfo::Weapon(Inv().Ready);
	View.WeaponSprite = D.Sprite;
	View.FlashSprite = D.FlashSprite;
	View.WeaponFrame = D.ReadyLetter();
}

bool ADoomPlayer::CheckAmmo()
{
	const FDoomWeaponDef& D = DoomInfo::Weapon(Inv().Ready);
	if (D.Ammo == EDoomAmmo::None || Inv().Ammo[(int32)D.Ammo] >= D.AmmoPerShot)
	{
		return true;
	}
	// out of ammo: Doom's preference order
	static const EDoomWeapon Pref[] = { EDoomWeapon::Plasma, EDoomWeapon::SuperShotgun, EDoomWeapon::Chaingun, EDoomWeapon::Shotgun,
		EDoomWeapon::Pistol, EDoomWeapon::Chainsaw, EDoomWeapon::Rocket, EDoomWeapon::BFG, EDoomWeapon::Fist };
	for (EDoomWeapon W : Pref)
	{
		const FDoomWeaponDef& WD = DoomInfo::Weapon(W);
		if (Inv().Weapons[(int32)W] && (WD.Ammo == EDoomAmmo::None || Inv().Ammo[(int32)WD.Ammo] >= WD.AmmoPerShot))
		{
			if (W != Inv().Ready) PendingWeapon = W;
			break;
		}
	}
	return false;
}

void ADoomPlayer::TicWeapon()
{
	const FDoomWeaponDef& D = DoomInfo::Weapon(Inv().Ready);
	if (FlashTics > 0 && --FlashTics == 0)
	{
		View.FlashFrame = 0;
		View.ExtraLight = 0;
	}

	switch (Phase)
	{
	case EWeaponPhase::Raising:
		View.WeaponY -= DoomPlayerDetail::RaiseSpeed;
		if (View.WeaponY <= DoomPlayerDetail::WeaponTop)
		{
			View.WeaponY = DoomPlayerDetail::WeaponTop;
			Phase = EWeaponPhase::Ready;
		}
		break;
	case EWeaponPhase::Lowering:
		View.WeaponY += DoomPlayerDetail::RaiseSpeed;
		if (View.WeaponY >= DoomPlayerDetail::WeaponBottom)
		{
			View.WeaponY = DoomPlayerDetail::WeaponBottom;
			if (IsDead())
			{
				break;
			}
			if (PendingWeapon != EDoomWeapon::Count)
			{
				Inv().Ready = PendingWeapon;
				PendingWeapon = EDoomWeapon::Count;
			}
			SetWeaponSprite();
			Phase = EWeaponPhase::Raising;
			if (Inv().Ready == EDoomWeapon::Chainsaw)
			{
				PlaySelfSound(TEXT("SAWUP"));
			}
		}
		break;
	case EWeaponPhase::Ready:
		if (Inv().Ready == EDoomWeapon::Chainsaw)
		{
			View.WeaponFrame = ((DoomWorld->GetTic() / 4) & 1) ? 'D' : 'C';
			if ((DoomWorld->GetTic() % 8) == 0) PlaySelfSound(TEXT("SAWIDL"), 0.6f);
		}
		else
		{
			View.WeaponFrame = D.ReadyLetter();
		}
		if (IsDead() || PendingWeapon != EDoomWeapon::Count)
		{
			Phase = EWeaponPhase::Lowering;
			break;
		}
		if (bFireHeld && CheckAmmo())
		{
			bRefire = false;
			StartFire();
		}
		else if (PendingWeapon != EDoomWeapon::Count)
		{
			Phase = EWeaponPhase::Lowering;
		}
		break;
	case EWeaponPhase::Firing:
		if (--FrameTics > 0)
		{
			break;
		}
		++FireIndex;
		// A_ReFire runs on entering the last frame (when that frame doesn't fire itself), so with the
		// trigger held its tics are skipped: plasma refires every 3 tics, the pistol every 14
		if (FireIndex == D.Fire.Num() - 1 && !D.Fire[FireIndex].bFire &&
			bFireHeld && !IsDead() && PendingWeapon == EDoomWeapon::Count && CheckAmmo())
		{
			bRefire = true;
			StartFire();
			break;
		}
		if (FireIndex >= D.Fire.Num())
		{
			// A_ReFire
			if (bFireHeld && !IsDead() && PendingWeapon == EDoomWeapon::Count && CheckAmmo())
			{
				bRefire = true;
				StartFire();
			}
			else
			{
				bRefire = false;
				Phase = EWeaponPhase::Ready;
				View.WeaponFrame = D.ReadyLetter();
			}
			break;
		}
		{
			const FDoomWeaponFrame& F = D.Fire[FireIndex];
			View.WeaponFrame = F.Letter();
			FrameTics = FMath::Max(1, F.Tics);
			if (F.bFire)
			{
				FireWeapon(FireIndex);
			}
			else if (F.Flash >= 0)
			{
				View.FlashFrame = 'A' + F.Flash; // A_GunFlash on a frame of its own (the BFG's charge-up)
				FlashTics = FMath::Max(4, F.Tics);
			}
			if (Inv().Ready == EDoomWeapon::SuperShotgun)
			{
				if (F.Letter() == 'D') PlaySelfSound(TEXT("DBOPN"));
				if (F.Letter() == 'F') PlaySelfSound(TEXT("DBLOAD"));
				if (F.Letter() == 'H') PlaySelfSound(TEXT("DBCLS"));
			}
			if (F.Tics == 0)
			{
				FrameTics = 1;
			}
		}
		break;
	}
}

void ADoomPlayer::StartFire()
{
	const FDoomWeaponDef& D = DoomInfo::Weapon(Inv().Ready);
	Phase = EWeaponPhase::Firing;
	FireIndex = 0;
	const FDoomWeaponFrame& F = D.Fire[0];
	View.WeaponFrame = F.Letter();
	FrameTics = FMath::Max(1, F.Tics);
	if (Inv().Ready == EDoomWeapon::BFG)
	{
		PlaySelfSound(TEXT("BFG"));
	}
	if (F.bFire)
	{
		FireWeapon(0);
	}
	else if (F.Flash >= 0)
	{
		View.FlashFrame = 'A' + F.Flash;
		FlashTics = FMath::Max(4, F.Tics);
	}
}

void ADoomPlayer::FireWeapon(int32 FrameIndex)
{
	const FDoomWeaponDef& D = DoomInfo::Weapon(Inv().Ready);
	const FDoomWeaponFrame& F = D.Fire[FrameIndex];
	if (D.Ammo != EDoomAmmo::None)
	{
		if (Inv().Ammo[(int32)D.Ammo] < D.AmmoPerShot)
		{
			return;
		}
		Inv().Ammo[(int32)D.Ammo] -= D.AmmoPerShot;
	}
	if (F.Flash >= 0)
	{
		View.FlashFrame = 'A' + (Inv().Ready == EDoomWeapon::Plasma ? (DoomPlayerDetail::PlayerRand() & 1) : F.Flash);
		FlashTics = Inv().Ready == EDoomWeapon::Pistol ? 7 : 5;
		View.ExtraLight = (Inv().Ready == EDoomWeapon::Rocket || Inv().Ready == EDoomWeapon::BFG || Inv().Ready == EDoomWeapon::SuperShotgun) ? 2 : 1;
		if (DoomWorld->IsModern())
		{
			MuzzleLight->SetIntensity(Inv().Ready == EDoomWeapon::Plasma ? 30000.f : 60000.f);
			MuzzleLight->SetLightColor(Inv().Ready == EDoomWeapon::Plasma ? FLinearColor(0.4f, 0.6f, 1.f) :
				Inv().Ready == EDoomWeapon::BFG ? FLinearColor(0.4f, 1.f, 0.4f) : FLinearColor(1.f, 0.75f, 0.45f));
		}
	}
	View.AttackDown = 4;
	DoomWorld->NoiseAlert(this, GetActorLocation());
	if (!D.Sound.IsEmpty() && Inv().Ready != EDoomWeapon::Chainsaw && Inv().Ready != EDoomWeapon::Fist && Inv().Ready != EDoomWeapon::BFG)
	{
		PlaySelfSound(D.Sound);
	}

	const FVector Eye = GetActorLocation() + FVector(0, 0, 8.f * Doom::Scale);
	const FVector Fwd = FRotator(0, GetControlRotation().Yaw, 0).Vector();
	switch (Inv().Ready)
	{
	case EDoomWeapon::Fist: MeleeAttack(false); break;
	case EDoomWeapon::Chainsaw: MeleeAttack(true); break;
	case EDoomWeapon::Pistol:
	case EDoomWeapon::Chaingun: HitscanAttack(1, 5.6f, 0.f, 5, 3, !bRefire); break;
	case EDoomWeapon::Shotgun: HitscanAttack(7, 5.6f, 0.f, 5, 3, false); break;
	case EDoomWeapon::SuperShotgun: HitscanAttack(20, 11.2f, 7.f, 5, 3, false); break;
	case EDoomWeapon::Rocket:
	case EDoomWeapon::Plasma:
	case EDoomWeapon::BFG:
	{
		// P_SpawnPlayerMissile: straight ahead, or 5.6 degrees either side if that is where a target is;
		// launched 32 units above the feet
		AActor* AimTarget = nullptr;
		float AimAngle = DoomAngle();
		const float Slope = AimSlope(DoomAngle(), 1024.f, AimTarget, &AimAngle);
		const FVector Flat = FRotator(0, Doom::AngleToYaw(AimAngle), 0).Vector();
		const FVector Dir = (Flat + FVector(0, 0, Slope)).GetSafeNormal();
		const FVector Launch = GetActorLocation() + FVector(0, 0, 4.f * Doom::Scale);
		const TCHAR* Type = Inv().Ready == EDoomWeapon::Rocket ? TEXT("Rocket") : Inv().Ready == EDoomWeapon::Plasma ? TEXT("Plasma") : TEXT("BFGBall");
		// the BFG's sound already played when the trigger was pulled
		DoomWorld->SpawnProjectile(Type, this, Launch + Flat * 20.f * Doom::Scale, Dir, nullptr, Inv().Ready == EDoomWeapon::BFG);
		break;
	}
	default: break;
	}
}

float ADoomPlayer::AimSlope(float AngleDeg, float Range, AActor*& OutTarget, float* OutAngle) const
{
	// P_AimLineAttack: the nearest shootable thing the horizontal aim line passes through,
	// within Doom's vertical aiming window (100/160 up and down); else the view pitch.
	OutTarget = nullptr;
	const FVector Eye = GetActorLocation() + FVector(0, 0, 8.f * Doom::Scale);
	const FVector2f Origin = DoomPos();
	const UDoomSettings* Settings = GetDefault<UDoomSettings>();
	const float ViewSlope = FMath::Tan(FMath::DegreesToRadians(GetControlRotation().GetNormalized().Pitch));
	if (!Settings->bAutoAim)
	{
		return ViewSlope;
	}
	// whatever can be shot: monsters, barrels, and the other players (P_AimLineAttack takes any
	// MF_SHOOTABLE thing)
	TArray<TPair<AActor*, float>> Candidates;
	for (const TWeakObjectPtr<ADoomMonster>& M : DoomWorld->GetMonsters())
	{
		if (M.IsValid() && !M->IsDead()) Candidates.Add({ M.Get(), M->Def->Radius });
	}
	for (const TWeakObjectPtr<ADoomThing>& T : DoomWorld->GetThings())
	{
		if (T.IsValid() && !T->bDead && T->Def && (T->Def->Flags & EThingFlag::Shootable)) Candidates.Add({ T.Get(), T->Def->Radius });
	}
	for (ADoomPlayer* P : DoomWorld->GetPlayers())
	{
		if (P != this && !P->IsDead()) Candidates.Add({ P, 16.f });
	}
	for (float Offset : { 0.f, 5.6f, -5.6f })
	{
		const float A = FMath::DegreesToRadians(AngleDeg + Offset);
		const FVector2f Dir(FMath::Cos(A), FMath::Sin(A));
		float Best = Range;
		float BestSlope = 0;
		for (const TPair<AActor*, float>& C : Candidates)
		{
			const FVector2f To = Doom::ToDoom2D(C.Key->GetActorLocation()) - Origin;
			const float Along = FVector2f::DotProduct(To, Dir);
			if (Along <= 0 || Along > Best)
			{
				continue;
			}
			const float Side = FMath::Abs(To.X * Dir.Y - To.Y * Dir.X);
			if (Side > C.Value)
			{
				continue;
			}
			const float Dz = (C.Key->GetActorLocation().Z - Eye.Z) / Doom::Scale;
			const float Slope = Dz / FMath::Max(Along, 1.f);
			if (FMath::Abs(Slope) > 100.f / 160.f || !DoomWorld->CheckSight(this, C.Key))
			{
				continue;
			}
			Best = Along;
			BestSlope = Slope;
			OutTarget = C.Key;
		}
		if (OutTarget)
		{
			if (OutAngle) *OutAngle = AngleDeg + Offset;
			return BestSlope;
		}
	}
	return ViewSlope;
}

void ADoomPlayer::HitscanAttack(int32 Pellets, float SpreadYaw, float SpreadPitch, int32 DamageMul, int32 DamageDie, bool bAccurate)
{
	AActor* AimTarget = nullptr;
	const float Slope = AimSlope(DoomAngle(), 1024.f, AimTarget); // P_BulletSlope's range
	const FVector Eye = GetActorLocation() + FVector(0, 0, 8.f * Doom::Scale);
	const float Range = 2048.f * Doom::Scale;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(DoomPlayerShot), false, this);
	for (int32 i = 0; i < Pellets; ++i)
	{
		const float Yaw = GetControlRotation().Yaw + (bAccurate ? 0.f : (DoomPlayerDetail::PlayerRand() - DoomPlayerDetail::PlayerRand()) / 255.f * SpreadYaw);
		const float PelletSlope = Slope + (SpreadPitch > 0 ? FMath::Tan(FMath::DegreesToRadians((DoomPlayerDetail::PlayerRand() - DoomPlayerDetail::PlayerRand()) / 255.f * SpreadPitch)) : 0.f);
		const FVector Dir = (FRotator(0, Yaw, 0).Vector() + FVector(0, 0, PelletSlope)).GetSafeNormal();
		FHitResult Hit;
		const FVector End = Eye + Dir * Range;
		const bool bHit = GetWorld()->LineTraceSingleByChannel(Hit, Eye, End, ECC_Visibility, Q);
		// a few units past the hit: the hit point lies on the wall's line, and the crossing test needs
		// the ends on opposite sides (else shooting G1/GR switches came down to rounding)
		DoomWorld->CheckShootLines(this, Doom::ToDoom2D(Eye), Doom::ToDoom2D(bHit ? Hit.ImpactPoint + Dir * 4.f * Doom::Scale : End));
		if (!bHit)
		{
			continue;
		}
		const int32 Damage = DamageMul * ((DoomPlayerDetail::PlayerRand() % DamageDie) + 1);
		AActor* A = Hit.GetActor();
		if (A && (A->IsA<ADoomMonster>() || A->IsA<ADoomThing>() || A->IsA<ADoomPlayer>()))
		{
			UGameplayStatics::ApplyPointDamage(A, Damage, Dir, Hit, GetController(), this, nullptr);
			const ADoomMonster* M = Cast<ADoomMonster>(A);
			const bool bBlood = M && !(M->Def->Flags & EThingFlag::NoBlood);
			if (bBlood)
			{
				DoomWorld->SpawnEffect(TEXT("BLUD"), Damage > 12 ? TEXT("ABC") : Damage >= 9 ? TEXT("BC") : TEXT("C"), 8, Hit.ImpactPoint, false);
			}
			else
			{
				DoomWorld->SpawnEffect(TEXT("PUFF"), TEXT("ABCD"), 4, Hit.ImpactPoint - Dir * 4.f * Doom::Scale, true);
			}
			continue;
		}
		// no puffs on the sky
		const int32 Sec = DoomWorld->SectorAt(Hit.ImpactPoint - Dir * 2.f);
		if (Sec >= 0)
		{
			const FDoomSector& S = DoomWorld->GetMap()->Sectors[Sec];
			if (FDoomGraphics::IsSkyFlat(S.CeilingPic) && Hit.ImpactPoint.Z > (S.Ceiling - 4.f) * Doom::Scale)
			{
				continue;
			}
		}
		DoomWorld->SpawnEffect(TEXT("PUFF"), TEXT("ABCD"), 4, Hit.ImpactPoint - Dir * 4.f * Doom::Scale, true);
	}
}

void ADoomPlayer::MeleeAttack(bool bSaw)
{
	int32 Damage = 2 * ((DoomPlayerDetail::PlayerRand() % 10) + 1);
	if (!bSaw && View.bBerserk)
	{
		Damage *= 10;
	}
	AActor* AimTarget = nullptr;
	const float Range = bSaw ? 65.f : 64.f;
	const float Slope = AimSlope(DoomAngle(), Range, AimTarget);
	const float Yaw = GetControlRotation().Yaw + (DoomPlayerDetail::PlayerRand() - DoomPlayerDetail::PlayerRand()) / 255.f * 5.6f; // A_Punch and A_Saw both spread
	const FVector Eye = GetActorLocation() + FVector(0, 0, 8.f * Doom::Scale);
	const FVector Dir = (FRotator(0, Yaw, 0).Vector() + FVector(0, 0, Slope)).GetSafeNormal();
	FCollisionQueryParams Q(SCENE_QUERY_STAT(DoomMelee), false, this);
	FHitResult Hit;
	const bool bHit = GetWorld()->SweepSingleByChannel(Hit, Eye, Eye + Dir * Range * Doom::Scale, FQuat::Identity, ECC_Visibility,
		FCollisionShape::MakeSphere(4.f * Doom::Scale), Q);
	if (!bHit)
	{
		if (bSaw) PlaySelfSound(TEXT("SAWFUL"));
		return;
	}
	AActor* A = Hit.GetActor();
	if (A && (A->IsA<ADoomMonster>() || A->IsA<ADoomThing>()))
	{
		UGameplayStatics::ApplyPointDamage(A, Damage, Dir, Hit, GetController(), this, nullptr);
		DoomWorld->SpawnEffect(TEXT("BLUD"), TEXT("BC"), 8, Hit.ImpactPoint, false);
		PlaySelfSound(bSaw ? TEXT("SAWHIT") : TEXT("PUNCH"));
		// a punch turns you to face what it hit; the saw pulls you round 4.5 degrees per hit (ANG90/20)
		if (AController* C = GetController())
		{
			FRotator R = C->GetControlRotation();
			const float Want = (A->GetActorLocation() - GetActorLocation()).Rotation().Yaw;
			R.Yaw = bSaw ? R.Yaw + FMath::Clamp(FMath::FindDeltaAngleDegrees(R.Yaw, Want), -4.5f, 4.5f) : Want;
			C->SetControlRotation(R);
		}
	}
	else
	{
		DoomWorld->SpawnEffect(TEXT("PUFF"), TEXT("ABCD"), 4, Hit.ImpactPoint - Dir * 4.f * Doom::Scale, true);
		if (bSaw) PlaySelfSound(TEXT("SAWFUL"));
	}
}
