#include "DoomThing.h"
#include "Net/UnrealNetwork.h"
#include "DoomWorld.h"
#include "DoomPlayer.h"
#include "DoomSpriteComponent.h"
#include "DoomUE.h"
#include "Components/CapsuleComponent.h"
#include "Components/PointLightComponent.h"
#include "Engine/DamageEvents.h"
#include "GameFramework/CharacterMovementComponent.h"

ADoomThing::ADoomThing()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false; // only network clients' effects tick
	Capsule = CreateDefaultSubobject<UCapsuleComponent>(TEXT("Capsule"));
	Capsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Capsule->SetCanEverAffectNavigation(false);
	RootComponent = Capsule;
	Sprite = CreateDefaultSubobject<UDoomSpriteComponent>(TEXT("Sprite"));
	Sprite->SetupAttachment(Capsule);
}

void ADoomThing::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ADoomThing, NetType);
	DOREPLIFETIME(ADoomThing, bNetDropped);
	DOREPLIFETIME(ADoomThing, bNetDead);
}

void ADoomThing::OnRep_NetType()
{
	UDoomWorld* W = GetWorld()->GetSubsystem<UDoomWorld>();
	const FDoomThingDef* D = DoomInfo::FindThing(NetType);
	if (W && D && W->EnsureWads())
	{
		InitThing(W, D, Sprite->DoomAngle, bNetDropped);
	}
}

void ADoomThing::OnRep_NetDead()
{
	if (bNetDead)
	{
		bDead = true;
		Capsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
}

void ADoomThing::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	// a network client's own effects (puffs, blood) end with their animation
	if (bEffect && Sprite->IsSequenceDone())
	{
		Destroy();
	}
}

void ADoomThing::InitThing(UDoomWorld* InWorld, const FDoomThingDef* InDef, float InDoomAngle, bool bInDropped)
{
	DoomWorld = InWorld;
	Def = InDef;
	bDropped = bInDropped;
	if (HasAuthority() && DoomWorld->IsNetServer())
	{
		SetReplicates(true);
		SetReplicateMovement(true); // things ride lifts and fall
		NetType = Def->DoomEdNum;
		bNetDropped = bDropped;
	}
	Health = Def->Health;
	const float S = Doom::Scale;
	Capsule->SetCapsuleSize(Def->Radius * S, FMath::Max(Def->Height * 0.5f, Def->Radius) * S);

	if (Def->Flags & EThingFlag::Pickup)
	{
		Capsule->SetCollisionProfileName(TEXT("OverlapAllDynamic"));
		Capsule->SetGenerateOverlapEvents(true);
		Capsule->OnComponentBeginOverlap.AddDynamic(this, &ADoomThing::OnOverlap);
	}
	else if (Def->Flags & EThingFlag::Solid)
	{
		Capsule->SetCollisionProfileName(TEXT("BlockAllDynamic"));
		// Doom's bullets pass decorations; only shootable things (barrels) stop them
		Capsule->SetCollisionResponseToChannel(ECC_Visibility, (Def->Flags & EThingFlag::Shootable) ? ECR_Block : ECR_Ignore);
		Capsule->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
	}

	Sprite->Init(DoomWorld, Def->Sprite);
	Sprite->DoomAngle = InDoomAngle;
	Sprite->PlaySequence(Def->Idle, Def->IdleTics, true, (Def->Flags & EThingFlag::FullBright) || Def->bFullBrightIdle);
	SnapToPlanes();

	const bool bGlows = Def->LightColor.A > 0 || (Def->Flags & EThingFlag::FullBright);
	if (DoomWorld->IsModern() && bGlows)
	{
		Light = NewObject<UPointLightComponent>(this, TEXT("Light"));
		Light->SetupAttachment(Capsule);
		Light->SetRelativeLocation(FVector(0, 0, Def->Height * 0.3f * S));
		Light->SetLightColor(Def->LightColor.A > 0 ? Def->LightColor : FLinearColor(0.6f, 0.7f, 1.f));
		Light->SetIntensity(Def->LightColor.A > 0 ? 3000.f : 800.f);
		Light->SetAttenuationRadius(220.f * S);
		Light->SetCastShadows(Def->LightColor.A > 0);
		Light->SetSourceRadius(4.f * S);
		Light->RegisterComponent();
	}
}

void ADoomThing::InitEffect(UDoomWorld* InWorld, const FString& SpriteName, const FString& Frames, int32 Tics, bool bFullBright, bool bLoop)
{
	DoomWorld = InWorld;
	bEffect = true;
	if (DoomWorld->IsNetClient())
	{
		SetActorTickEnabled(true); // no game tics here: Tick removes the effect when it ends
	}
	Capsule->SetCapsuleSize(1, 1);
	Sprite->Init(DoomWorld, SpriteName);
	Sprite->PlaySequence(Frames, Tics, bLoop, bFullBright);
	Sprite->SetSector(DoomWorld->SectorAt(GetActorLocation()));
	if (DoomWorld->IsModern() && bFullBright)
	{
		Light = NewObject<UPointLightComponent>(this, TEXT("Light"));
		Light->SetupAttachment(Capsule);
		Light->SetLightColor(SpriteName == TEXT("TFOG") ? FLinearColor(0.4f, 1.f, 0.4f) : FLinearColor(1.f, 0.6f, 0.25f));
		Light->SetIntensity(5000.f);
		Light->SetAttenuationRadius(160.f * Doom::Scale);
		Light->SetCastShadows(false);
		Light->RegisterComponent();
	}
}

void ADoomThing::SnapToPlanes()
{
	if (!Def || !DoomWorld || !DoomWorld->GetMap())
	{
		return;
	}
	const FVector P = GetActorLocation();
	const float Half = Capsule->GetScaledCapsuleHalfHeight();
	FVector N = P;
	if (Def->Flags & EThingFlag::Hanging)
	{
		N.Z = DoomWorld->CeilingAt(P) * Doom::Scale - Def->Height * Doom::Scale + Half;
	}
	else
	{
		N.Z = DoomWorld->FloorAt(P) * Doom::Scale + Half;
	}
	if (!N.Equals(P, 0.1f))
	{
		SetActorLocation(N);
	}
	Sprite->SetSector(DoomWorld->SectorAt(N));
}

void ADoomThing::DoomTic()
{
	Sprite->AnimateTic();
	if (bEffect)
	{
		if (Light)
		{
			Light->SetIntensity(Light->Intensity * 0.85f);
		}
		if (Sprite->IsSequenceDone())
		{
			Destroy();
		}
		return;
	}
	if (bDead && Def && (Def->Flags & EThingFlag::Barrel))
	{
		// BEXP: the blast goes off on the third frame
		if (Sprite->FrameIndex() == 2 && ExplodeDelay < 0)
		{
			ExplodeDelay = 0;
			AActor* Credit = LastAttacker.IsValid() ? LastAttacker.Get() : this;
			DoomWorld->RadiusAttack(this, Credit, GetActorLocation(), 128.f);
			DoomWorld->SpawnParticles(EDoomParticles::Explosion, GetActorLocation());
		}
		if (Sprite->IsSequenceDone())
		{
			Destroy();
		}
		return;
	}
	if (Def && (Def->Flags & EThingFlag::Pickup) && (DoomWorld->GetTic() & 3) == 0)
	{
		// standing on an item while full: retry like Doom's per-move touch checks
		for (ADoomPlayer* P : DoomWorld->GetPlayers())
		{
			if (Capsule->IsOverlappingActor(P))
			{
				FHitResult Dummy;
				OnOverlap(Capsule, P, nullptr, 0, false, Dummy);
				if (IsActorBeingDestroyed()) break;
			}
		}
	}
}

void ADoomThing::OnOverlap(UPrimitiveComponent* OverlappedComp, AActor* Other, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& Sweep)
{
	ADoomPlayer* P = Cast<ADoomPlayer>(Other);
	if (!P || P->IsDead() || !Def || IsActorBeingDestroyed() || !HasAuthority())
	{
		return; // pickups happen on the server
	}
	// deathmatch: weapons stay for the next player, and you cannot take one you already carry
	const bool bWeaponStays = DoomWorld->IsDeathmatch() && !bDropped && Def->Pickup >= EPickup::Chainsaw;
	if (bWeaponStays)
	{
		static const TMap<EPickup, EDoomWeapon> Weapons = {
			{ EPickup::Chainsaw, EDoomWeapon::Chainsaw }, { EPickup::Shotgun, EDoomWeapon::Shotgun },
			{ EPickup::SuperShotgun, EDoomWeapon::SuperShotgun }, { EPickup::Chaingun, EDoomWeapon::Chaingun },
			{ EPickup::RocketLauncher, EDoomWeapon::Rocket }, { EPickup::Plasma, EDoomWeapon::Plasma }, { EPickup::BFG, EDoomWeapon::BFG } };
		const EDoomWeapon* W = Weapons.Find(Def->Pickup);
		if (W && P->Inv().Weapons[(int32)*W])
		{
			return;
		}
	}
	if (P->TryPickup(*Def, bDropped))
	{
		if (Def->Flags & EThingFlag::CountItem)
		{
			DoomWorld->CreditItem(P);
		}
		if (!bWeaponStays)
		{
			Destroy();
		}
	}
}

void ADoomThing::DoomSerialize(FArchive& Ar)
{
	Ar << Health << bDropped;
	Sprite->DoomSerialize(Ar);
}

float ADoomThing::TakeDamage(float Damage, FDamageEvent const& DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
	if (!Def || !(Def->Flags & EThingFlag::Shootable) || bDead)
	{
		return 0.f;
	}
	Health -= FMath::RoundToInt(Damage);
	if (DamageCauser)
	{
		LastAttacker = DamageCauser->GetOwner() ? DamageCauser->GetOwner() : DamageCauser;
	}
	if (Health <= 0)
	{
		bDead = true;
		bNetDead = true;
		Capsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Sprite->SetSpritePrefix(TEXT("BEXP"));
		Sprite->PlaySequence(Def->Death, 5, false, true);
		const FVector L = GetActorLocation();
		DoomWorld->PlaySound(Def->DeathSound, &L);
		if (DoomWorld->IsModern() && !Light)
		{
			Light = NewObject<UPointLightComponent>(this, TEXT("Light"));
			Light->SetupAttachment(Capsule);
			Light->SetLightColor(FLinearColor(1.f, 0.55f, 0.2f));
			Light->SetIntensity(20000.f);
			Light->SetAttenuationRadius(300.f * Doom::Scale);
			Light->RegisterComponent();
		}
	}
	return Damage;
}
