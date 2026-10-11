#include "DoomProjectile.h"
#include "Net/UnrealNetwork.h"
#include "DoomWorld.h"
#include "DoomMonster.h"
#include "DoomPlayer.h"
#include "DoomThing.h"
#include "DoomLevel.h"
#include "DoomInfo.h"
#include "DoomGraphics.h"
#include "DoomSpriteComponent.h"
#include "DoomUE.h"
#include "Components/SphereComponent.h"
#include "Components/PointLightComponent.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Kismet/GameplayStatics.h"

namespace
{
	FString ExplosionSprite(const FString& Fly)
	{
		if (Fly == TEXT("PLSS")) return TEXT("PLSE");
		if (Fly == TEXT("BFS1")) return TEXT("BFE1");
		if (Fly == TEXT("APLS")) return TEXT("APBX");
		if (Fly == TEXT("FATB")) return TEXT("FBXP");
		if (Fly == TEXT("MANF")) return TEXT("MISL");
		return Fly;
	}
}

ADoomProjectile::ADoomProjectile()
{
	PrimaryActorTick.bCanEverTick = false;
	Sphere = CreateDefaultSubobject<USphereComponent>(TEXT("Sphere"));
	Sphere->SetCollisionObjectType(ECC_DoomMissile);
	Sphere->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Sphere->SetCollisionResponseToAllChannels(ECR_Block);
	Sphere->SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore);
	Sphere->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
	Sphere->SetCollisionResponseToChannel(ECC_DoomMissile, ECR_Ignore);
	Sphere->SetCanEverAffectNavigation(false);
	RootComponent = Sphere;

	Sprite = CreateDefaultSubobject<UDoomSpriteComponent>(TEXT("Sprite"));
	Sprite->SetupAttachment(Sphere);

	Movement = CreateDefaultSubobject<UProjectileMovementComponent>(TEXT("Movement"));
	Movement->UpdatedComponent = Sphere;
	Movement->ProjectileGravityScale = 0.f;
	Movement->bRotationFollowsVelocity = true;
	Movement->bShouldBounce = false;
	Movement->bSweepCollision = true;
	bReplicates = true;
	SetReplicatingMovement(true);
}

void ADoomProjectile::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ADoomProjectile, NetType);
}

void ADoomProjectile::OnRep_NetType()
{
	UDoomWorld* W = GetWorld()->GetSubsystem<UDoomWorld>();
	const FDoomProjectileDef* D = DoomInfo::FindProjectile(NetType);
	if (!W || !D || !W->EnsureWads())
	{
		return;
	}
	// a client's copy: no simulation of its own, just the server's position, sprite and light
	DoomWorld = W;
	Def = D;
	Movement->StopMovementImmediately();
	Movement->SetUpdatedComponent(nullptr);
	Movement->Deactivate();
	Sphere->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Sprite->Init(DoomWorld, Def->Sprite);
	if (DoomWorld->IsModern())
	{
		Light = NewObject<UPointLightComponent>(this, TEXT("Light"));
		Light->SetupAttachment(Sphere);
		Light->SetLightColor(Def->Light);
		Light->SetIntensity(8000.f);
		Light->SetAttenuationRadius(200.f * Doom::Scale);
		Light->SetCastShadows(true);
		Light->RegisterComponent();
	}
}

void ADoomProjectile::InitProjectile(UDoomWorld* InWorld, const FDoomProjectileDef* InDef, AActor* InSource, const FVector& Direction, AActor* InTracer)
{
	DoomWorld = InWorld;
	Def = InDef;
	NetType = Def->Name;
	Source = InSource;
	Tracer = InTracer;
	const float S = Doom::Scale;
	Sphere->SetSphereRadius(Def->Radius * S * 0.6f);
	if (InSource)
	{
		Sphere->IgnoreActorWhenMoving(InSource, true);
	}
	float Speed = Def->Speed;
	if (DoomWorld->GetSkill() >= 5 && InSource && !InSource->IsA<ADoomPlayer>())
	{
		Speed *= 1.6f; // nightmare's fast missiles
	}
	Movement->InitialSpeed = Movement->MaxSpeed = Speed * Doom::TicRate * S;
	Movement->Velocity = Direction * Movement->InitialSpeed;
	Movement->OnProjectileStop.AddDynamic(this, &ADoomProjectile::OnStop);
	if (Def->bHoming && InTracer)
	{
		Movement->bIsHomingProjectile = true;
		Movement->HomingTargetComponent = InTracer->GetRootComponent();
		Movement->HomingAccelerationMagnitude = 2500.f * S;
	}

	Sprite->Init(DoomWorld, Def->Sprite);
	Sprite->DoomAngle = FMath::RadiansToDegrees(FMath::Atan2(-Direction.Y, Direction.X));
	Sprite->PlaySequence(Def->Fly, 4, true, true);
	Sprite->SetSector(DoomWorld->SectorAt(GetActorLocation()));

	if (DoomWorld->IsModern())
	{
		Light = NewObject<UPointLightComponent>(this, TEXT("Light"));
		Light->SetupAttachment(Sphere);
		Light->SetLightColor(Def->Light);
		Light->SetIntensity(8000.f);
		Light->SetAttenuationRadius(200.f * S);
		Light->SetCastShadows(true);
		Light->RegisterComponent();
	}
}

void ADoomProjectile::DoomTic()
{
	Sprite->AnimateTic();
	if (bExploded)
	{
		if (Light)
		{
			Light->SetIntensity(Light->Intensity * 0.8f);
		}
		if (Def && FCString::Strcmp(Def->Name, TEXT("BFGBall")) == 0 && Sprite->FrameIndex() == 2 && Age >= 0)
		{
			Age = -1000;
			BfgSpray();
		}
		if (Sprite->IsSequenceDone())
		{
			Destroy();
		}
		return;
	}
	Sprite->SetSector(DoomWorld->SectorAt(GetActorLocation()));
	if (++Age > 20 * Doom::TicRate)
	{
		Destroy();
	}
	if (Tracer.IsValid() && (Age & 3) == 0 && Def->bHoming)
	{
		DoomWorld->SpawnEffect(TEXT("PUFF"), TEXT("BCD"), 4, GetActorLocation(), true);
	}
}

void ADoomProjectile::DoomSerialize(FArchive& Ar)
{
	FVector Velocity = Movement->Velocity;
	Ar << Age << Velocity;
	if (Ar.IsLoading())
	{
		Movement->Velocity = Velocity;
		Sprite->DoomAngle = FMath::RadiansToDegrees(FMath::Atan2(-Velocity.Y, Velocity.X));
	}
	Sprite->DoomSerialize(Ar);
}

void ADoomProjectile::OnStop(const FHitResult& Hit)
{
	if (bExploded || !HasAuthority())
	{
		return;
	}
	// missiles that hit the sky vanish (Doom's sky hack)
	const int32 Sec = DoomWorld->SectorAt(Hit.ImpactPoint);
	if (Sec >= 0 && !Hit.GetActor()->IsA<ADoomMonster>() && !Hit.GetActor()->IsA<ADoomPlayer>())
	{
		const FDoomSector& S = DoomWorld->GetMap()->Sectors[Sec];
		if (FDoomGraphics::IsSkyFlat(S.CeilingPic) && Hit.ImpactPoint.Z >= (S.Ceiling - 2.f) * Doom::Scale)
		{
			Destroy();
			return;
		}
	}
	Explode(Hit.GetActor());
}

void ADoomProjectile::Explode(AActor* HitActor)
{
	bExploded = true;
	const FVector Incoming = Movement->Velocity;
	Movement->StopMovementImmediately();
	Movement->SetUpdatedComponent(nullptr);
	Sphere->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	const FVector L = GetActorLocation();
	AActor* Credit = Source.IsValid() ? Source.Get() : this;

	if (HitActor && HitActor != Source.Get() && (HitActor->IsA<ADoomMonster>() || HitActor->IsA<ADoomPlayer>() || HitActor->IsA<ADoomThing>()))
	{
		const int32 Damage = ((DoomInfo::Random() % 8) + 1) * Def->DamageMul;
		FHitResult H;
		UGameplayStatics::ApplyPointDamage(HitActor, Damage, GetVelocity().GetSafeNormal(), H, nullptr, this, nullptr);
	}
	if (Def->SplashDamage > 0)
	{
		DoomWorld->RadiusAttack(this, Credit, L, Def->SplashDamage);
	}
	DoomWorld->PlaySound(Def->HitSound, &L);
	// modern renderer: a particle burst coloured like the missile's light, away from what it hit
	const FLinearColor& C = Def->Light;
	const EDoomParticles Burst = (C.G > C.R && C.G > C.B) ? EDoomParticles::BFG : (C.B > C.R) ? EDoomParticles::Plasma : EDoomParticles::Explosion;
	DoomWorld->SpawnParticles(Burst, L, (-Incoming).GetSafeNormal(1e-4f, FVector::UpVector));
	Sprite->SetSpritePrefix(ExplosionSprite(Def->Sprite));
	Sprite->PlaySequence(Def->Explode, FCString::Strcmp(Def->Name, TEXT("BFGBall")) == 0 ? 8 : 6, false, true);
	if (Light)
	{
		Light->SetIntensity(Light->Intensity * 3.f);
	}
}

void ADoomProjectile::BfgSpray()
{
	// 40 invisible tracers fanned over 90 degrees from the shooter's facing
	AActor* Shooter = Source.Get();
	if (!Shooter)
	{
		return;
	}
	const FVector From = Shooter->GetActorLocation();
	const float Yaw0 = Shooter->IsA<ADoomPlayer>() ? Cast<ADoomPlayer>(Shooter)->GetControlRotation().Yaw : Shooter->GetActorRotation().Yaw;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(DoomBfg), false, Shooter);
	for (int32 i = 0; i < 40; ++i)
	{
		const float Yaw = Yaw0 - 45.f + 90.f / 40.f * i;
		const FVector Dir = FRotator(0, Yaw, 0).Vector();
		FHitResult Hit;
		if (!GetWorld()->LineTraceSingleByChannel(Hit, From, From + Dir * 1024.f * Doom::Scale, ECC_Visibility, Q))
		{
			continue;
		}
		if (Hit.GetActor() && Hit.GetActor()->IsA<ADoomMonster>())
		{
			int32 Damage = 0;
			for (int32 k = 0; k < 15; ++k) Damage += (DoomInfo::Random() & 7) + 1;
			DoomWorld->SpawnEffect(TEXT("BFE2"), TEXT("ABCD"), 8, Hit.GetActor()->GetActorLocation(), true);
			UGameplayStatics::ApplyPointDamage(Hit.GetActor(), Damage, Dir, Hit, nullptr, Shooter, nullptr);
		}
	}
}
