#include "DoomSpriteComponent.h"
#include "DoomWorld.h"
#include "DoomGraphics.h"
#include "DoomAssets.h"
#include "DoomLevel.h"
#include "DoomUE.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Kismet/GameplayStatics.h"
#include "Camera/PlayerCameraManager.h"

UDoomSpriteComponent::UDoomSpriteComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetCastShadow(false);
	bUseAsyncCooking = false;
	SetUsingAbsoluteLocation(true);
	SetUsingAbsoluteRotation(true);
	SetUsingAbsoluteScale(true);
}

void UDoomSpriteComponent::Init(UDoomWorld* InWorld, const FString& InSprite)
{
	DoomWorld = InWorld;
	Sprite = InSprite;

	// unit quad: Y right 0..1, Z up 0..1, facing -X (towards the viewer once rotated)
	const TArray<FVector> V = { FVector(0, 0, 0), FVector(0, 0, 1), FVector(0, 1, 1), FVector(0, 1, 0) };
	const TArray<int32> I = { 0, 1, 2, 0, 2, 3 };
	const TArray<FVector> N = { FVector(-1, 0, 0), FVector(-1, 0, 0), FVector(-1, 0, 0), FVector(-1, 0, 0) };
	const TArray<FVector2D> UV = { FVector2D(0, 1), FVector2D(0, 0), FVector2D(1, 0), FVector2D(1, 1) };
	const TArray<FLinearColor> C = { FLinearColor::White, FLinearColor::White, FLinearColor::White, FLinearColor::White };
	CreateMeshSection_LinearColor(0, V, I, N, UV, C, {}, false);

	MID = UMaterialInstanceDynamic::Create(DoomAssets::SpriteMaterial(), this);
	if (ADoomLevel* Level = DoomWorld ? DoomWorld->GetLevel() : nullptr)
	{
		MID->SetTextureParameterValue(TEXT("SectorLights"), Level->GetSectorLightTexture());
		MID->SetScalarParameterValue(TEXT("SectorLightRows"), FMath::Max(1, FMath::DivideAndRoundUp(DoomWorld->GetMap()->Sectors.Num(), 256)));
	}
	SetMaterial(0, MID);
	// generous bounds: the quad is resized every frame
	SetBoundsScale(4.f);
	bDirty = true;
}

void UDoomSpriteComponent::PlaySequence(const FString& Frames, int32 Tics, bool bInLoop, bool bFullBright)
{
	Seq = Frames.IsEmpty() ? FString(TEXT("A")) : Frames;
	Index = 0;
	TicsPerFrame = FMath::Max(1, Tics);
	TicCount = 0;
	bLoop = bInLoop;
	bDone = false;
	bDirty = true;
	SetFullBright(bFullBright);
}

bool UDoomSpriteComponent::AnimateTic()
{
	if (bDone)
	{
		return false;
	}
	if (++TicCount < TicsPerFrame)
	{
		return false;
	}
	TicCount = 0;
	if (Index + 1 < Seq.Len())
	{
		++Index;
	}
	else if (bLoop)
	{
		Index = 0;
	}
	else
	{
		bDone = true;
		return false;
	}
	bDirty = true;
	return true;
}

void UDoomSpriteComponent::SetSector(int32 InSector)
{
	if (InSector != Sector && MID)
	{
		Sector = InSector;
		MID->SetScalarParameterValue(TEXT("Sector"), FMath::Max(0, Sector));
	}
}

void UDoomSpriteComponent::SetFullBright(bool b)
{
	if (MID)
	{
		MID->SetScalarParameterValue(TEXT("FullBright"), b ? 1.f : 0.f);
	}
}

void UDoomSpriteComponent::SetFuzz(bool b)
{
	if (MID)
	{
		MID->SetScalarParameterValue(TEXT("Fuzz"), b ? 1.f : 0.f);
	}
}

void UDoomSpriteComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	APlayerCameraManager* Cam = UGameplayStatics::GetPlayerCameraManager(this, 0);
	if (Cam)
	{
		UpdateVisual(Cam->GetCameraLocation());
	}
}

void UDoomSpriteComponent::UpdateVisual(const FVector& ViewLocation)
{
	if (!DoomWorld || !DoomWorld->GetGraphics() || !GetOwner())
	{
		return;
	}
	const FVector Feet = GetOwner()->GetActorLocation() - FVector(0, 0, GetOwner()->GetSimpleCollisionHalfHeight());
	const FVector ToThing = Feet - ViewLocation;

	// rotation: Doom's (angle_to_thing - thing_angle + 202.5) / 45
	const float AngToThing = FMath::RadiansToDegrees(FMath::Atan2(-ToThing.Y, ToThing.X));
	const float Rel = FMath::Fmod(AngToThing - DoomAngle + 202.5f + 720.f, 360.f);
	const int32 Rotation = FMath::Clamp((int32)(Rel / 45.f), 0, 7) + 1;

	const FDoomSpriteFrame F = DoomWorld->GetGraphics()->SpriteFrame(Sprite, CurrentFrame(), Rotation);
	if (!F.Picture)
	{
		SetVisibility(false);
		return;
	}
	SetVisibility(true);
	if (F.Picture != LastPicture || F.bMirror != bLastMirror)
	{
		LastPicture = F.Picture;
		bLastMirror = F.bMirror;
		MID->SetTextureParameterValue(TEXT("Tex"), F.Picture->Texture);
		MID->SetScalarParameterValue(TEXT("Mirror"), F.bMirror ? 1.f : 0.f);
	}

	// face the camera (Doom's sprites are screen-aligned around the vertical axis)
	const float Yaw = FMath::RadiansToDegrees(FMath::Atan2(ToThing.Y, ToThing.X));
	const FRotator Rot(0, Yaw, 0);
	const FVector Right = FRotationMatrix(Rot).GetUnitAxis(EAxis::Y);
	const float S = Doom::Scale;
	const float Left = F.bMirror ? (F.Picture->Width - F.Picture->LeftOffset) : F.Picture->LeftOffset;
	const FVector Origin = Feet - Right * (Left * S) + FVector(0, 0, (F.Picture->TopOffset - F.Picture->Height) * S);
	SetWorldLocationAndRotation(Origin, Rot);
	SetWorldScale3D(FVector(1, F.Picture->Width * S, F.Picture->Height * S));
	bDirty = false;
}
